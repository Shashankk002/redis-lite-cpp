#include "server.hpp"

#include <fcntl.h>          // fcntl, O_NONBLOCK
#include <netinet/in.h>     // sockaddr_in, htons, htonl
#include <sys/event.h>      // kqueue, kevent, EV_SET
#include <sys/socket.h>     // socket, bind, listen, accept, send, recv
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "commands.hpp"
#include "persistence.hpp"
#include "resp.hpp"

namespace redis_lite {

    namespace {

        using ClientBuffers = std::unordered_map<int, std::string>;

        // Most a client may have buffered while a request is still incomplete.
        // Without it, an unfinished request grows the buffer without bound.
        constexpr std::size_t kMaxRequestBytes = 16 * 1024 * 1024;

        enum class AfterRead {
            KeepOpen,
            CloseWhenSent,  // the peer half-closed: reply first, then close
            CloseNow,       // broken, or the byte stream is out of sync
        };

        volatile sig_atomic_t g_running = 1;

        void handle_shutdown_signal(int /*signal_number*/) {
            g_running = 0;
        }

        void install_signal_handlers() {
            struct sigaction action{};
            action.sa_handler = handle_shutdown_signal;
            action.sa_flags = 0;
            sigemptyset(&action.sa_mask);
            sigaction(SIGINT, &action, nullptr);
            sigaction(SIGTERM, &action, nullptr);
        }

        bool set_non_blocking(int fd) {
            const int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0) {
                perror("fcntl(F_GETFL)");
                return false;
            }
            if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                perror("fcntl(F_SETFL)");
                return false;
            }
            return true;
        }

        bool update_event(int kq, int fd, int16_t filter, uint16_t flags) {
            struct kevent change;
            EV_SET(&change, static_cast<uintptr_t>(fd), filter, flags, 0, 0, nullptr);

            if (kevent(kq, &change, 1, nullptr, 0, nullptr) < 0) {
                // Removing a filter that was never registered is harmless.
                if ((flags & EV_DELETE) != 0 && errno == ENOENT) {
                    return true;
                }
                perror("kevent(register)");
                return false;
            }
            return true;
        }

        void close_client(int kq,
                          int fd,
                          ClientBuffers& input,
                          ClientBuffers& output,
                          std::unordered_set<int>& closing) {
            update_event(kq, fd, EVFILT_READ, EV_DELETE);
            update_event(kq, fd, EVFILT_WRITE, EV_DELETE);

            close(fd);
            input.erase(fd);
            output.erase(fd);
            closing.erase(fd);

            std::cout << ("client closed (fd " + std::to_string(fd) + ")\n");
        }

        bool try_send(int kq, int fd, ClientBuffers& output) {
            std::string& pending = output[fd];

            while (!pending.empty()) {
                const ssize_t sent = send(fd, pending.data(), pending.size(), 0);

                if (sent > 0) {
                    pending.erase(0, static_cast<std::size_t>(sent));
                    continue;
                }
                if (sent < 0 && errno == EINTR) {
                    continue;
                }
                if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    // The kernel's send buffer is full. Rather than spinning on
                    // send(), ask kqueue to wake us when there is room again.
                    update_event(kq, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE);
                    return true;
                }
                perror("send");
                return false;
            }
            return true;
        }

        void accept_clients(int kq, int listen_fd, ClientBuffers& input, ClientBuffers& output) {
            while (true) {
                const int client_fd = accept(listen_fd, nullptr, nullptr);

                if (client_fd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        return;  // no more waiting connections
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    perror("accept");
                    return;
                }

                if (!set_non_blocking(client_fd)) {
                    close(client_fd);
                    continue;
                }
                if (!update_event(kq, client_fd, EVFILT_READ, EV_ADD | EV_ENABLE)) {
                    close(client_fd);
                    continue;
                }

                input[client_fd].clear();
                output[client_fd].clear();

                std::cout << ("client connected (fd " + std::to_string(client_fd) + ")\n");
            }
        }

        AfterRead handle_readable(int kq,
                                  int fd,
                                  Store& store,
                                  std::ofstream& log,
                                  ClientBuffers& input,
                                  ClientBuffers& output) {
            char chunk[4096];
            bool peer_finished = false;
            std::string& buffer = input[fd];

            while (true) {
                const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);

                if (received > 0) {
                    if (buffer.size() + static_cast<std::size_t>(received) > kMaxRequestBytes) {
                        output[fd] += serialize(make_error(
                            "ERR Protocol error: request larger than " +
                            std::to_string(kMaxRequestBytes) + " bytes"));
                        std::cout << ("request too large (fd " + std::to_string(fd) + ")\n");
                        try_send(kq, fd, output);
                        return AfterRead::CloseNow;
                    }
                    buffer.append(chunk, static_cast<std::size_t>(received));
                    continue;
                }
                if (received == 0) {
                    std::cout << ("client finished sending (fd " + std::to_string(fd) + ")\n");
                    peer_finished = true;
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;  // nothing more to read for now
                }
                perror("recv");
                return AfterRead::CloseNow;
            }

            while (true) {
                const ParseResult result = parse(buffer);

                if (result.status == ParseStatus::Incomplete) {
                    break;  // wait for the rest to arrive
                }

                if (result.status == ParseStatus::Malformed) {
                    output[fd] += serialize(make_error("ERR Protocol error: " + result.error));
                    std::cout << ("protocol error (fd " + std::to_string(fd) + "): " +
                                  result.error + "\n");
                    try_send(kq, fd, output);
                    return AfterRead::CloseNow;  // out of sync; close, as Redis does
                }

                const RespValue reply = execute_command(result.value, store, &log);
                output[fd] += serialize(reply);

                buffer.erase(0, result.consumed);
            }

            if (!output[fd].empty() && !try_send(kq, fd, output)) {
                return AfterRead::CloseNow;
            }
            return peer_finished ? AfterRead::CloseWhenSent : AfterRead::KeepOpen;
        }

    }  // namespace

    int run_server(uint16_t port, const std::string& log_path) {
        install_signal_handlers();

        Store store;
        std::string replay_error;
        if (!replay_log(log_path, store, replay_error)) {
            std::cerr << "cannot start: " << replay_error << "\n";
            std::cerr << "fix or remove " << log_path << " and try again\n";
            return 1;
        }
        std::cout << "loaded " << store.values.size() << " key(s) from " << log_path
                  << std::endl;  // endl flushes: a startup line is useless if unseen

        std::ofstream log;
        if (!open_log(log_path, log)) {
            std::cerr << "cannot open " << log_path << " for appending\n";
            return 1;
        }

        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            perror("socket");
            return 1;
        }

        int reuse = 1;
        if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
            perror("setsockopt");
            close(listen_fd);
            return 1;
        }

        if (!set_non_blocking(listen_fd)) {
            close(listen_fd);
            return 1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            perror("bind");
            close(listen_fd);
            return 1;
        }

        if (listen(listen_fd, SOMAXCONN) < 0) {
            perror("listen");
            close(listen_fd);
            return 1;
        }

        const int kq = kqueue();
        if (kq < 0) {
            perror("kqueue");
            close(listen_fd);
            return 1;
        }

        if (!update_event(kq, listen_fd, EVFILT_READ, EV_ADD | EV_ENABLE)) {
            close(kq);
            close(listen_fd);
            return 1;
        }

        ClientBuffers client_input;   // fd -> bytes received, not yet parsed
        ClientBuffers client_output;  // fd -> bytes owed to the client

        std::unordered_set<int> closing;

        std::cout << "Redis-Lite server listening on 127.0.0.1:" << port
                  << " (kqueue event loop)" << std::endl;

        std::vector<struct kevent> events(64);

        while (g_running) {
            const int ready = kevent(kq,
                                     nullptr, 0,
                                     events.data(), static_cast<int>(events.size()),
                                     nullptr);

            if (ready < 0) {
                if (errno == EINTR) {
                    continue;  // Ctrl+C: the loop condition ends things
                }
                perror("kevent(wait)");
                break;
            }

            for (int i = 0; i < ready; ++i) {
                const struct kevent& event = events[static_cast<std::size_t>(i)];
                const int fd = static_cast<int>(event.ident);

                if ((event.flags & EV_ERROR) != 0) {
                    if (fd == listen_fd) {
                        std::cerr << "kevent error on the listening socket: "
                                  << std::strerror(static_cast<int>(event.data)) << "\n";
                        g_running = 0;
                    } else {
                        close_client(kq, fd, client_input, client_output, closing);
                    }
                    continue;
                }

                if (fd == listen_fd) {
                    accept_clients(kq, listen_fd, client_input, client_output);
                    continue;
                }

                if (event.filter == EVFILT_READ) {
                    const AfterRead outcome =
                        handle_readable(kq, fd, store, log, client_input, client_output);

                    if (outcome == AfterRead::CloseNow) {
                        close_client(kq, fd, client_input, client_output, closing);
                    } else if (outcome == AfterRead::CloseWhenSent) {
                        if (client_output[fd].empty()) {
                            close_client(kq, fd, client_input, client_output, closing);
                        } else {
                            closing.insert(fd);  // close once the reply is written
                        }
                    }
                    continue;
                }

                if (event.filter == EVFILT_WRITE) {
                    if (!try_send(kq, fd, client_output)) {
                        close_client(kq, fd, client_input, client_output, closing);
                        continue;
                    }
                    if (client_output[fd].empty()) {
                        // Nothing left to write: stop asking about writability,
                        // otherwise this event fires constantly.
                        update_event(kq, fd, EVFILT_WRITE, EV_DELETE);

                        if (closing.count(fd) > 0) {
                            close_client(kq, fd, client_input, client_output, closing);
                        }
                    }
                }
            }
        }

        std::cout << "\nRedis-Lite server shutting down; closing "
                  << client_input.size() << " client connection(s)\n";

        for (const auto& entry : client_input) {
            close(entry.first);
        }
        client_input.clear();
        client_output.clear();
        closing.clear();

        close(kq);
        close(listen_fd);

        std::cout << "event loop stopped\n";
        return 0;
    }

}  // namespace redis_lite

#include "server.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>  

#include <csignal>    
#include <cerrno>   
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "commands.hpp"
#include "resp.hpp"

namespace redis_lite {

    namespace {
        volatile sig_atomic_t g_running = 1;

        void handle_shutdown_signal(int /*signal_number*/) { g_running = 0; }

        void install_signal_handlers() {
            struct sigaction action{};
            action.sa_handler = handle_shutdown_signal;
            action.sa_flags = 0;
            sigemptyset(&action.sa_mask);
            sigaction(SIGINT, &action, nullptr);
            sigaction(SIGTERM, &action, nullptr);
        }

        bool send_reply(int client_fd, const std::string& bytes) {
            ssize_t sent = send(client_fd, bytes.data(), bytes.size(), 0);
            if (sent < 0) {
                perror("send");
                return false;
            }
            return true;
        }

        void handle_client(int client_fd, Store& store, std::mutex& store_mutex) {
            std::string buffer;
            char chunk[1024];
            bool connected = true;

            while (connected) {
                ssize_t received = recv(client_fd, chunk, sizeof(chunk), 0);

                if (received < 0) {
                    if (errno == EINTR) continue;

                    perror("recv");
                    break;
                }
                if (received == 0) {
                    std::cout << "client disconnected\n";
                    break;
                }

                buffer.append(chunk, static_cast<size_t>(received));

                // One read may carry several commands, or only part of one, so
                // keep executing until the leftover bytes are incomplete.
                while (connected) {
                    ParseResult result = parse(buffer);

                    if (result.status == ParseStatus::Incomplete) {
                        break;  // wait for the rest of this command
                    }

                    if (result.status == ParseStatus::Malformed) {
                        send_reply(client_fd,
                                   serialize(make_error("ERR Protocol error: " + result.error)));
                        // Redis closes the connection here.
                        std::cout << "protocol error: " << result.error << "\n";
                        connected = false;
                        break;
                    }

                    if (!result.value.elements.empty()) {
                        std::cout << "command: " << result.value.elements[0].string << "\n";
                    }

                    // The mutex guards the shared store and nothing else: it is held for the command only, never across recv() or send().
                    RespValue reply;
                    {
                        std::lock_guard<std::mutex> lock(store_mutex);
                        reply = execute_command(result.value, store);
                    }

                    if (!send_reply(client_fd, serialize(reply))) {
                        connected = false;
                        break;
                    }

                    
                    buffer.erase(0, result.consumed); // Drop only what this command used; the rest is the next one.
                }
            }

            close(client_fd);
        }

    }  // namespace

    int run_server(uint16_t port) {
        install_signal_handlers();

        int listen_fd = socket(AF_INET, SOCK_STREAM, 0); //listening socket
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

        std::cout << "Redis-Lite server listening on 127.0.0.1:" << port << "\n";

        Store store;
        std::mutex store_mutex;          // guards `store` across client threads
        std::vector<std::thread> client_threads;

        while (g_running) {
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;  // Ctrl+C: the loop condition ends things
                }
                perror("accept");
                break;
            }

            std::cout << ("client connected (fd " + std::to_string(client_fd) + ")\n");
            client_threads.emplace_back(handle_client,
                                        client_fd,
                                        std::ref(store),
                                        std::ref(store_mutex));
        }

        // Stop accepting first, then let the connected clients finish. A thread sitting in recv() returns when its client disconnects.
        close(listen_fd);
        std::cout << "\nRedis-Lite server shutting down; waiting for "
                  << client_threads.size() << " client thread(s)\n";

        for (std::thread& thread : client_threads) {
            thread.join();
        }

        std::cout << "all client threads finished\n";
        return 0;
    }

}  // namespace redis_lite

// Integration smoke test: starts the real redis-lite-server and talks to it
// over TCP. Covers the request-size limit, which cannot be reached from the
// unit tests because they do not link src/server.cpp.
//
//   redis-lite-integration <path-to-redis-lite-server> [port]

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "resp.hpp"

namespace {

    int failures = 0;

    void check(bool condition, const std::string& description) {
        if (condition) {
            std::cout << "  PASS  " << description << "\n";
        } else {
            std::cout << "  FAIL  " << description << "\n";
            ++failures;
        }
    }

    std::string escape(const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (c == '\r') {
                out += "\\r";
            } else if (c == '\n') {
                out += "\\n";
            } else {
                out += c;
            }
        }
        return out;
    }

    void check_equal(const std::string& actual,
                     const std::string& expected,
                     const std::string& description) {
        if (actual == expected) {
            std::cout << "  PASS  " << description << "\n";
        } else {
            std::cout << "  FAIL  " << description << "\n";
            std::cout << "          expected: \"" << escape(expected) << "\"\n";
            std::cout << "          actual:   \"" << escape(actual) << "\"\n";
            ++failures;
        }
    }

    struct Client {
        int fd = -1;
        std::string incoming;

        // Reads one complete reply; false if the connection closed first.
        bool next_reply(std::string& out) {
            while (true) {
                const redis_lite::ParseResult result = redis_lite::parse(incoming);
                if (result.status == redis_lite::ParseStatus::Ok) {
                    out.assign(incoming, 0, result.consumed);
                    incoming.erase(0, result.consumed);
                    return true;
                }
                if (result.status == redis_lite::ParseStatus::Malformed) {
                    return false;
                }

                char chunk[8192];
                const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);
                if (received <= 0) {
                    return false;
                }
                incoming.append(chunk, static_cast<std::size_t>(received));
            }
        }

        void close_it() {
            if (fd >= 0) {
                close(fd);
                fd = -1;
            }
        }
    };

    bool send_all(int fd, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const ssize_t wrote = send(fd, data + sent, size - sent, 0);
            if (wrote < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    bool send_all(int fd, const std::string& bytes) {
        return send_all(fd, bytes.data(), bytes.size());
    }

    int connect_to(uint16_t port) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return -1;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            close(fd);
            return -1;
        }
        return fd;
    }

    std::string encode(const std::vector<std::string>& arguments) {
        std::string out = "*" + std::to_string(arguments.size()) + "\r\n";
        for (const std::string& argument : arguments) {
            out += "$" + std::to_string(argument.size()) + "\r\n" + argument + "\r\n";
        }
        return out;
    }

    pid_t start_server(const char* binary, const std::string& log_path, uint16_t port) {
        const pid_t pid = fork();
        if (pid == 0) {
            const int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
            }
            const std::string port_text = std::to_string(port);
            execl(binary, binary, log_path.c_str(), port_text.c_str(),
                  static_cast<char*>(nullptr));
            _exit(127);
        }
        return pid;
    }

    int wait_for_server(uint16_t port) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const int fd = connect_to(port);
            if (fd >= 0) {
                return fd;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return -1;
    }

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: redis-lite-integration <path-to-redis-lite-server> [port]\n";
        return 2;
    }

    // A closed peer must not kill this process mid-write.
    signal(SIGPIPE, SIG_IGN);

    const char* binary = argv[1];
    const uint16_t port =
        (argc > 2) ? static_cast<uint16_t>(std::atoi(argv[2])) : static_cast<uint16_t>(16379);
    const std::string log_path = "/tmp/redis-lite-integration.aof";
    std::remove(log_path.c_str());

    const pid_t server = start_server(binary, log_path, port);
    if (server < 0) {
        std::cerr << "could not start the server\n";
        return 2;
    }

    Client client;
    client.fd = wait_for_server(port);
    if (client.fd < 0) {
        std::cerr << "server never accepted a connection on port " << port << "\n";
        kill(server, SIGKILL);
        waitpid(server, nullptr, 0);
        return 2;
    }

    std::cout << "integration test against " << binary << " on port " << port << "\n";

    std::cout << "\n-- ordinary commands --\n";
    std::string reply;
    check(send_all(client.fd, encode({"PING"})), "PING is sent");
    check(client.next_reply(reply), "a reply arrives");
    check_equal(reply, "+PONG\r\n", "PING answers PONG");

    send_all(client.fd, encode({"SET", "foo", "bar"}));
    client.next_reply(reply);
    check_equal(reply, "+OK\r\n", "SET answers OK");
    send_all(client.fd, encode({"GET", "foo"}));
    client.next_reply(reply);
    check_equal(reply, "$3\r\nbar\r\n", "GET returns the value");

    std::cout << "\n-- pipelining --\n";
    send_all(client.fd, encode({"PING"}) + encode({"SET", "pip", "yes"}) +
                            encode({"GET", "pip"}));
    std::string first;
    std::string second;
    std::string third;
    check(client.next_reply(first) && client.next_reply(second) && client.next_reply(third),
          "three pipelined replies arrive");
    check_equal(first + second + third, "+PONG\r\n+OK\r\n$3\r\nyes\r\n",
                "pipelined replies come back in order");

    std::cout << "\n-- a large but valid request --\n";
    {
        const std::string big_value(4u * 1024 * 1024, 'x');  // 4 MiB, under the limit
        send_all(client.fd, encode({"SET", "big", big_value}));
        check(client.next_reply(reply), "a 4 MiB SET is answered");
        check_equal(reply, "+OK\r\n", "a 4 MiB value is accepted");

        send_all(client.fd, encode({"GET", "big"}));
        check(client.next_reply(reply), "the 4 MiB value reads back");

        // "$" + length + CRLF + payload + CRLF
        const std::size_t expected_size =
            1 + std::to_string(big_value.size()).size() + 2 + big_value.size() + 2;
        check(reply.size() == expected_size, "every byte of it returns");
    }

    std::cout << "\n-- an incomplete request may not grow without bound --\n";
    {
        Client flooder;
        flooder.fd = connect_to(port);
        check(flooder.fd >= 0, "a second client connects");

        // Announce 100 MB and then never finish sending it.
        const std::string header = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$100000000\r\n";
        send_all(flooder.fd, header);

        const std::string block(65536, 'x');
        const std::size_t ceiling = 64u * 1024 * 1024;
        std::size_t accepted = 0;
        bool closed = false;

        while (accepted < ceiling) {
            if (!send_all(flooder.fd, block)) {
                closed = true;
                break;
            }
            accepted += block.size();
        }

        std::cout << "          server accepted " << (accepted / (1024 * 1024))
                  << " MiB before closing\n";
        check(closed, "the server closed the connection instead of buffering forever");
        check(accepted < ceiling, "it closed well before 64 MiB");
        check(accepted <= 32u * 1024 * 1024,
              "it closed within twice the 16 MiB limit");

        flooder.close_it();
    }

    std::cout << "\n-- the server survives that client --\n";
    {
        Client fresh;
        fresh.fd = connect_to(port);
        check(fresh.fd >= 0, "a new client can still connect");
        if (fresh.fd >= 0) {
            send_all(fresh.fd, encode({"PING"}));
            check(fresh.next_reply(reply), "the new client gets a reply");
            check_equal(reply, "+PONG\r\n", "the server still answers PING");

            send_all(fresh.fd, encode({"GET", "foo"}));
            fresh.next_reply(reply);
            check_equal(reply, "$3\r\nbar\r\n", "earlier data is intact");
            fresh.close_it();
        }
    }

    client.close_it();
    kill(server, SIGINT);
    waitpid(server, nullptr, 0);
    std::remove(log_path.c_str());

    std::cout << "\n";
    if (failures == 0) {
        std::cout << "all integration checks passed\n";
        return 0;
    }
    std::cout << failures << " integration check(s) failed\n";
    return 1;
}

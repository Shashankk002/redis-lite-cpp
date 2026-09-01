#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

#include "resp.hpp"

namespace {

    using redis_lite::ParseResult;
    using redis_lite::ParseStatus;
    using redis_lite::parse;

    struct Options {
        std::string host = "127.0.0.1";
        uint16_t port = 6379;
        int ops = 20000;
        int pipeline = 64;
        std::size_t value_size = 16;
        std::string only;
    };

    struct Connection {
        int fd = -1;
        std::string incoming;
        std::size_t parsed = 0;
    };

    std::string encode(const std::vector<std::string>& arguments) {
        std::string out = "*" + std::to_string(arguments.size()) + "\r\n";
        for (const std::string& argument : arguments) {
            out += "$" + std::to_string(argument.size()) + "\r\n" + argument + "\r\n";
        }
        return out;
    }

    bool open_connection(Connection& connection, const Options& options) {
        connection.fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connection.fd < 0) {
            perror("socket");
            return false;
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(options.port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (connect(connection.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            perror("connect");
            close(connection.fd);
            connection.fd = -1;
            return false;
        }

        // Nagle would batch small writes and destroy the sequential numbers.
        int nodelay = 1;
        setsockopt(connection.fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        return true;
    }

    bool send_all(int fd, const std::string& bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t wrote = send(fd, bytes.data() + sent, bytes.size() - sent, 0);
            if (wrote < 0) {
                if (errno == EINTR) {
                    continue;
                }
                perror("send");
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    // Reads until `wanted` complete replies have been parsed off the connection.
    bool await_replies(Connection& connection, int wanted) {
        char chunk[65536];

        while (wanted > 0) {
            // Consume whatever is already buffered first.
            while (wanted > 0) {
                const std::string_view remaining(connection.incoming.data() + connection.parsed,
                                                 connection.incoming.size() - connection.parsed);
                if (remaining.empty()) {
                    break;
                }

                const ParseResult result = parse(remaining);
                if (result.status == ParseStatus::Incomplete) {
                    break;
                }
                if (result.status == ParseStatus::Malformed) {
                    std::cerr << "benchmark: malformed reply: " << result.error << "\n";
                    return false;
                }
                connection.parsed += result.consumed;
                --wanted;
            }

            if (connection.parsed > 0) {
                connection.incoming.erase(0, connection.parsed);
                connection.parsed = 0;
            }
            if (wanted == 0) {
                break;
            }

            const ssize_t received = recv(connection.fd, chunk, sizeof(chunk), 0);
            if (received > 0) {
                connection.incoming.append(chunk, static_cast<std::size_t>(received));
                continue;
            }
            if (received < 0 && errno == EINTR) {
                continue;
            }
            std::cerr << "benchmark: connection closed with " << wanted << " replies outstanding\n";
            return false;
        }
        return true;
    }

    using RequestFor = std::function<std::string(const std::string&, int)>;

    bool seed(Connection& connection, const RequestFor& request, const std::string& prefix,
              int ops, int depth) {
        int sent = 0;
        while (sent < ops) {
            const int batch = std::min(depth, ops - sent);
            std::string payload;
            for (int i = 0; i < batch; ++i) {
                payload += request(prefix, sent + i);
            }
            if (!send_all(connection.fd, payload) || !await_replies(connection, batch)) {
                return false;
            }
            sent += batch;
        }
        return true;
    }

    double ops_per_second(int ops, std::chrono::steady_clock::duration elapsed) {
        const double seconds = std::chrono::duration<double>(elapsed).count();
        return (seconds > 0.0) ? (ops / seconds) : 0.0;
    }

    double run_sequential(Connection& connection, const RequestFor& request,
                          const std::string& prefix, int ops) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < ops; ++i) {
            if (!send_all(connection.fd, request(prefix, i)) || !await_replies(connection, 1)) {
                return 0.0;
            }
        }
        return ops_per_second(ops, std::chrono::steady_clock::now() - start);
    }

    double run_pipelined(Connection& connection, const RequestFor& request,
                         const std::string& prefix, int ops, int depth) {
        const auto start = std::chrono::steady_clock::now();
        int sent = 0;
        while (sent < ops) {
            const int batch = std::min(depth, ops - sent);
            std::string payload;
            for (int i = 0; i < batch; ++i) {
                payload += request(prefix, sent + i);
            }
            if (!send_all(connection.fd, payload) || !await_replies(connection, batch)) {
                return 0.0;
            }
            sent += batch;
        }
        return ops_per_second(ops, std::chrono::steady_clock::now() - start);
    }

    struct Benchmark {
        std::string name;
        RequestFor request;
        RequestFor setup;  // empty when the command needs no seeding
    };

    std::vector<Benchmark> build_benchmarks(const Options& options) {
        const std::string value(options.value_size, 'x');
        const RequestFor none;

        return {
            {"SET",
             [value](const std::string& p, int i) {
                 return encode({"SET", "key:" + p + std::to_string(i), value});
             },
             none},
            {"GET",
             [](const std::string& p, int i) {
                 return encode({"GET", "key:" + p + std::to_string(i)});
             },
             [value](const std::string& p, int i) {
                 return encode({"SET", "key:" + p + std::to_string(i), value});
             }},
            {"DEL",
             [](const std::string& p, int i) {
                 return encode({"DEL", "del:" + p + std::to_string(i)});
             },
             [value](const std::string& p, int i) {
                 return encode({"SET", "del:" + p + std::to_string(i), value});
             }},
            {"LPUSH",
             [value](const std::string& p, int i) {
                 return encode({"LPUSH", "list:" + p + std::to_string(i % 64), value});
             },
             none},
            {"LPOP",
             [](const std::string& p, int i) {
                 return encode({"LPOP", "poplist:" + p + std::to_string(i % 64)});
             },
             [value](const std::string& p, int i) {
                 return encode({"LPUSH", "poplist:" + p + std::to_string(i % 64), value});
             }},
            {"HSET",
             [value](const std::string& p, int i) {
                 return encode({"HSET", "hash:" + p, "field:" + std::to_string(i), value});
             },
             none},
            {"HGET",
             [](const std::string& p, int i) {
                 return encode({"HGET", "hash:" + p, "field:" + std::to_string(i)});
             },
             [value](const std::string& p, int i) {
                 return encode({"HSET", "hash:" + p, "field:" + std::to_string(i), value});
             }},
            {"SADD",
             [](const std::string& p, int i) {
                 return encode({"SADD", "set:" + p, "member:" + std::to_string(i)});
             },
             none},
            {"SISMEMBER",
             [](const std::string& p, int i) {
                 return encode({"SISMEMBER", "set:" + p, "member:" + std::to_string(i)});
             },
             [](const std::string& p, int i) {
                 return encode({"SADD", "set:" + p, "member:" + std::to_string(i)});
             }},
        };
    }

    bool parse_options(int argc, char* argv[], Options& options) {
        for (int i = 1; i < argc; ++i) {
            const std::string flag = argv[i];
            const bool has_value = (i + 1 < argc);

            if (flag == "--host" && has_value) {
                options.host = argv[++i];
            } else if (flag == "--port" && has_value) {
                options.port = static_cast<uint16_t>(std::stoi(argv[++i]));
            } else if (flag == "--ops" && has_value) {
                options.ops = std::stoi(argv[++i]);
            } else if (flag == "--pipeline" && has_value) {
                options.pipeline = std::stoi(argv[++i]);
            } else if (flag == "--value-size" && has_value) {
                options.value_size = static_cast<std::size_t>(std::stoi(argv[++i]));
            } else if (flag == "--only" && has_value) {
                options.only = argv[++i];
            } else {
                std::cerr << "unknown or incomplete option: " << flag << "\n";
                return false;
            }
        }
        return true;
    }

}  // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        return 1;
    }

    Connection connection;
    if (!open_connection(connection, options)) {
        return 1;
    }

    std::cout << "redis-lite benchmark\n"
              << "  target      " << options.host << ":" << options.port << "\n"
              << "  operations  " << options.ops << " per command\n"
              << "  pipeline    " << options.pipeline << "\n"
              << "  value size  " << options.value_size << " bytes\n\n";

    std::printf("%-12s %16s %18s\n", "command", "sequential/s", "pipelined/s");
    std::printf("%-12s %16s %18s\n", "-------", "------------", "-----------");

    // A short warm-up so the first measured command is not paying for connection
    // setup and first-touch allocation.
    const RequestFor ping = [](const std::string&, int) { return encode({"PING"}); };
    run_pipelined(connection, ping, "warmup", 2000, options.pipeline);

    for (const Benchmark& benchmark : build_benchmarks(options)) {
        if (!options.only.empty() && options.only != benchmark.name) {
            continue;
        }

        if (benchmark.setup && !seed(connection, benchmark.setup, "seq:", options.ops,
                                     options.pipeline)) {
            return 1;
        }
        const double sequential =
            run_sequential(connection, benchmark.request, "seq:", options.ops);

        if (benchmark.setup && !seed(connection, benchmark.setup, "pipe:", options.ops,
                                     options.pipeline)) {
            return 1;
        }
        const double pipelined =
            run_pipelined(connection, benchmark.request, "pipe:", options.ops, options.pipeline);

        std::printf("%-12s %16.0f %18.0f\n", benchmark.name.c_str(), sequential, pipelined);
        std::fflush(stdout);
    }

    close(connection.fd);
    return 0;
}

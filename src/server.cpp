#include "server.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>  

#include <csignal>    
#include <cerrno>   
#include <cstdio>
#include <iostream>

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

        void handle_client(int client_fd) {
            char buffer[1024];

            while (true) {
                ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);

                if (received < 0) {
                    if (errno == EINTR) continue;
                    
                    perror("recv");
                    break;
                }
                if (received == 0) {
                    std::cout << "client disconnected\n";
                    break;
                }

                std::cout << "received " << received << " bytes\n";

                ssize_t sent = send(client_fd, buffer, static_cast<size_t>(received), 0);
                if (sent < 0) {
                    perror("send");
                    break;
                }
                std::cout << "echoed " << sent << " bytes back\n";
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

        while (g_running) {
            int client_fd = accept(listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;  // Ctrl+C: the loop condition ends things
                }
                perror("accept");
                break;
            }

            std::cout << "client connected\n";
            handle_client(client_fd);
        }

        close(listen_fd);
        std::cout << "\nRedis-Lite server shutting down\n";
        return 0;
    }

}  // namespace redis_lite

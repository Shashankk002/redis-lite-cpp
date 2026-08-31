#include "server.hpp"

#include <netinet/in.h>   // sockaddr_in, htons, htonl
#include <sys/socket.h>   // socket, bind, listen, accept, send, recv
#include <unistd.h>       // close

#include <csignal>        // sigaction, sig_atomic_t
#include <cerrno>         // errno, EINTR
#include <cstdio>         // perror
#include <iostream>

namespace redis_lite {
namespace {

// Set to 0 by the Ctrl+C handler. volatile sig_atomic_t is the only type a
// signal handler may safely touch -- almost nothing else (including printing)
// is legal in there, so the handler does the bare minimum and the main loop
// notices on its next pass.
volatile sig_atomic_t g_running = 1;

void handle_shutdown_signal(int /*signal_number*/) { g_running = 0; }

// Ask the kernel to deliver SIGINT (Ctrl+C) and SIGTERM to our handler.
//
// sigaction rather than the shorter signal(): on macOS and the BSDs, signal()
// installs handlers that automatically restart interrupted system calls, so
// the blocking accept() below would resume instead of returning and Ctrl+C
// would appear to do nothing. sigaction with no SA_RESTART flag guarantees
// accept() returns with errno == EINTR.
void install_signal_handlers() {
    struct sigaction action{};
    action.sa_handler = handle_shutdown_signal;
    action.sa_flags = 0;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

// Reads from one client and writes the same bytes back, until that client
// disconnects. The connection is closed before returning.
void handle_client(int client_fd) {
    char buffer[1024];

    while (true) {
        ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);

        if (received < 0) {
            if (errno == EINTR) {
                continue;  // interrupted by a signal; try again
            }
            perror("recv");
            break;
        }
        if (received == 0) {
            std::cout << "client disconnected\n";
            break;
        }

        std::cout << "received " << received << " bytes\n";

        // One send call. On loopback with small messages this writes
        // everything; a partial write would need a loop, which needs message
        // boundaries to be meaningful -- that is a later stage's problem.
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

    // 1. Create the listening socket. IPv4 + TCP.
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    // Without SO_REUSEADDR the port stays reserved for a minute or so after
    // the server exits (TCP's TIME_WAIT), and restarting during development
    // fails with "Address already in use".
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(listen_fd);
        return 1;
    }

    // 2. Bind to 127.0.0.1:<port>. htons/htonl convert to network byte order;
    //    INADDR_LOOPBACK keeps the server reachable only from this machine.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind");
        close(listen_fd);
        return 1;
    }

    // 3. Start queueing incoming connections.
    if (listen(listen_fd, SOMAXCONN) < 0) {
        perror("listen");
        close(listen_fd);
        return 1;
    }

    std::cout << "Redis-Lite server listening on 127.0.0.1:" << port << "\n";

    // 4. Accept clients one at a time, forever. A client is served to
    //    completion before the next is accepted -- that is what "sequential"
    //    means here, and why a second client waits in the kernel's backlog
    //    until the first one goes away.
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

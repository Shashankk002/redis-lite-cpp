// Stage 1 experiment: the TCP server lifecycle, start to finish.
//
// This is a learning exercise, deliberately kept separate from src/. It is
// blocking and single-shot: it accepts exactly one client, echoes one message
// back, and exits. No event loop, no threads, no Redis protocol.
//
// Build and run:
//   cmake --build build
//   ./build/tcp-echo-server
//
// Then, from another terminal:
//   ./build/tcp-echo-client hello
//   or: echo hello | nc localhost 6380

#include <netinet/in.h>   // sockaddr_in, htons, htonl
#include <sys/socket.h>   // socket, bind, listen, accept, send, recv
#include <unistd.h>       // close

#include <cstdio>         // perror
#include <iostream>

int main() {
    const uint16_t port = 6380;  // 6379 is real Redis; stay out of its way.

    // 1. Create the socket.
    //    AF_INET  = IPv4, SOCK_STREAM = TCP. The socket exists but is not yet
    //    attached to any address.
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    // Without SO_REUSEADDR the port stays reserved for a minute or so after
    // the server exits (TCP's TIME_WAIT state), and an immediate restart fails
    // with "Address already in use".
    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }

    // 2. Bind the socket to 127.0.0.1:6380.
    //    Numbers that travel over the network go in big-endian ("network byte
    //    order"), which is what htons/htonl convert to. INADDR_LOOPBACK means
    //    localhost only, so nothing outside this machine can connect.
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // 3. Listen: mark the socket as passive so the kernel starts queueing
    //    incoming connections. The backlog is that queue's size; we only ever
    //    handle one client, so 1 is honest.
    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }
    std::cout << "listening on 127.0.0.1:" << port << "\n";

    // 4. Accept one connection. This blocks until a client arrives, then
    //    returns a *new* socket for that client. server_fd keeps listening;
    //    client_fd is the actual conversation.
    int client_fd = accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        return 1;
    }
    std::cout << "client connected\n";

    // 5. Receive data. recv blocks until bytes arrive. It returns how many
    //    bytes were read: 0 means the client closed the connection, negative
    //    means an error. The bytes are NOT null-terminated, so the length
    //    matters.
    char buffer[1024];
    ssize_t received = recv(client_fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
        perror("recv");
        close(client_fd);
        close(server_fd);
        return 1;
    }
    if (received == 0) {
        std::cout << "client disconnected without sending anything\n";
        close(client_fd);
        close(server_fd);
        return 0;
    }

    std::cout << "received " << received << " bytes: ";
    std::cout.write(buffer, received);
    std::cout << "\n";

    // 6. Send the same bytes back.
    ssize_t sent = send(client_fd, buffer, static_cast<size_t>(received), 0);
    if (sent < 0) {
        perror("send");
        close(client_fd);
        close(server_fd);
        return 1;
    }
    std::cout << "echoed " << sent << " bytes back\n";

    // 7. Close both sockets. Closing client_fd is what tells the client the
    //    conversation is over.
    close(client_fd);
    close(server_fd);
    std::cout << "server shutting down\n";
    return 0;
}

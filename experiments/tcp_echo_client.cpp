// Stage 1 experiment: the client half of the same conversation.
//
// Connects to the echo server, sends one message, prints the reply, exits.
//
//   ./build/tcp-echo-client            (sends "hello")
//   ./build/tcp-echo-client "ping me"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>

int main(int argc, char* argv[]) {
    const uint16_t port = 6380;
    const char* message = (argc > 1) ? argv[1] : "hello";


    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }
    std::cout << "connected to 127.0.0.1:" << port << "\n";

    std::cout << "sending: " << message << "\n";
    if (send(fd, message, strlen(message), 0) < 0) {
        perror("send");
        close(fd);
        return 1;
    }

    char buffer[1024];
    ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
    if (received < 0) {
        perror("recv");
        close(fd);
        return 1;
    }

    std::cout << "server replied: ";
    std::cout.write(buffer, received);
    std::cout << "\n";

    close(fd);
    return 0;
}

//
// Created by mkoshel on 06.10.2021.
//
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <liburing.h>
#include <string>
#include <netinet/in.h>

#include <thread>


#include <unistd.h>
#include <arpa/inet.h>

#define PORT                8080
#define QUEUE_DEPTH         256

static struct io_uring ioUring;

void fatal_error(std::string error) {
    perror(error.c_str());
    exit(1);
}

int setup_server(int port) {
    int socketDescriptor;
    socketDescriptor = socket(PF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serverAddress;
    if (socketDescriptor == -1) fatal_error("socket()");
    int enable = 1;
    auto setOptResult = setsockopt(socketDescriptor, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
    if (setOptResult) fatal_error("setsockopt(SO_REUSEADDR)");
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    auto bindResult = bind(socketDescriptor, (sockaddr *) &serverAddress, sizeof(serverAddress));
    if (bindResult) fatal_error("bind()");
    auto listenResult = listen(socketDescriptor, 100);
    if (listenResult) fatal_error("listen()");
    return socketDescriptor;
}

void mainLoop(int socketDescriptor) {
    int clientSocket{-1};
    auto buffer = new char[512];
    auto outBuffer = new char[512];
    struct sockaddr_in clientAddress;
    auto cientAddressSize{sizeof clientAddress};
    while (true) {
        if (clientSocket < 0) {
            printf("waiting for the new connection...\n");
            clientSocket = accept(socketDescriptor, (sockaddr *) &clientAddress, (socklen_t *) &cientAddressSize);
            printf("accepted new connection! %s:%i\n", (inet_ntoa(clientAddress.sin_addr)), htons (clientAddress.sin_port));
        }
        if (clientSocket < 0) {
            printf("could not accept new connection!\n");
            continue;
        }
        try {
            auto readResult = read(clientSocket, buffer, 512);
            if (readResult < 0) {
                printf("error on reading from socket , %i", readResult);
                close(clientSocket);
                clientSocket = -1;
            }
            sprintf(outBuffer, "you sent %s", buffer);
            auto writeResult = send(clientSocket, outBuffer, 512, 0);
            if (writeResult < 0) printf("error on reading from socket , %i", readResult);
            close(clientSocket);
            clientSocket = -1;
        } catch (std::exception e) {
            printf("error %s \n", e.what());
        }
        wmemset(reinterpret_cast<wchar_t *>(buffer), 0, sizeof buffer);
        wmemset(reinterpret_cast<wchar_t *>(outBuffer), 0, sizeof outBuffer);
    }
}


int main(int argc, char *argv[]) {
    auto server_socket = setup_server(PORT);
    signal(SIGINT, [](int signalNumber) {
        printf("^C pressed. Shutting down.\n");
        io_uring_queue_exit(&ioUring);
        exit(0);
    });
    signal(SIGPIPE, [](int signalNumber) {
        printf("SIGPIPE caught!\n");
    });

    //    io_uring_queue_init(QUEUE_DEPTH, &ioring, 0);
    mainLoop(server_socket);
    return 0;
}
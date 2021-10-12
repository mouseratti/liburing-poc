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
#include <cstring>

#define PORT                8080
#define SIZEOF_READ 32
static struct io_uring ioUring;
static struct sockaddr_in serverAddress;
static bool isNewRequestRequired{true};
enum class RequestType {
    ACCEPT,
    READ,
    WRITE
};

struct Request {
    RequestType eventType;
    int clientSocket{-1};
    std::shared_ptr<sockaddr_in> clientAddress;
    int clientAddressSize;
    iovec buffer;

    Request(RequestType type) : eventType{type}, clientAddress{nullptr}, clientAddressSize{sizeof clientAddress} {
        printf("Request %d: %i", this, eventType);
    }
    ~Request() {
        free(buffer.iov_base);
    }
    static std::shared_ptr<Request> makeRequest() {
        return std::make_shared<Request>(RequestType::ACCEPT);
    };

    std::shared_ptr<char> toString() {
        auto result = std::make_shared<char>();
        sprintf(result.get(), "%s:%d type %d", inet_ntoa(clientAddress->sin_addr), htons(clientAddress->sin_port),
                eventType);
        return result;
    }

    void allocateBuffer(int size) {
        buffer.iov_base = malloc(size);
        memset(buffer.iov_base, 0, size);
        buffer.iov_len = (size_t) size;
    }
};


void fatal_error(std::string error) {
    perror(error.c_str());
    exit(1);
}

int setup_server(int port) {
    int socketDescriptor;
    socketDescriptor = socket(PF_INET, SOCK_STREAM, 0);
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
    struct io_uring_cqe *cqe;
    struct io_uring_sqe *sqe;
    std::shared_ptr<Request> request;
    while (true) {
        if (isNewRequestRequired) {
            request = Request::makeRequest();
            sqe = io_uring_get_sqe(&ioUring);
            io_uring_prep_accept(sqe, socketDescriptor, (sockaddr *) request->clientAddress.get(),
                                 (socklen_t *) &request->clientAddressSize, 0);
            io_uring_sqe_set_data(sqe, request.get());
            io_uring_submit(&ioUring);
            isNewRequestRequired = false;
        }
        int pollResult = io_uring_wait_cqe(&ioUring, &cqe);
        if (pollResult < 0) fatal_error("poll result is negative!");
        auto rawResponse = (Request *) io_uring_cqe_get_data(cqe);
        auto response = std::shared_ptr<Request>(rawResponse);
        if (cqe->res < 0) printf("%s failed!", response->toString().get());
        switch (response->eventType) {
            case RequestType::ACCEPT:
                isNewRequestRequired = true;
//                if accept request failed, forgetting it;
                if (cqe->res < 0) break;
//                submit read request
                sqe = io_uring_get_sqe(&ioUring);
                response->eventType = RequestType::READ;
                response->clientSocket = cqe->res;
                response->allocateBuffer(SIZEOF_READ);
                io_uring_prep_readv(sqe, response->clientSocket, &(response->buffer), 1, 0);
                io_uring_sqe_set_data(sqe, response.get());
                io_uring_submit(&ioUring);
                break;
            case RequestType::READ:
//                ignoring failed request
                if (cqe->res < 0) break;
                printf("read %s from %s: %d", (char *) response->buffer.iov_base, inet_ntoa(response->clientAddress->sin_addr), htons(response->clientAddress->sin_port));
                response->eventType = RequestType::WRITE;
                sqe = io_uring_get_sqe(&ioUring);
                io_uring_prep_writev(sqe, response->clientSocket, &(response->buffer), 1, 0);
                io_uring_sqe_set_data(sqe, response.get());
                io_uring_submit(&ioUring);
            case RequestType::WRITE:
                close(response->clientSocket);
                break;
        }
        io_uring_cqe_seen(&ioUring, cqe);

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

    io_uring_queue_init(256, &ioUring, 0);
    mainLoop(server_socket);
    return 0;
}
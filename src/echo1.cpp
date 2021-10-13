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
#define BUFFER_SIZE 32
static struct io_uring ioUring;
static struct sockaddr_in serverAddress;
static bool isNewRequestRequired{true};
enum class RequestType {
    ACCEPT_CONN,
    READ_SOCKET,
    WRITE_SOCKET
};

struct Request {
    RequestType eventType;
    int clientSocket{-1};
    sockaddr_in clientAddress;
    int clientAddressSize{sizeof clientAddress};
    iovec buffer;

    Request(RequestType type) : eventType{type} { }
    ~Request() {
        free(buffer.iov_base);
    }

    static Request *makeRequest() {
        return new Request(RequestType::ACCEPT_CONN);
    };

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
    Request *request;
    while (true) {
        printf("new iteration\n");
        if (isNewRequestRequired) {
            printf("waiting for new connection\n");
            request = Request::makeRequest();
            sqe = io_uring_get_sqe(&ioUring);
            io_uring_prep_accept(sqe, socketDescriptor, (sockaddr *) &(request->clientAddress),
                                 (socklen_t *) &(request->clientAddressSize), 0);
            io_uring_sqe_set_data(sqe, request);
            io_uring_submit(&ioUring);
            isNewRequestRequired = false;
        }
        int pollResult = io_uring_wait_cqe(&ioUring, &cqe);
        if (pollResult < 0) fatal_error("poll result is negative!\n");
        auto response = (Request *) io_uring_cqe_get_data(cqe);
        if (cqe->res < 0) {
            printf("%s type %d failed!\n", response, response->eventType);
            if (response->eventType == RequestType::ACCEPT_CONN) isNewRequestRequired = true;
            delete response;
            io_uring_cqe_seen(&ioUring, cqe);
            continue;
        }
        switch (response->eventType) {
            case RequestType::ACCEPT_CONN:
                isNewRequestRequired = true;
                printf("new connection accepted from %s:%d\n", inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port) );
//                submit read request
                sqe = io_uring_get_sqe(&ioUring);
                response->eventType = RequestType::READ_SOCKET;
                (response->clientSocket) = cqe->res;
                response->allocateBuffer(BUFFER_SIZE);
                io_uring_prep_readv(sqe, response->clientSocket, &(response->buffer), 1, 0);
                io_uring_sqe_set_data(sqe, response);
                io_uring_submit(&ioUring);
                printf("waiting for data from %s:%d\n", inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port) );
                break;
            case RequestType::READ_SOCKET:
//                ignoring failed request
                printf("read %d bytes from %s:%d\n", response->buffer.iov_len, inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port));
                response->eventType = RequestType::WRITE_SOCKET;
                sqe = io_uring_get_sqe(&ioUring);
                io_uring_prep_writev(sqe, response->clientSocket, &(response->buffer), 1, 0);
                io_uring_sqe_set_data(sqe, response);
                io_uring_submit(&ioUring);
                printf("wrote buffer back to %s:%d\n", inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port));
                break;
            case RequestType::WRITE_SOCKET:
                printf("%s:%d event %d\n",  inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port), response->eventType);
                close(response->clientSocket);
                delete response;
                break;
            default:
                printf("unknown response\n");
        }
        io_uring_cqe_seen(&ioUring, cqe);
        printf("end of current iteration\n");

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
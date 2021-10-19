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
#include <arpa/inet.h>
#include <cstring>
#include <map>
#include <fcntl.h>
#include <sstream>

using std::string;

#define PORT                8080
#define BUFFER_SIZE 32
#define DIRNAME "/tmp/mkoshel/"
static struct io_uring ioUring;
static struct sockaddr_in serverAddress;
static bool isNewRequestRequired{true};
enum class RequestType {
    ACCEPT_CONN,
    READ_SOCKET,
    WRITE_SOCKET,
    OPEN_FILE,
    WRITE_FILE,
    CLOSE_FILE,
    CLOSE_SOCKET,
};

struct Request {
    RequestType eventType;
    int clientSocket{-1};
    int fileDescriptor{-1};
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
        if (buffer.iov_base != nullptr) return;
        buffer.iov_base = malloc(size);
        memset(buffer.iov_base, 0, size);
        buffer.iov_len = (size_t) size;
    }
    void askCloseSocket(io_uring* ring) {
        auto sqe = io_uring_get_sqe(ring);
        this->eventType = RequestType::CLOSE_SOCKET;
        io_uring_prep_close(sqe, clientSocket);
        io_uring_sqe_set_data(sqe, this);
        io_uring_submit(ring);
    }
    void askWriteSocket(io_uring* ring) {
        eventType = RequestType::WRITE_SOCKET;
        auto sqe = io_uring_get_sqe(ring);
        io_uring_prep_writev(sqe, clientSocket, &buffer, 1, 0);
        io_uring_sqe_set_data(sqe, this);
        io_uring_submit(&ioUring);
        printf("request to write data back to %s\n", toString().c_str());
    }
    void askReadSocket(io_uring* ring) {
        eventType = RequestType::READ_SOCKET;
        auto sqe = io_uring_get_sqe(ring);
        this->allocateBuffer(BUFFER_SIZE);
        io_uring_prep_readv(sqe, clientSocket, &buffer, 1, 0);
        io_uring_sqe_set_data(sqe, this);
        io_uring_submit(&ioUring);
        printf("request to read data from  %s\n", toString().c_str());
    }

    void askOpenFile(io_uring * ring, int directoryFd) {
        eventType = RequestType::OPEN_FILE;
        char filePath[32];
        memset(filePath, 0, sizeof filePath);
        sprintf(filePath, "%s", toString().c_str());
        auto  sqe = io_uring_get_sqe(ring);
        io_uring_prep_openat(sqe, directoryFd, filePath,O_RDWR| O_CREAT | O_NONBLOCK, 0777);
        io_uring_sqe_set_data(sqe, this);
        io_uring_submit(&ioUring);
        printf("requesting to open a file %s\n", filePath);
    }


    string toString() {
        char buffer[32];
        sprintf(buffer,"%s:%d type %d", inet_ntoa(clientAddress.sin_addr), htons(clientAddress.sin_port), eventType);
        return string{buffer};
    }
};
//static std::map<long,Request*> activeRequests;


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

void mainLoop(int socketDescriptor, int directoryFd) {
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
        if (io_uring_peek_cqe(&ioUring, &cqe) == -EAGAIN ) io_uring_wait_cqe(&ioUring, &cqe);
        auto response = (Request *) io_uring_cqe_get_data(cqe);
        if (cqe->res < 0) {
            if (response->eventType == RequestType::ACCEPT_CONN) isNewRequestRequired = true;
            printf("%s fail! error %d\n", response->toString().c_str(), cqe->res);
            response->askCloseSocket(&ioUring);
            io_uring_cqe_seen(&ioUring, cqe);
            continue;
        }
        switch (response->eventType) {
            case RequestType::ACCEPT_CONN:
                isNewRequestRequired = true;
                printf("new connection accepted from %s:%d\n", inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port) );
//                submit read request
                response->clientSocket = cqe->res;
                response->askOpenFile(&ioUring, directoryFd);
                break;
            case RequestType::OPEN_FILE:
                response->fileDescriptor = cqe->res;
                printf("file opened! fd is %d\n", response->fileDescriptor);
                response->askReadSocket(&ioUring);
                break;
            case RequestType::READ_SOCKET:
                printf("read %d bytes from %s:%d\n", response->buffer.iov_len, inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port));
                response->eventType = RequestType::WRITE_FILE;
                sqe = io_uring_get_sqe(&ioUring);
                io_uring_prep_writev(sqe, response->fileDescriptor, &(response->buffer), 1, 0);
                io_uring_sqe_set_data(sqe, response);
                io_uring_submit(&ioUring);
                printf("requested to write buffer into file %s\n", response);
                break;
            case RequestType::WRITE_FILE:
                printf("echo written into file %d!\n",  response);
                response->askWriteSocket(&ioUring);
                break;
            case RequestType::WRITE_SOCKET:
                printf("%s:%d echo sent!\n",  inet_ntoa(response->clientAddress.sin_addr), htons(response->clientAddress.sin_port), response->eventType);
                response->askReadSocket(&ioUring);
                break;
            case RequestType::CLOSE_SOCKET:
                printf("%s closed by the server!\n", response->toString().c_str());
                delete response;
                break;
            default:
                printf("unknown response\n");
        }
        io_uring_cqe_seen(&ioUring, cqe);
    }
}
int makeDirectory(string dirName) {
    std::stringstream ss;
    ss << "mkdir -p " << dirName;
    int result;
    result = system(ss.str().c_str());
    if (result < 0 ) fatal_error("could not make a directory!");
    result  = open(dirName.c_str(), O_DIRECTORY | O_RDONLY);
    if (result < 0 ) fatal_error("could not open a directory!!!");
    return  result;
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
    auto directoryFd = makeDirectory(DIRNAME);
    io_uring_queue_init(256, &ioUring, 0);
    mainLoop(server_socket, directoryFd);
    return 0;
}
/*
** atom_supplier.c -- a TCP client that sends ADD commands and prints server replies
**
** Usage: ./atom_supplier -h <hostname> -p <port>
** After connecting, you can enter commands of the form:
**   ADD CARBON <number>
**   ADD OXYGEN <number>
**   ADD HYDROGEN <number>
** Each command must be on its own line (ending with Enter). For example:
**   ADD CARBON 100
**   ADD OXYGEN 50
**   ADD HYDROGEN 1000000000000000000
** To exit, press Ctrl+D (EOF) or Ctrl+C.
*/

#include <stdio.h>          // for fgets, printf, fprintf
#include <stdlib.h>         // for exit
#include <string.h>         // for strlen, strcmp, memset
#include <unistd.h>         // for close
#include <errno.h>          // for errno
#include <netdb.h>          // for getaddrinfo, freeaddrinfo, struct addrinfo
#include <sys/types.h>      // for socklen_t
#include <sys/socket.h>     // for socket, connect, send, recv
#include <arpa/inet.h>      // for inet_ntop (print IP address)
#include <getopt.h>          // getopt
#include <sys/un.h>          // sockaddr_un

#define MAXDATASIZE 1024    // maximum buffer size for receiving data

// get_in_addr: return a pointer to the IPv4 or IPv6 address within sockaddr
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        // IPv4 address
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
    // IPv6 address
    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(int argc, char *argv[]) {
    char *hostname = NULL;
    char *port_str = NULL;
    char *uds_path  = NULL;    // new: "-f" specifies UDS datagram socket file

    // 1) Parse command‐line arguments: either UDP or UDS_DGRAM
    const char *short_opts = "h:p:f:";
    int opt;
    while ((opt = getopt(argc, argv, short_opts)) != -1) {
        switch (opt) {
            case 'h':
                // e.g. "-h server.example.com"
                hostname = optarg;
                break;
            case 'p':
                // e.g. "-p 5555"
                port_str = optarg;
                break;
            case 'f':
                // e.g. "-f /tmp/molecule_dgram.sock"
                uds_path = optarg;
                break;
            default:
                fprintf(stderr,
                    "Usage:\n"
                    "  UDP mode:      %s -h <hostname> -p <port>\n"
                    "  UDS_STREAM mode:%s -f <uds_socket_file_path>\n",
                    argv[0], argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // 2) Decide which transport to use (exactly one)
    int use_tcp        = (hostname && port_str) ? 1 : 0;
    int use_uds_stream = (uds_path) ? 1 : 0;
    if ((use_tcp + use_uds_stream) != 1) {
        fprintf(stderr,
            "ERROR: you must specify exactly one transport mode:\n"
            "  TCP:         -h <hostname> -p <port>\n"
            "  UDS_STREAM:  -f <uds_socket_file>\n");
        exit(EXIT_FAILURE);
    }

    int sockfd = -1;    // this will hold our socket FD

    if (use_tcp) {
        //------------------------------------------------------------
        // TCP mode: do a normal getaddrinfo() + socket() + connect()
        //------------------------------------------------------------
        struct addrinfo hints, *servinfo, *p;
        memset(&hints, 0, sizeof hints);
        hints.ai_family   = AF_UNSPEC;         // IPv4 or IPv6
        hints.ai_socktype = SOCK_STREAM;       // TCP streams

        int rv;
        if ((rv = getaddrinfo(hostname, port_str, &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            return 1;
        }

        for (p = servinfo; p != NULL; p = p->ai_next) {
            sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (sockfd < 0) {
                perror("client: socket");
                continue;
            }
            if (connect(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
                perror("client: connect");
                close(sockfd);
                continue;
            }
            break;  // got connected successfully
        }

        if (p == NULL) {
            fprintf(stderr, "client: failed to connect to %s:%s\n", hostname, port_str);
            freeaddrinfo(servinfo);
            return 2;
        }

        // print the server IP we connected to
        char s[INET6_ADDRSTRLEN];
        inet_ntop(p->ai_family,
                  get_in_addr((struct sockaddr *)p->ai_addr),
                  s, sizeof s);
        printf("client (TCP): connected to %s:%s\n", s, port_str);

        freeaddrinfo(servinfo);
    }
    else {
        //------------------------------------------------------------
        // UDS_STREAM mode: create AF_UNIX/SOCK_STREAM socket + connect()
        //------------------------------------------------------------
        sockfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sockfd < 0) {
            perror("socket (UDS_STREAM)");
            exit(EXIT_FAILURE);
        }

        struct sockaddr_un uds_addr;
        memset(&uds_addr, 0, sizeof(uds_addr));
        uds_addr.sun_family = AF_UNIX;
        // copy the path into sun_path (null-terminated)
        strncpy(uds_addr.sun_path, uds_path, sizeof(uds_addr.sun_path) - 1);

        if (connect(sockfd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) < 0) {
            perror("connect (UDS_STREAM)");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        printf("client (UDS_STREAM): connected to %s\n", uds_path);
    }

    // 3) Print a brief help / available commands
    printf("\nAvailable commands (each on its own line):\n");
    printf("  ADD CARBON <number>\n");
    printf("  ADD OXYGEN <number>\n");
    printf("  ADD HYDROGEN <number>\n");
    printf("Type Ctrl+D or Ctrl+C to exit.\n\n");

    // 4) Read lines from stdin and send them to the server
    char line[MAXDATASIZE];
    char buffer[MAXDATASIZE];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strcmp(line, "\n") == 0) {
            // skip empty line
            continue;
        }

        size_t len = strlen(line);
        if (send(sockfd, line, len, 0) == -1) {
            perror("send");
            break;
        }

        // 5) Receive the server's reply
        ssize_t numbytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (numbytes < 0) {
            perror("recv");
            break;
        }
        else if (numbytes == 0) {
            // server closed connection
            printf("Server closed connection\n");
            break;
        }

        buffer[numbytes] = '\0';
        printf("%s", buffer);
    }

    // 6) Close socket
    close(sockfd);
    printf("client: connection closed\n");
    return 0;
}
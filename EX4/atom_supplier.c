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
    // 1) Variables to hold the parsed arguments
    char *hostname = NULL;       // we expect a string for hostname/IP
    char *port_str = NULL;   // string for port number
    
    // 2) Define the short options string: "h:p:"
    //    'h' expects an argument (hostname), 'p' expects an argument (port)
    const char *short_opts = "h:p:";
    int opt;

    // 3) If the total argc is not exactly 5 (program name + -h + hostname + -p + port),
    //    we can still try parsing with getopt, but let's at least hint if there aren't enough args.
    if (argc < 5) {
        fprintf(stderr, "Usage: %s -h <hostname/IP> -p <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 4) Call getopt in a loop to parse '-h' and '-p'
    //    getopt will return the character of the option it finds, or -1 when done.
    while ((opt = getopt(argc, argv, short_opts)) != -1) {
        switch (opt) {
            case 'h':
                // optarg points to the string after '-h'
                hostname = optarg;
                break;
            case 'p':
                // optarg points to the string after '-p'
                port_str = optarg;   // convert string to integer
                break;
            case '?':
            default:
                // Unknown option or missing required argument
                fprintf(stderr, "Usage: %s -h <hostname/IP> -p <port>\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }
    
    // 5) After getopt, we should have both hostname and port set correctly.
    //    If either is missing or port is invalid, print error and exit.
    if (hostname == NULL || port_str == NULL) {
        fprintf(stderr, "ERROR: both -h <hostname/IP> and -p <port> must be specified.\n");
        fprintf(stderr, "Usage: %s -h <hostname/IP> -p <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 1. Prepare hints for getaddrinfo
    struct addrinfo hints, *servinfo, *p;
    memset(&hints, 0, sizeof hints);                      // zero out hints
    hints.ai_family   = AF_UNSPEC;                         // allow IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;                       // TCP stream socket

    int rv;
    if ((rv = getaddrinfo(hostname, port_str, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // 2. Loop through all the results and connect to the first we can
    int sockfd;
    for (p = servinfo; p != NULL; p = p->ai_next) {
        // create a socket
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd < 0) {
            perror("client: socket");
            continue;                                      // try next address
        }

        // connect to the server
        if (connect(sockfd, p->ai_addr, p->ai_addrlen) < 0) {
            perror("client: connect");
            close(sockfd);                                // close this socket on failure
            continue;                                      // try next address
        }
        break;                                            // if we get here, we have connected
    }

    if (p == NULL) {                                      // if p is NULL, no address succeeded
        fprintf(stderr, "client: failed to connect to %s:%s\n", hostname, port_str);
        freeaddrinfo(servinfo);                           // free the linked list
        return 2;
    }

    // 3. Print the address to which we connected
    char s[INET6_ADDRSTRLEN];
    inet_ntop(p->ai_family, get_in_addr((struct sockaddr *)p->ai_addr),
              s, sizeof s);
    printf("client: connected to %s:%s\n", s, port_str);

    freeaddrinfo(servinfo);                               // done with servinfo

    // 4. Print a brief help / available commands
    printf("\nAvailable commands (each on its own line):\n");
    printf("  ADD CARBON <number>\n");
    printf("  ADD OXYGEN <number>\n");
    printf("  ADD HYDROGEN <number>\n");
    printf("Type Ctrl+D or Ctrl+C to exit.\n\n");

    // 5. Read lines from stdin and send them to the server as commands
    char line[MAXDATASIZE];
    char buffer[MAXDATASIZE];
    while (fgets(line, sizeof(line), stdin) != NULL) {    // read a line from stdin
        if (strcmp(line, "\n") == 0) {                    // skip empty lines
            continue;
        }

        size_t len = strlen(line);                        // length of the line including newline
        // send the line (including newline) to the server
        if (send(sockfd, line, len, 0) == -1) {
            perror("send");
            break;                                        // exit loop on send error
        }

        // 6. Receive the server's reply
        ssize_t numbytes = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
        if (numbytes < 0) {
            perror("recv");
            break;                                        // exit loop on recv error
        } else if (numbytes == 0) {
            // server closed connection
            printf("Server closed connection\n");
            break;                                        // exit loop
        }

        buffer[numbytes] = '\0';                           // null-terminate the received data
        printf("%s", buffer);                              // print the server's response
    }

    // 7. Close the socket once done or on EOF
    close(sockfd);                                          // close connection
    printf("client: connection closed\n");
    return 0;
}

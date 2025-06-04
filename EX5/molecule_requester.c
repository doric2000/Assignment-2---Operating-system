/*
** molecule_requester.c -- a client that sends DELIVER commands and prints server replies
**
** Now supports:
**   • UDP mode:    ./molecule_requester -h <hostname> -p <port>
**   • UDS‐DGRAM:   ./molecule_requester -f <uds_socket_file_path>
**
** After “connecting,” you can enter commands of the form:
**   DELIVER WATER <number>
**   DELIVER CARBON DIOXIDE <number>
**   DELIVER ALCOHOL <number>
**   DELIVER GLUCOSE <number>
** Each command must be on its own line. To exit, press Ctrl+D (EOF) or Ctrl+C.
*/

#include <stdio.h>          // for fgets, printf, fprintf
#include <stdlib.h>         // for exit, malloc, free
#include <string.h>         // for strlen, strcmp, memset, memcpy, strtok_r
#include <unistd.h>         // for close, getopt
#include <errno.h>          // for errno
#include <netdb.h>          // for getaddrinfo, freeaddrinfo, struct addrinfo
#include <sys/types.h>      // for socklen_t
#include <sys/socket.h>     // for socket, sendto, recvfrom
#include <arpa/inet.h>      // for inet_ntop (print IP address)
#include <sys/un.h>         // for sockaddr_un (UDS)
#include <stddef.h>         // for offsetof
#include <getopt.h>          // getopt_long
#include <stddef.h>          // offsetof
#include <sys/un.h>          // sockaddr_un

#define MAXDATASIZE 1024    // maximum buffer size for sending/receiving

// get_in_addr: given a sockaddr*, return pointer to the IPv4 or IPv6 address
static void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }
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
                    "  UDS_DGRAM mode:%s -f <uds_socket_file_path>\n",
                    argv[0], argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // 2) Decide which transport to use. Exactly one must be set.
    int use_udp       = (hostname && port_str) ? 1 : 0;
    int use_uds_dgram = (uds_path) ? 1 : 0;

    if ((use_udp + use_uds_dgram) != 1) {
        fprintf(stderr,
            "ERROR: you must specify exactly one transport mode:\n"
            "  UDP:           -h <hostname> -p <port>\n"
            "  UDS_DGRAM:     -f <uds_socket_file_path>\n");
        exit(EXIT_FAILURE);
    }

    // 3) Create a socket, and if UDP, resolve the remote address now.
    int sockfd = -1;
    struct sockaddr *server_addr = NULL;
    socklen_t server_addr_len = 0;

    if (use_udp) {
        // ----- UDP Mode: use getaddrinfo() to find the server’s IP/port -----
        struct addrinfo hints, *servinfo, *p;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family   = AF_UNSPEC;   // IPv4 or IPv6
        hints.ai_socktype = SOCK_DGRAM;  // UDP

        int rv;
        if ((rv = getaddrinfo(hostname, port_str, &hints, &servinfo)) != 0) {
            fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
            exit(EXIT_FAILURE);
        }

        // Loop through results and pick the first we can open
        for (p = servinfo; p != NULL; p = p->ai_next) {
            sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (sockfd < 0) {
                perror("socket (UDP)");
                continue;
            }
            // Save a copy of the server’s sockaddr for sendto()/recvfrom():
            server_addr_len = p->ai_addrlen;
            server_addr = malloc(server_addr_len);
            if (!server_addr) {
                perror("malloc");
                close(sockfd);
                freeaddrinfo(servinfo);
                exit(EXIT_FAILURE);
            }
            memcpy(server_addr, p->ai_addr, server_addr_len);
            break;
        }
        if (p == NULL) {
            fprintf(stderr, "Failed to create UDP socket to %s:%s\n", hostname, port_str);
            freeaddrinfo(servinfo);
            exit(EXIT_FAILURE);
        }

        // Print out the concrete IP we will send to:
        {
            char ipstr[INET6_ADDRSTRLEN];
            inet_ntop(p->ai_family,
                      get_in_addr((struct sockaddr*)p->ai_addr),
                      ipstr, sizeof(ipstr));
            printf("client: UDP socket ready to send to %s:%s\n", ipstr, port_str);
        }
        freeaddrinfo(servinfo);
    }
    else {
        // ----- UDS_DGRAM Mode -----
        sockfd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (sockfd < 0) {
            perror("socket (UDS_DGRAM)");
            exit(EXIT_FAILURE);
        }

        // To receive the server’s reply, bind ourselves to a unique abstract address:
        struct sockaddr_un local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sun_family = AF_UNIX;
        // Place a leading '\0' so this is an abstract‐namespace socket:
        local_addr.sun_path[0] = '\0';
        // Name it “mreq_<pid>” to avoid collisions:
        snprintf(&local_addr.sun_path[1],
                 sizeof(local_addr.sun_path) - 1,
                 "mreq_%d", getpid());

        // Compute the length: offsetof(sun_path) + 1 + strlen("mreq_<pid>")
        socklen_t local_len = offsetof(struct sockaddr_un, sun_path)
                              + 1
                              + strlen(&local_addr.sun_path[1]);

        if (bind(sockfd, (struct sockaddr*)&local_addr, local_len) < 0) {
            perror("bind (UDS_DGRAM local)");
            close(sockfd);
            exit(EXIT_FAILURE);
        }

        // Prepare the server’s UDS address structure:
        struct sockaddr_un uds_addr;
        memset(&uds_addr, 0, sizeof(uds_addr));
        uds_addr.sun_family = AF_UNIX;
        strncpy(uds_addr.sun_path, uds_path, sizeof(uds_addr.sun_path) - 1);

        server_addr = (struct sockaddr*)&uds_addr;
        server_addr_len = sizeof(uds_addr);

        printf("client: UDS_DGRAM ready to send to %s\n", uds_path);
    }

    // 4) Print a brief help / available commands
    printf("\nAvailable commands (each on its own line):\n");
    printf("  DELIVER WATER <number>\n");
    printf("  DELIVER CARBON DIOXIDE <number>\n");
    printf("  DELIVER ALCOHOL <number>\n");
    printf("  DELIVER GLUCOSE <number>\n");
    printf("Type Ctrl+D or Ctrl+C to exit.\n\n");

    // 5) Main loop: read a line, send it, receive & print the reply
    char line[MAXDATASIZE];
    char buffer[MAXDATASIZE];

    while (fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        if (len == 0) continue;

        if (use_udp) {
            // ------ UDP send/receive ------
            if (sendto(sockfd, line, len, 0, server_addr, server_addr_len) == -1) {
                perror("sendto (UDP)");
                break;
            }
            struct sockaddr_storage their_addr;
            socklen_t addr_len = sizeof(their_addr);
            ssize_t numbytes = recvfrom(
                sockfd,
                buffer, sizeof(buffer) - 1,
                0,
                (struct sockaddr*)&their_addr,
                &addr_len
            );
            if (numbytes < 0) {
                perror("recvfrom (UDP)");
                continue;  
            }
            buffer[numbytes] = '\0';
            printf("%s", buffer);
        }
        else {
            // ------ UDS_DGRAM send/receive ------
            if (sendto(sockfd, line, len, 0, server_addr, server_addr_len) == -1) {
                perror("sendto (UDS_DGRAM)");
                break;
            }
            struct sockaddr_storage their_addr;
            socklen_t addr_len = sizeof(their_addr);
            ssize_t numbytes = recvfrom(
                sockfd,
                buffer, sizeof(buffer) - 1,
                0,
                (struct sockaddr*)&their_addr,
                &addr_len
            );
            if (numbytes < 0) {
                perror("recvfrom (UDS_DGRAM)");
                continue;
            }
            buffer[numbytes] = '\0';
            printf("%s", buffer);
        }
    }

    // 6) Clean up
    close(sockfd);
    if (use_udp) {
        free(server_addr);
    }
    printf("client: exiting\n");
    return 0;
}

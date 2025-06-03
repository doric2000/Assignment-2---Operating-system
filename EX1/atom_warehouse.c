/*
** atom_warehouse.c -- a TCP server using I/O MUX (select),
**                    storing Carbon/Oxygen/Hydrogen atoms
*/

#include <stdio.h>          // printf, fprintf
#include <stdlib.h>         // exit
#include <string.h>         // strlen, strcpy, strcmp, strtok_r, strncpy
#include <stdint.h>         // uint64_t
#include <stdbool.h>        // bool
#include <unistd.h>         // close, read, write
#include <errno.h>          // errno
#include <sys/types.h>      // socklen_t
#include <sys/socket.h>     // socket, bind, listen, accept, send, recv
#include <netdb.h>          // getaddrinfo, freeaddrinfo, struct addrinfo
#include <arpa/inet.h>      // inet_ntop
#include <sys/select.h>     // select, FD_SET, FD_ZERO, FD_ISSET

#define MAX_ATOMS ((uint64_t)1000000000000000000ULL)  // 10^18
#define BACKLOG   10                                 // max pending connections
#define MAX_CLIENTS FD_SETSIZE                        // max number of clients

// Struct that groups counts of each atom type together
typedef struct {
    uint64_t carbon;    // count of carbon atoms
    uint64_t oxygen;    // count of oxygen atoms
    uint64_t hydrogen;  // count of hydrogen atoms
} AtomStock;

// A single global variable that holds the current stock
static AtomStock stock = { 0, 0, 0 };

// Function prototypes
void parse_and_update(const char *line, char *response, size_t resp_size);
bool handle_client_command(int client_fd);
void *get_in_addr(struct sockaddr *sa);

int main(int argc, char *argv[]) {
    if (argc < 2) {                                     // require port as argument
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    const char *PORT = argv[1];                         // port to listen on

    // 1. Prepare hints for getaddrinfo
    struct addrinfo hints, *servinfo, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;        // IPv4 only
    hints.ai_socktype = SOCK_STREAM;    // TCP
    hints.ai_flags    = AI_PASSIVE;     // use my IP

    int rv;
    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        exit(1);
    }

    // 2. Create socket and bind to the first result
    int listen_fd;
    int yes = 1;
    for (p = servinfo; p != NULL; p = p->ai_next) {
        listen_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (listen_fd < 0) {
            perror("socket");
            continue;
        }
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

        if (bind(listen_fd, p->ai_addr, p->ai_addrlen) < 0) {
            close(listen_fd);
            perror("bind");
            continue;
        }
        break;  // bound successfully
    }
    freeaddrinfo(servinfo);

    if (p == NULL) {
        fprintf(stderr, "Failed to bind on port %s\n", PORT);
        exit(1);
    }

    // 3. Put socket into listening mode
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        exit(1);
    }
    printf("Server: listening on port %s...\n", PORT);

    // 4. Initialize client file descriptor array (all slots = -1)
    int client_fds[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;
    }

    // 5. Main loop using select() for I/O multiplexing
    while (1) {
        fd_set read_fds;                                 // set of FDs to watch for reading
        FD_ZERO(&read_fds);                              // clear the set
        FD_SET(listen_fd, &read_fds);                    // add listening socket
        int max_fd = listen_fd;                          // track highest FD

        // add all active client_fds to the read_fds set
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] != -1) {                   // if slot is in use
                FD_SET(client_fds[i], &read_fds);        // watch this client socket
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];              // update max_fd if needed
                }
            }
        }

        // block until one or more FDs are "hot" (ready)
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select");
            exit(1);
        }

        // 5.1. Check if the listening socket is hot (new incoming connection)
        if (FD_ISSET(listen_fd, &read_fds)) {
            struct sockaddr_storage client_addr;
            socklen_t addr_len = sizeof client_addr;
            int new_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (new_fd < 0) {
                perror("accept");
            } else {
                // add new_fd to an empty slot in client_fds[]
                bool added = false;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] == -1) {            // find free slot
                        client_fds[i] = new_fd;           // store client FD
                        added = true;
                        break;
                    }
                }
                if (!added) {                            // no free slot found
                    close(new_fd);                       // drop connection
                } else {
                    // print the new client's IP address
                    char client_ip[INET_ADDRSTRLEN];
                    struct sockaddr_in *sa = (struct sockaddr_in *)&client_addr;
                    inet_ntop(AF_INET, &sa->sin_addr, client_ip, sizeof client_ip);
                    printf("New connection from %s\n", client_ip);
                }
            }
        }

        // 5.2. For each client: if FD is hot, handle one command
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds[i];
            if (fd != -1 && FD_ISSET(fd, &read_fds)) {    // if this client socket is hot
                // handle one command; returns false if client closed or error occurred
                if (!handle_client_command(fd)) {
                    close(fd);                              // close disconnected client
                    client_fds[i] = -1;                     // mark slot as free
                }
            }
        }
    }

    // close listening socket on exit (unreachable here)
    close(listen_fd);
    return 0;
}


// handle_client_command: receive a single command from client_fd and respond.
// Returns false if client closed connection or error occurred.
bool handle_client_command(int client_fd) {
    char buffer[1024];                                   // buffer to hold incoming command
    ssize_t bytes = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes <= 0) {                                    // bytes == 0: client closed, bytes < 0: error
        return false;                                    // indicate that client is gone
    }
    buffer[bytes] = '\0';                                // null-terminate the received string

    char response[256];                                  // buffer for the response
    parse_and_update(buffer, response, sizeof response); // parse command and update the stock

    send(client_fd, response, strlen(response), 0);      // send response back to client
    return true;                                         // keep the connection open
}


// parse_and_update: given a line "ADD <TYPE> <NUM>\n", update the stock accordingly.
// Fills 'response' with either "OK: Carbon=.. Oxygen=.. Hydrogen=..\n" or "ERROR: …\n".
void parse_and_update(const char *line, char *response, size_t resp_size) {
    // copy 'line' to a mutable buffer because strtok_r modifies the string
    char temp[1024];
    strncpy(temp, line, sizeof temp);
    temp[sizeof temp - 1] = '\0';

    // tokenize input into command, type, and number
    char *saveptr;
    char *token_cmd  = strtok_r(temp, " \t\r\n", &saveptr);  // expected "ADD"
    char *token_type = strtok_r(NULL,  " \t\r\n", &saveptr);  // expected "CARBON"/"OXYGEN"/"HYDROGEN"
    char *token_num  = strtok_r(NULL,  " \t\r\n", &saveptr);  // expected a numeric string

    // if any token is missing, it's invalid
    if (!token_cmd || !token_type || !token_num) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }
    // verify the command is exactly "ADD"
    if (strcmp(token_cmd, "ADD") != 0) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }

    // identify which atom type
    enum { CARBON, OXYGEN, HYDROGEN, UNKNOWN } type = UNKNOWN;
    if (strcmp(token_type, "CARBON") == 0) {
        type = CARBON;
    } else if (strcmp(token_type, "OXYGEN") == 0) {
        type = OXYGEN;
    } else if (strcmp(token_type, "HYDROGEN") == 0) {
        type = HYDROGEN;
    } else {
        snprintf(response, resp_size, "ERROR: invalid atom type\n");
        return;
    }

    // convert the numeric string to unsigned long long
    char *endptr;
    unsigned long long val = strtoull(token_num, &endptr, 10);
    // check for conversion errors (non-numeric or leftover characters)
    if (endptr == token_num || *endptr != '\0') {
        snprintf(response, resp_size, "ERROR: invalid number\n");
        return;
    }
    // check if the provided number alone exceeds MAX_ATOMS
    if (val > MAX_ATOMS) {
        snprintf(response, resp_size, "ERROR: number too large\n");
        return;
    }

    // attempt to update the stock, checking for overflow
    switch (type) {
        case CARBON:
            if (stock.carbon + val > MAX_ATOMS) {
                snprintf(response, resp_size, "ERROR: capacity exceeded\n");
                return;
            }
            stock.carbon += val;
            break;
        case OXYGEN:
            if (stock.oxygen + val > MAX_ATOMS) {
                snprintf(response, resp_size, "ERROR: capacity exceeded\n");
                return;
            }
            stock.oxygen += val;
            break;
        case HYDROGEN:
            if (stock.hydrogen + val > MAX_ATOMS) {
                snprintf(response, resp_size, "ERROR: capacity exceeded\n");
                return;
            }
            stock.hydrogen += val;
            break;
        default:
            snprintf(response, resp_size, "ERROR: unexpected error\n");
            return;
    }

    // build a success response showing the updated stock
    snprintf(response, resp_size,
             "OK: Carbon=%llu Oxygen=%llu Hydrogen=%llu\n",
             (unsigned long long)stock.carbon,
             (unsigned long long)stock.oxygen,
             (unsigned long long)stock.hydrogen);
}


// get_in_addr: return pointer to the IPv4 address portion of sockaddr
void *get_in_addr(struct sockaddr *sa) {
    return &(((struct sockaddr_in*)sa)->sin_addr);
}

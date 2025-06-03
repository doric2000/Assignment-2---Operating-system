/*
** drinks_bar.c -- a combined TCP+UDP server for:
**   • TCP ADD CARBON/ADD OXYGEN/ADD HYDROGEN (Stage 1)
**   • UDP DELIVER WATER/CARBON DIOXIDE/GLUCOSE/ALCOHOL (Stage 2)
**   • can receive commands from keyboard , will inform how much beverages can be made with our supplies (SOFT DRINK , VODKA, CHAMPAGNE) (Stage 3)
  
*/

#include <stdio.h>           // for printf, fprintf, perror
#include <stdlib.h>          // for exit, malloc, free
#include <string.h>          // for strlen, strcmp, strtok_r, strncpy, snprintf, memset, memcpy
#include <stdint.h>          // for uint64_t type
#include <stdbool.h>         // for bool, true, false
#include <unistd.h>          // for close, fork
#include <errno.h>           // for errno
#include <sys/types.h>       // for socklen_t
#include <sys/socket.h>      // for socket, bind, listen, accept, send, recv, sendto, recvfrom
#include <netdb.h>           // for getaddrinfo, freeaddrinfo, struct addrinfo
#include <arpa/inet.h>       // for inet_ntop
#include <netinet/in.h>      // for struct sockaddr_in, struct sockaddr_in6
#include <sys/select.h>      // for select, fd_set, FD_ZERO, FD_SET, FD_ISSET
#include <sys/wait.h>        // for waitpid, WNOHANG in SIGCHLD handler
#include <signal.h>          // for sigaction, SIGCHLD
#include <getopt.h>          // for getopt_long


#define MAX_ATOMS  ((uint64_t)1000000000000000000ULL)  // 10^18 maximum quantity
#define BACKLOG    10                                   // TCP listen backlog
#define MAX_CLIENTS FD_SETSIZE                           // maximum number of simultaneous TCP clients
#define MAXBUF     1024                                  // buffer size for receiving/sending



// Struct to store counts of each atom type (Stage 1)
typedef struct {
    uint64_t carbon;    // count of carbon atoms
    uint64_t oxygen;    // count of oxygen atoms
    uint64_t hydrogen;  // count of hydrogen atoms
} AtomStock;


// Global stocks for atoms and molecules (initialized to zero)
static AtomStock     atom_stock     = { 0, 0, 0 };


// Function prototypes
void sigchld_handler(int s);  // handler to reap zombie processes
void *get_in_addr(struct sockaddr *sa);  // get IPv4 or IPv6 address from sockaddr
bool handle_tcp_client(int client_fd);   // process one TCP client command
void parse_and_update_tcp(const char *line, char *response, size_t resp_size);     // parse "ADD ..." and update atom_stock
void parse_and_update_udp(const char *line, char *response, size_t resp_size);     // parse "DELIVER ..." and update molecule_stock

// SIGCHLD handler to reap child processes
void sigchld_handler(int s) {
    (void)s;  // suppress unused parameter warning
    int saved_errno = errno;                 // save errno, because waitpid may change it
    while (waitpid(-1, NULL, WNOHANG) > 0) { }  // reap all child processes
    errno = saved_errno;                     // restore errno
}

// get_in_addr: return pointer to IPv4 or IPv6 address inside sockaddr
void *get_in_addr(struct sockaddr *sa) {
    if (sa->sa_family == AF_INET) {          // if IPv4
        return &(((struct sockaddr_in*)sa)->sin_addr);  // return pointer to sin_addr
    }
    return &(((struct sockaddr_in6*)sa)->sin6_addr);   // else return pointer to sin6_addr for IPv6
}

int main(int argc, char *argv[]) {
    if (argc != 2) {                         // expect exactly one argument: port
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);  // print usage message
        exit(EXIT_FAILURE);                  // exit with failure
    }
    const char *PORT = argv[1];              // port on which server listens (both TCP and UDP)


    // --------------------------------------------------
    // 1. Create TCP listening socket (Stage 1)
    // --------------------------------------------------
    int tcp_listen_fd;                       // file descriptor for TCP listening socket
    struct addrinfo hints_tcp, *servinfo_tcp, *p_tcp;
    memset(&hints_tcp, 0, sizeof hints_tcp); // zero out hints
    hints_tcp.ai_family   = AF_INET;         // use IPv4 only
    hints_tcp.ai_socktype = SOCK_STREAM;     // TCP
    hints_tcp.ai_flags    = AI_PASSIVE;      // use local IP

    int rv;
    if ((rv = getaddrinfo(NULL, PORT, &hints_tcp, &servinfo_tcp)) != 0) {
        fprintf(stderr, "getaddrinfo (TCP): %s\n", gai_strerror(rv));  // print error if getaddrinfo fails
        exit(1);
    }
    int yes = 1;                             // for setsockopt option
    for (p_tcp = servinfo_tcp; p_tcp != NULL; p_tcp = p_tcp->ai_next) {
        tcp_listen_fd = socket(p_tcp->ai_family, p_tcp->ai_socktype, p_tcp->ai_protocol);  // create TCP socket
        if (tcp_listen_fd < 0) {
            perror("server: tcp socket");     // on error, print and continue
            continue;
        }
        if (setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
            perror("setsockopt (TCP)");       // allow reusing address after program exits
            close(tcp_listen_fd);
            exit(1);
        }
        if (bind(tcp_listen_fd, p_tcp->ai_addr, p_tcp->ai_addrlen) < 0) {
            close(tcp_listen_fd);             // bind failed, close socket
            perror("server: tcp bind");       // print bind error
            continue;
        }
        break;  // bound successfully
    }
    if (p_tcp == NULL) {
        fprintf(stderr, "server: failed to bind TCP on port %s\n", PORT);  // no address succeeded
        exit(1);
    }
    freeaddrinfo(servinfo_tcp);             // free the linked list

    if (listen(tcp_listen_fd, BACKLOG) < 0) {
        perror("listen (TCP)");              // put socket into listening mode
        exit(1);
    }
    printf("server (TCP): listening on port %s ...\n", PORT); // log listening status

    // Set up SIGCHLD handler to reap dead child processes (if using fork())
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;        // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;               // restart interrupted system calls
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");                // on failure, print and exit
        exit(1);
    }

    // ------------------------------------------------------------------
    // 2. Create UDP socket on the same port (Stage 2)
    // ------------------------------------------------------------------
    int udp_fd;                              // file descriptor for UDP socket
    struct addrinfo hints_udp, *servinfo_udp, *p_udp;
    memset(&hints_udp, 0, sizeof hints_udp); // zero out hints_udp
    hints_udp.ai_family   = AF_INET;         // IPv4 only
    hints_udp.ai_socktype = SOCK_DGRAM;      // UDP
    hints_udp.ai_flags    = AI_PASSIVE;      // use local IP

    if ((rv = getaddrinfo(NULL, PORT, &hints_udp, &servinfo_udp)) != 0) {
        fprintf(stderr, "getaddrinfo (UDP): %s\n", gai_strerror(rv)); // print error if getaddrinfo fails
        exit(1);
    }
    for (p_udp = servinfo_udp; p_udp != NULL; p_udp = p_udp->ai_next) {
        udp_fd = socket(p_udp->ai_family, p_udp->ai_socktype, p_udp->ai_protocol);  // create UDP socket
        if (udp_fd < 0) {
            perror("server: udp socket");    // on error, print and continue
            continue;
        }
        if (bind(udp_fd, p_udp->ai_addr, p_udp->ai_addrlen) < 0) {
            close(udp_fd);                    // bind failed, close socket
            perror("server: udp bind");       // print bind error
            continue;
        }
        break;  // bound successfully
    }
    if (p_udp == NULL) {
        fprintf(stderr, "server: failed to bind UDP on port %s\n", PORT); // no address succeeded
        exit(1);
    }
    freeaddrinfo(servinfo_udp);             // free the linked list
    printf("server (UDP): listening on port %s ...\n", PORT); // log listening status

    // ------------------------------------------------------
    // 3. Initialize array of TCP client file descriptors
    // ------------------------------------------------------
    int client_fds[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;                  // -1 indicates empty slot
    }

    // ------------------------------------------------------
    // 4. Print prompt of valid operations on the server console
    // ------------------------------------------------------
    printf("\n=== DRINKS_BAR SERVER READY ===\n");
    printf("Valid console commands (type here):\n");
    printf("  GEN SOFT DRINK\n");
    printf("  GEN VODKA\n");
    printf("  GEN CHAMPAGNE\n\n");
    printf("Press Ctrl+C to terminate.\n\n");


    // ---------------------------------------------------------
    // 5. Main select() loop to monitor TCP listen, UDP, and TCP clients
    // ---------------------------------------------------------
    while (1) {
        fd_set read_fds;                     // set of file descriptors to watch for reading
        FD_ZERO(&read_fds);                  // clear the set

        // a) Add TCP listening socket to read_fds
        FD_SET(tcp_listen_fd, &read_fds);
        int max_fd = tcp_listen_fd;          // track highest fd

        // b) Add UDP socket to read_fds
        FD_SET(udp_fd, &read_fds);
        if (udp_fd > max_fd) max_fd = udp_fd;

        // c) Add all active TCP client sockets to read_fds
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds[i];
            if (fd != -1) {
                FD_SET(fd, &read_fds);
                if (fd > max_fd) max_fd = fd;
            }
        }

        // d) Add STDIN (keyboard) to read_fds
        FD_SET(STDIN_FILENO, &read_fds);
        if (STDIN_FILENO > max_fd) max_fd = STDIN_FILENO;

        // Block until at least one socket is ready
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            perror("select");                 // print error and exit on failure
            exit(1);
        }

        // ---------------------------------------
        // 4.1 Check if the TCP listening socket is ready (new incoming connection)
        // ---------------------------------------
        if (FD_ISSET(tcp_listen_fd, &read_fds)) {
            struct sockaddr_storage client_addr;  // connector's address information
            socklen_t addr_len = sizeof client_addr;
            int new_fd = accept(tcp_listen_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (new_fd < 0) {
                perror("accept");              // error on accept
            } else {
                // add new_fd to an empty slot in client_fds[]
                bool added = false;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] == -1) {  // find free slot
                        client_fds[i] = new_fd; // store client fd
                        added = true;
                        break;
                    }
                }
                if (!added) {                   // no free slot found
                    close(new_fd);              // drop the connection
                } else {
                    // print the new client's IP address
                    char client_ip[INET_ADDRSTRLEN];
                    struct sockaddr_in *sa = (struct sockaddr_in *)&client_addr;
                    inet_ntop(AF_INET, &sa->sin_addr, client_ip, sizeof client_ip);
                    printf("New TCP client from %s\n", client_ip);
                }
            }
        }

        // ---------------------------------------
        // 4.2 Check if the UDP socket is ready (incoming DELIVER request)
        // ---------------------------------------
        if (FD_ISSET(udp_fd, &read_fds)) {
            char buf[MAXBUF];                  // buffer to hold incoming message
            struct sockaddr_storage client_addr; 
            socklen_t addr_len = sizeof client_addr;
            ssize_t numbytes = recvfrom(
                udp_fd,
                buf, sizeof(buf) - 1,
                0,
                (struct sockaddr*)&client_addr,
                &addr_len
            );
            if (numbytes < 0) {
                perror("recvfrom (UDP)");     // error on recvfrom
            } else {
                buf[numbytes] = '\0';         // null-terminate the received string

                // parse the UDP command (DELIVER ...) and form response
                char response[MAXBUF];
                parse_and_update_udp(buf, response, sizeof response);

                // send response back to the UDP client
                ssize_t sent = sendto(
                    udp_fd,
                    response, strlen(response),
                    0,
                    (struct sockaddr*)&client_addr,
                    addr_len
                );
                if (sent < 0) {
                    perror("sendto (UDP)");   // error sending response
                }
            }
        }

        // --------------------------------------------------------
        // 4.3 Check each TCP client: if ready to read, handle one command
        // --------------------------------------------------------
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds[i];
            if (fd == -1) continue;         // skip empty slots
            if (FD_ISSET(fd, &read_fds)) {  // if this client socket is ready
                // handle one command; returns false if client closed or error occurred
                if (!handle_tcp_client(fd)) {
                    close(fd);               // close disconnected or errored client
                    client_fds[i] = -1;      // mark slot as free
                }
            }
        }

        // --------------------------------------------------------
        // 4.4 Check if STDIN (keyboard) is ready, handle one command
        // --------------------------------------------------------

        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            char line[MAXBUF];

            if (fgets(line, sizeof(line), stdin) != NULL) {
                size_t len = strlen(line);
                if (len > 0 && line[len - 1] == '\n') {
                    line[len - 1] = '\0';
                }

                char *cmd = strtok(line, " \t");
                if (!cmd || strcmp(cmd, "GEN") != 0) {
                    printf("ERROR: invalid console command\n");
                } else {
                    char *drink = strtok(NULL, " \t");
                    if (!drink) {
                        printf("ERROR: missing drink type after GEN\n");
                    } else if (strcmp(drink, "SOFT") == 0) {
                        char *maybe_drink = strtok(NULL, " \t");
                        if (!maybe_drink || strcmp(maybe_drink, "DRINK") != 0) {
                            printf("ERROR: did you mean 'GEN SOFT DRINK'?\n");
                        } else {
                            // SOFT DRINK = 6C + 14H + 9O
                            uint64_t c = atom_stock.carbon / 6;
                            uint64_t h = atom_stock.hydrogen / 14;
                            uint64_t o = atom_stock.oxygen / 9;
                            uint64_t can_make = c;
                            if (h < can_make) can_make = h;
                            if (o < can_make) can_make = o;
                            printf("You can make up to %llu SOFT DRINK(s)\n", (unsigned long long)can_make);
                        }
                    } else if (strcmp(drink, "VODKA") == 0) {
                        // VODKA = 8C + 20H + 8O
                        uint64_t c = atom_stock.carbon / 8;
                        uint64_t h = atom_stock.hydrogen / 20;
                        uint64_t o = atom_stock.oxygen / 8;
                        uint64_t can_make = c;
                        if (h < can_make) can_make = h;
                        if (o < can_make) can_make = o;
                        printf("You can make up to %llu VODKA(s)\n", (unsigned long long)can_make);
                    } else if (strcmp(drink, "CHAMPAGNE") == 0) {
                        // CHAMPAGNE = 3C + 9H + 4O
                        uint64_t c = atom_stock.carbon / 3;
                        uint64_t h = atom_stock.hydrogen / 9;
                        uint64_t o = atom_stock.oxygen / 4;
                        uint64_t can_make = c;
                        if (h < can_make) can_make = h;
                        if (o < can_make) can_make = o;
                        printf("You can make up to %llu CHAMPAGNE(s)\n", (unsigned long long)can_make);
                    } else {
                        printf("ERROR: unknown drink type '%s'\n", drink);
                    }
                }
            } else {
                printf("Console closed or error – exiting.\n");
                break;
            }
        }

    }  // end of while (1)

    // We never reach here because of infinite loop, but for completeness:
    close(tcp_listen_fd);
    close(udp_fd);
    return 0;
}
// ----------------------------------------------------
// handle_tcp_client:
//   receive a single command from client_fd via TCP, respond, 
//   return false if client closed or error occurred
// ----------------------------------------------------
bool handle_tcp_client(int client_fd) {
    char buf[MAXBUF];
    ssize_t numbytes = recv(client_fd, buf, sizeof(buf) - 1, 0);  // read up to MAXBUF-1 bytes
    if (numbytes <= 0) {               // numbytes == 0 means client closed, numbytes < 0 means error
        return false;                  // indicate client is gone
    }
    buf[numbytes] = '\0';              // null-terminate the received string

    // parse and update the TCP command (ADD ...)
    char response[MAXBUF];
    parse_and_update_tcp(buf, response, sizeof response);

    // send response back to the TCP client
    if (send(client_fd, response, strlen(response), 0) < 0) {
        perror("send (TCP)");          // print error if send fails
    }
    return true;                       // keep connection open
}

// ----------------------------------------------------
// parse_and_update_tcp:
//   parse a line "ADD <TYPE> <NUM>\n", update atom_stock accordingly,
//   fill response with either "OK: Carbon=.. Oxygen=.. Hydrogen=..\n"
//   or "ERROR: …\n"
// ----------------------------------------------------
void parse_and_update_tcp(const char *line, char *response, size_t resp_size) {
    char temp[MAXBUF];
    strncpy(temp, line, sizeof temp);           // copy line to mutable buffer
    temp[sizeof temp - 1] = '\0';               // ensure null-termination

    char *saveptr;
    char *token_cmd  = strtok_r(temp, " \t\r\n", &saveptr);  // expected "ADD"
    char *token_type = strtok_r(NULL,  " \t\r\n", &saveptr); // expected "CARBON"/"OXYGEN"/"HYDROGEN"
    char *token_num  = strtok_r(NULL,  " \t\r\n", &saveptr); // expected numeric string

    // if any token is missing, invalid command
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
    enum { CARBON, OXYGEN, HYDROGEN, TYPE_UNKNOWN } type = TYPE_UNKNOWN;
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

    // convert numeric string to unsigned long long
    char *endptr;
    unsigned long long val = strtoull(token_num, &endptr, 10);
    if (endptr == token_num || *endptr != '\0') {
        snprintf(response, resp_size, "ERROR: invalid number\n");
        return;
    }
    // check if provided number alone exceeds MAX_ATOMS
    if (val > MAX_ATOMS) {
        snprintf(response, resp_size, "ERROR: number too large\n");
        return;
    }

    // attempt to update the stock, checking for overflow
    switch (type) {
        case CARBON:
            if (atom_stock.carbon + val > MAX_ATOMS) {
                snprintf(response, resp_size, "ERROR: capacity exceeded\n");
                return;
            }
            atom_stock.carbon += val;
            break;
        case OXYGEN:
            if (atom_stock.oxygen + val > MAX_ATOMS) {
                snprintf(response, resp_size, "ERROR: capacity exceeded\n");
                return;
            }
            atom_stock.oxygen += val;
            break;
        case HYDROGEN:
            if (atom_stock.hydrogen + val > MAX_ATOMS) {
                snprintf(response, resp_size, "ERROR: capacity exceeded\n");
                return;
            }
            atom_stock.hydrogen += val;
            break;
        default:
            snprintf(response, resp_size, "ERROR: unknown error\n");
            return;
    }

    //printing atoms status before responding to client.
    printf(
        "SERVER INVENTORY (atoms): Carbon=%llu  Oxygen=%llu  Hydrogen=%llu\n",
        (unsigned long long)atom_stock.carbon,
        (unsigned long long)atom_stock.oxygen,
        (unsigned long long)atom_stock.hydrogen
    );

    // build success response with updated stock
    snprintf(response, resp_size,
             "OK: Carbon=%llu Oxygen=%llu Hydrogen=%llu\n",
             (unsigned long long)atom_stock.carbon,
             (unsigned long long)atom_stock.oxygen,
             (unsigned long long)atom_stock.hydrogen);
}

// ----------------------------------------------------
// parse_and_update_udp:
//   parse a line "DELIVER <MOLECULE> <NUM>\n", check if there
//   are enough atoms to build that many molecules, subtract
//   the required atoms if possible, update molecule_stock,
//   and fill 'response' with either "OK: ..." or "ERROR: ...\n"
// ----------------------------------------------------
void parse_and_update_udp(const char *line, char *response, size_t resp_size) {
    char temp[MAXBUF];
    // Copy the incoming line into a mutable buffer (strtok_r modifies it)
    strncpy(temp, line, sizeof temp);
    temp[sizeof temp - 1] = '\0';                    // Ensure null-termination

    char *saveptr;
    // Extract the first token: expected to be "DELIVER"
    char *token_cmd = strtok_r(temp, " \t\r\n", &saveptr);
    // Extract the second token: the first part of the molecule name (e.g., "WATER" or "CARBON")
    char *token_mol = strtok_r(NULL, " \t\r\n", &saveptr);

    // If either "DELIVER" or the molecule name is missing, it’s an invalid command
    if (!token_cmd || !token_mol) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }

    // Verify that the first token is exactly "DELIVER"
    if (strcmp(token_cmd, "DELIVER") != 0) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }

    // Build the full molecule name in 'full_mol'
    char full_mol[MAXBUF];
    if (strcmp(token_mol, "CARBON") == 0) {
        // If the token was "CARBON", expect the next token to be "DIOXIDE"
        char *token_next = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!token_next || strcmp(token_next, "DIOXIDE") != 0) {
            // If not "DIOXIDE", the molecule name is invalid
            snprintf(response, resp_size, "ERROR: invalid molecule type\n");
            return;
        }
        // Set full_mol to "CARBON DIOXIDE"
        strcpy(full_mol, "CARBON DIOXIDE");
    } else if (strcmp(token_mol, "WATER") == 0) {
        // Single-word molecule "WATER"
        strcpy(full_mol, "WATER");
    } else if (strcmp(token_mol, "GLUCOSE") == 0) {
        // Single-word molecule "GLUCOSE"
        strcpy(full_mol, "GLUCOSE");
    } else if (strcmp(token_mol, "ALCOHOL") == 0) {
        // Single-word molecule "ALCOHOL"
        strcpy(full_mol, "ALCOHOL");
    } else {
        // Any other token is not a recognized molecule
        snprintf(response, resp_size, "ERROR: invalid molecule type\n");
        return;
    }

    // Next token should be the requested number (as a string)
    char *token_num = strtok_r(NULL, " \t\r\n", &saveptr);
    if (!token_num) {
        // No number provided after the molecule name
        snprintf(response, resp_size, "ERROR: missing number\n");
        return;
    }

    // Ensure there are no extra tokens beyond the number
    char *token_extra = strtok_r(NULL, " \t\r\n", &saveptr);
    if (token_extra) {
        // Extra garbage after the number is not allowed
        snprintf(response, resp_size, "ERROR: too many arguments\n");
        return;
    }

    // Convert the numeric string to unsigned long long
    char *endptr;
    unsigned long long count = strtoull(token_num, &endptr, 10);
    if (endptr == token_num || *endptr != '\0') {
        // If conversion failed or leftover characters exist
        snprintf(response, resp_size, "ERROR: invalid number\n");
        return;
    }
    // If the requested count exceeds our absolute maximum
    if (count > MAX_ATOMS) {
        snprintf(response, resp_size, "ERROR: number too large\n");
        return;
    }

    // Now check and subtract atoms from atom_stock according to the molecule formula:
    //   - WATER (H2O) needs 2 hydrogen + 1 oxygen per molecule
    //   - CARBON DIOXIDE (CO2) needs 1 carbon + 2 oxygen per molecule
    //   - GLUCOSE (C6H12O6) needs 6 carbon + 12 hydrogen + 6 oxygen per molecule
    //   - ALCOHOL (C2H6O) needs 2 carbon + 6 hydrogen + 1 oxygen per molecule

    uint64_t req_carbon = 0, req_oxygen = 0, req_hydrogen = 0;

    if (strcmp(full_mol, "WATER") == 0) {
        // Each H2O needs 2 H and 1 O
        req_hydrogen = 2ULL * count;
        req_oxygen   = 1ULL * count;
        req_carbon   = 0;
    } else if (strcmp(full_mol, "CARBON DIOXIDE") == 0) {
        // Each CO2 needs 1 C and 2 O
        req_carbon   = 1ULL * count;
        req_oxygen   = 2ULL * count;
        req_hydrogen = 0;
    } else if (strcmp(full_mol, "GLUCOSE") == 0) {
        // Each C6H12O6 needs 6 C, 12 H, 6 O
        req_carbon   = 6ULL * count;
        req_hydrogen = 12ULL * count;
        req_oxygen   = 6ULL * count;
    } else if (strcmp(full_mol, "ALCOHOL") == 0) {
        // Each C2H6O needs 2 C, 6 H, 1 O
        req_carbon   = 2ULL * count;
        req_hydrogen = 6ULL * count;
        req_oxygen   = 1ULL * count;
    } else {
        // Should never happen, but in case we missed a condition
        snprintf(response, resp_size, "ERROR: unknown molecule\n");
        return;
    }

    // Check if we have enough atoms in atom_stock to fulfill the request
    if (atom_stock.carbon < req_carbon) {
        snprintf(response, resp_size, "ERROR: not enough carbon atoms\n");
        return;
    }
    if (atom_stock.oxygen < req_oxygen) {
        snprintf(response, resp_size, "ERROR: not enough oxygen atoms\n");
        return;
    }
    if (atom_stock.hydrogen < req_hydrogen) {
        snprintf(response, resp_size, "ERROR: not enough hydrogen atoms\n");
        return;
    }

    // We have enough atoms—subtract them now
    atom_stock.carbon   -= req_carbon;
    atom_stock.oxygen   -= req_oxygen;
    atom_stock.hydrogen -= req_hydrogen;

    // --------------------------------------------------------------
    // Print both atoms  inventory after any “DELIVER …” request
    // --------------------------------------------------------------
    printf(
        "SERVER INVENTORY (atoms):   Carbon=%llu  Oxygen=%llu  Hydrogen=%llu\n",
        (unsigned long long)atom_stock.carbon,
        (unsigned long long)atom_stock.oxygen,
        (unsigned long long)atom_stock.hydrogen
    );

    // Build a success response that lists the remaining atom stock
    snprintf(response, resp_size,
             "OK: Atoms left – Carbon=%llu Oxygen=%llu Hydrogen=%llu\n",
             (unsigned long long)atom_stock.carbon,
             (unsigned long long)atom_stock.oxygen,
             (unsigned long long)atom_stock.hydrogen);
}

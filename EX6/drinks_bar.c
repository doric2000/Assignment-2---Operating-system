/*
** drinks_bar.c -- a combined TCP+UDP server for:
**   • TCP ADD CARBON / ADD OXYGEN / ADD HYDROGEN (Stage 1)
**   • UDP DELIVER WATER / CARBON DIOXIDE / GLUCOSE / ALCOHOL (Stage 2)
**   • console commands to tell how many beverages (SOFT DRINK, VODKA, CHAMPAGNE) can be made (Stage 3)
**   • optionally also accept UDS‐STREAM (‐s) or UDS‐DGRAM (‐d) like UDP/TCP
**
** Mandatory flags: 
**   -c <initial_carbon> 
**   -o <initial_oxygen> 
**   -h <initial_hydrogen>
**   -T <tcp_port> 
**   -U <udp_port>
**
** Optional: 
**   -t <timeout_seconds>
**   -s <uds_stream_path>   (if you want a Unix‐domain STREAM socket in addition to TCP+UDP)
**   -d <uds_dgram_path>    (if you want a Unix‐domain DGRAM socket in addition to TCP+UDP)
**
** Examples:
**   ./drinks_bar -c 100 -o 50 -h 200 -T 5555 -U 6666
**     (only TCP on 5555 and UDP on 6666)
**
**   ./drinks_bar -c 10 -o 10 -h 10 -T 5555 -U 6666 -s /tmp/my_stream.sock
**     (TCP/UDP plus a UDS-STREAM socket at /tmp/my_stream.sock)
**
**   ./drinks_bar -c 10 -o 10 -h 10 -T 5555 -U 6666 -d /tmp/my_dgram.sock
**     (TCP/UDP plus a UDS-DGRAM socket at /tmp/my_dgram.sock)
**
*/

#include <stdio.h>           // printf, fprintf, perror
#include <stdlib.h>          // exit, malloc, free
#include <string.h>          // strlen, strcmp, strtok_r, strncpy, snprintf, memset, memcpy
#include <stdint.h>          // uint64_t
#include <stdbool.h>         // bool, true, false
#include <unistd.h>          // close, fork, write, unlink, STDIN_FILENO
#include <errno.h>           // errno
#include <sys/types.h>       // socklen_t
#include <sys/socket.h>      // socket, bind, listen, accept, send, recv, sendto, recvfrom
#include <sys/un.h>          // sockaddr_un
#include <netdb.h>           // getaddrinfo, freeaddrinfo, struct addrinfo
#include <arpa/inet.h>       // inet_ntop
#include <netinet/in.h>      // sockaddr_in, sockaddr_in6
#include <sys/select.h>      // select, fd_set, FD_ZERO, FD_SET, FD_ISSET
#include <sys/wait.h>        // waitpid, WNOHANG
#include <signal.h>          // sigaction, SIGCHLD, SIGALRM
#include <getopt.h>          // getopt_long
#include <stddef.h>          // offsetof
#include <sys/file.h>   // flock
#include <fcntl.h>
#include <sys/stat.h>

#define MAX_ATOMS  ((uint64_t)1000000000000000000ULL)  // 10^18 maximum quantity
#define BACKLOG    10                                   // TCP listen backlog
#define MAX_CLIENTS FD_SETSIZE                           // max simultaneous TCP clients
#define MAXBUF     1024                                  // buffer size for recv/send

// ----------------------------------------------------------------------------
// Struct to store counts of each atom type (Stage 1)
// ----------------------------------------------------------------------------
typedef struct {
    uint64_t carbon;    // count of carbon atoms
    uint64_t oxygen;    // count of oxygen atoms
    uint64_t hydrogen;  // count of hydrogen atoms
} AtomStock;


// Global atomic stock (initialized via flags -c, -o, -h)
static AtomStock atom_stock = { 0, 0, 0 };

// A simple flag set by SIGALRM to signal “timeout” (Stage 4)
static volatile sig_atomic_t timed_out = 0;

//if we will have -f flag than we will save here the path of the file to load/save the atoms from.
static char *save_file_path = NULL;

// ----------------------------------------------------------------------------
// Prototypes
// ----------------------------------------------------------------------------

// SIGCHLD handler: reap any zombie children
void sigchld_handler(int sig);

// SIGALRM handler: mark that timeout occurred
void alarm_handler(int sig);

// Get the “address field” portion (IPv4 or IPv6) from sockaddr*
void *get_in_addr(struct sockaddr *sa);

// Print the current stock of atoms to stdout
void print_inventory(void);

// Handle exactly one TCP client command on `client_fd` (an “ADD …” line).
// - Reads one line, parses “ADD <TYPE> <NUM>\n”
// - Updates atom_stock
// - Sends back either “OK: Carbon=… Oxygen=… Hydrogen=…\n” or “ERROR: …\n”
// Returns false if the client closed connection or a read‐error occurred.
bool handle_tcp_client(int client_fd);

// Parse a single “ADD <TYPE> <NUM>” line (no trailing newline), update atom_stock.
// Fill `response` with either
//   “OK: Carbon=.. Oxygen=.. Hydrogen=..\n”
// or “ERROR: ...\n”
void parse_and_update_tcp(const char *line, char *response, size_t resp_size);

// Parse a single “DELIVER <MOLECULE> <NUM>” line, check atom stock,
// subtract required atoms if possible, and fill `response` with
//   “OK: Atoms left – Carbon=.. Oxygen=.. Hydrogen=..\n”
// or “ERROR: ...\n”
void parse_and_update_udp(const char *line, char *response, size_t resp_size);

//if the file exists and big enough , reads sizeof (atomStock) to the global var.
//else creating a new file , fills it with the values of the atoms and read the full struct to the file.
static void load_atoms_from_file(const char *path, uint64_t init_c,uint64_t init_o,uint64_t init_h);


//opens / creates the file in rb or wb. locks the file with flock to prevent parallel changes.
//writes a block of our atoms struct with fwrite.
//releases the lock and closes the files.
static void save_atoms_to_file(const char *path);

// ----------------------------------------------------------------------------
// SIGALRM handler: marks that we timed out (no activity for <timeout> seconds).
// We use write() (async‐signal safe) just to print a quick message.
// ----------------------------------------------------------------------------
void alarm_handler(int sig) {
    (void)sig;
    const char msg[] = ">>> Alarm handler invoked! Server shutting down due to inactivity.\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    timed_out = 1;
}

// ----------------------------------------------------------------------------
// Print the current atom inventory on stdout.
// ----------------------------------------------------------------------------
void print_inventory(void) {
    printf("SERVER INVENTORY (atoms): Carbon=%llu  Oxygen=%llu  Hydrogen=%llu\n",
           (unsigned long long)atom_stock.carbon,
           (unsigned long long)atom_stock.oxygen,
           (unsigned long long)atom_stock.hydrogen);
}

// ----------------------------------------------------------------------------
// Parse and update a TCP “ADD <TYPE> <NUM>” command.
// Fills `response` with either “OK: Carbon=.. Oxygen=.. Hydrogen=..\n”
// or an ERROR line.
// ----------------------------------------------------------------------------
void parse_and_update_tcp(const char *line, char *response, size_t resp_size) {

    if (save_file_path) {
        load_atoms_from_file(save_file_path, 0, 0, 0);
    }
    
    
    char temp[MAXBUF];
    strncpy(temp, line, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';

    char *saveptr = NULL;
    char *token_cmd  = strtok_r(temp, " \t\r\n", &saveptr);  // “ADD”
    char *token_type = strtok_r(NULL,   " \t\r\n", &saveptr);  // “CARBON”|“OXYGEN”|“HYDROGEN”
    char *token_num  = strtok_r(NULL,   " \t\r\n", &saveptr);  // e.g. “100”

    if (!token_cmd || !token_type || !token_num) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }
    if (strcmp(token_cmd, "ADD") != 0) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }

    enum { CARBON, OXYGEN, HYDROGEN, TYPE_UNKNOWN } type = TYPE_UNKNOWN;
    if (strcmp(token_type, "CARBON") == 0)      type = CARBON;
    else if (strcmp(token_type, "OXYGEN") == 0) type = OXYGEN;
    else if (strcmp(token_type, "HYDROGEN") == 0) type = HYDROGEN;
    else {
        snprintf(response, resp_size, "ERROR: invalid atom type\n");
        return;
    }

    char *endptr = NULL;
    unsigned long long val = strtoull(token_num, &endptr, 10);
    if (endptr == token_num || *endptr != '\0') {
        snprintf(response, resp_size, "ERROR: invalid number\n");
        return;
    }
    if (val > MAX_ATOMS) {
        snprintf(response, resp_size, "ERROR: number too large\n");
        return;
    }

    // Attempt to add to the correct stock, checking for overflow.
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

    // Print updated atom inventory to server console
    printf("SERVER INVENTORY (atoms): Carbon=%llu  Oxygen=%llu  Hydrogen=%llu\n",
           (unsigned long long)atom_stock.carbon,
           (unsigned long long)atom_stock.oxygen,
           (unsigned long long)atom_stock.hydrogen);

    //if there is a save flag , we will save the atoms to the file.
    if (save_file_path) {
        save_atoms_to_file(save_file_path);
    }

    // Build success response
    snprintf(response, resp_size,
             "OK: Carbon=%llu Oxygen=%llu Hydrogen=%llu\n",
             (unsigned long long)atom_stock.carbon,
             (unsigned long long)atom_stock.oxygen,
             (unsigned long long)atom_stock.hydrogen);
}

// ----------------------------------------------------------------------------
// Parse and update a UDP “DELIVER <MOLECULE> <NUM>” command.
// Molecule → needs certain numbers of atoms; subtract if enough atoms; else error.
// Print the resulting inventory, then respond with a short “OK: Atoms left …\n”
// ----------------------------------------------------------------------------
void parse_and_update_udp(const char *line, char *response, size_t resp_size) {  
    
    if (save_file_path) {
        load_atoms_from_file(save_file_path, 0, 0, 0);
    }
    
    char temp[MAXBUF];
    strncpy(temp, line, sizeof(temp));
    temp[sizeof(temp)-1] = '\0';

    char *saveptr = NULL;
    char *token_cmd = strtok_r(temp, " \t\r\n", &saveptr);  // “DELIVER”
    char *token_mol = strtok_r(NULL,   " \t\r\n", &saveptr);  // e.g. “WATER” or “CARBON”

    if (!token_cmd || !token_mol) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }
    if (strcmp(token_cmd, "DELIVER") != 0) {
        snprintf(response, resp_size, "ERROR: invalid command\n");
        return;
    }

    // Build full molecule name if needed (“CARBON DIOXIDE” → two tokens)
    char full_mol[MAXBUF];
    if (strcmp(token_mol, "CARBON") == 0) {
        char *token_next = strtok_r(NULL, " \t\r\n", &saveptr);
        if (!token_next || strcmp(token_next, "DIOXIDE") != 0) {
            snprintf(response, resp_size, "ERROR: invalid molecule type\n");
            return;
        }
        strcpy(full_mol, "CARBON DIOXIDE");
    }
    else if (strcmp(token_mol, "WATER") == 0) {
        strcpy(full_mol, "WATER");
    }
    else if (strcmp(token_mol, "GLUCOSE") == 0) {
        strcpy(full_mol, "GLUCOSE");
    }
    else if (strcmp(token_mol, "ALCOHOL") == 0) {
        strcpy(full_mol, "ALCOHOL");
    }
    else {
        snprintf(response, resp_size, "ERROR: invalid molecule type\n");
        return;
    }

    // Next token must be a number
    char *token_num = strtok_r(NULL, " \t\r\n", &saveptr);
    if (!token_num) {
        snprintf(response, resp_size, "ERROR: missing number\n");
        return;
    }
    // Ensure no extra tokens
    char *token_extra = strtok_r(NULL, " \t\r\n", &saveptr);
    if (token_extra) {
        snprintf(response, resp_size, "ERROR: too many arguments\n");
        return;
    }

    char *endptr = NULL;
    unsigned long long count = strtoull(token_num, &endptr, 10);
    if (endptr == token_num || *endptr != '\0') {
        snprintf(response, resp_size, "ERROR: invalid number\n");
        return;
    }
    if (count > MAX_ATOMS) {
        snprintf(response, resp_size, "ERROR: number too large\n");
        return;
    }

    // Compute needed atoms for one molecule × count
    uint64_t req_carbon = 0, req_oxygen = 0, req_hydrogen = 0;
    if (strcmp(full_mol, "WATER") == 0) {
        // H2O: needs 2 H + 1 O per molecule
        req_hydrogen = 2ULL * count;
        req_oxygen   = 1ULL * count;
        req_carbon   = 0ULL;
    }
    else if (strcmp(full_mol, "CARBON DIOXIDE") == 0) {
        // CO2: needs 1 C + 2 O per molecule
        req_carbon   = 1ULL * count;
        req_oxygen   = 2ULL * count;
        req_hydrogen = 0ULL;
    }
    else if (strcmp(full_mol, "GLUCOSE") == 0) {
        // C6H12O6: needs 6 C + 12 H + 6 O per molecule
        req_carbon   = 6ULL * count;
        req_hydrogen = 12ULL * count;
        req_oxygen   = 6ULL * count;
    }
    else if (strcmp(full_mol, "ALCOHOL") == 0) {
        // C2H6O: needs 2 C + 6 H + 1 O per molecule
        req_carbon   = 2ULL * count;
        req_hydrogen = 6ULL * count;
        req_oxygen   = 1ULL * count;
    }
    else {
        snprintf(response, resp_size, "ERROR: unknown molecule\n");
        return;
    }

    // Check if enough atoms exist
    if (atom_stock.carbon   < req_carbon) {
        snprintf(response, resp_size, "ERROR: not enough carbon atoms\n");
        return;
    }
    if (atom_stock.oxygen   < req_oxygen) {
        snprintf(response, resp_size, "ERROR: not enough oxygen atoms\n");
        return;
    }
    if (atom_stock.hydrogen < req_hydrogen) {
        snprintf(response, resp_size, "ERROR: not enough hydrogen atoms\n");
        return;
    }

    // Subtract the required atoms
    atom_stock.carbon   -= req_carbon;
    atom_stock.oxygen   -= req_oxygen;
    atom_stock.hydrogen -= req_hydrogen;

    // Print updated inventory 
    print_inventory();

    //if there is a save flag , we will save the atoms to the file.
    if (save_file_path) {
        save_atoms_to_file(save_file_path);
    }

    // Respond with a short “OK: Atoms left – Carbon=.. Oxygen=.. Hydrogen=..\n”
    snprintf(response, resp_size,
             "OK: Atoms left – Carbon=%llu Oxygen=%llu Hydrogen=%llu\n",
             (unsigned long long)atom_stock.carbon,
             (unsigned long long)atom_stock.oxygen,
             (unsigned long long)atom_stock.hydrogen);
}

// ----------------------------------------------------------------------------
// handle_tcp_client():
//   - read exactly one “ADD …” line (via recv), 
//   - call parse_and_update_tcp(…), 
//   - send the response back over that same TCP socket.
// Return false if client closed or a recv‐error occurred.
// ----------------------------------------------------------------------------
bool handle_tcp_client(int client_fd) {
    char buf[MAXBUF];
    ssize_t numbytes = recv(client_fd, buf, sizeof(buf)-1, 0);
    if (numbytes <= 0) {
        // 0 => client closed; <0 => recv error
        return false;
    }
    buf[numbytes] = '\0';

    char response[MAXBUF];
    parse_and_update_tcp(buf, response, sizeof(response));

    if (send(client_fd, response, strlen(response), 0) < 0) {
        perror("send (TCP)");
    }
    return true;
}
// ----------------------------------------------------------------------------
// load_atoms_from_file():
//      if the file exists and big enough , reads sizeof (atomStock) to the global var.
//      else creating a new file , fills it with the values of the atoms and read the full struct to the file.
// ----------------------------------------------------------------------------
static void load_atoms_from_file(const char *path, uint64_t init_c, uint64_t init_o,uint64_t init_h)
{
    struct stat st;
    FILE *fp;

    //if the file exists
    if (stat(path,&st) == 0){
        if(st.st_size >= (off_t)sizeof(atom_stock)){
            //reading all the struct from the file:
            fp = fopen(path,"rb");
            if (!fp){
                perror("fopen (for read)");
                exit(EXIT_FAILURE);
            }
            size_t n = fread(&atom_stock,1,sizeof(AtomStock),fp);
            if (n != sizeof(AtomStock)){
                fprintf(stderr,"Error:could not read Atoms from file \n");
                fclose(fp);
                exit(EXIT_FAILURE);
            }
            fclose(fp);
            return;
        }
        //if file is too small , we will drop to create a new file.
    }

    //if we reach here , file not exists or too small -> creating a new file :
    atom_stock.carbon = init_c;
    atom_stock.oxygen = init_o;
    atom_stock.hydrogen = init_h;

    fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen for write");
        exit(EXIT_FAILURE);
    }
    size_t w = fwrite(&atom_stock,1,sizeof(AtomStock),fp);
    if (w != sizeof(AtomStock)){
        fprintf(stderr,"Error: could not write full Atoms struct to file \n");
        fclose(fp);
        exit(EXIT_FAILURE);
    }
    fclose(fp);
}


// ----------------------------------------------------------------------------
// save_atoms_to_file():
//opens / creates the file in rb or wb. locks the file with flock to prevent parallel changes.
//writes a block of our atoms struct with fwrite.
//releases the lock and closes the files.
// ----------------------------------------------------------------------------
static void save_atoms_to_file(const char *path){
    FILE *fp = fopen(path,"r+b");
    if (!fp) { //if file does not exits we will try to create it
        fp = fopen(path,"w+b");
        if(!fp){
            perror("fopen for save");
            return;
        }
    }

    int fd = fileno(fp); // should understand later
    if (fd < 0) {
        perror("fileno");
        fclose(fp);
        return;
    }

    //exlusive lock:
    if (flock(fd,LOCK_EX) < 0){
        perror("flock LOCK_EX");
        //might be risky but we will keep going :D
    }

    //writing:
    if (fseek(fp,0,SEEK_SET)!= 0)
        perror("fseek");
    size_t w = fwrite(&atom_stock,1,sizeof(AtomStock),fp);
    if (w!=sizeof(AtomStock))
        fprintf(stderr, "Error: could not write full Atom struct\n");
    fflush(fp);

    //releasing the lock:
    if (flock(fd, LOCK_UN) < 0) {
        perror("flock LOCK_UN");
    }
    fclose(fp);

}   






// ----------------------------------------------------------------------------
// main():
//   • parse flags (−c, −o, −h, −t, −T, −U, optionally −s or −d)
//   • set up atom_stock
//   • possibly install SIGALRM
//   • create and bind TCP listen socket on port T
//   • create and bind UDP socket on port U
//   • optionally create & bind UDS‐STREAM if −s was given
//   • optionally create & bind UDS‐DGRAM if −d was given
//   • enter select() loop: monitor 
//       – tcp_listen_fd, 
//       – udp_fd, 
//       – any accepted TCP client fds, 
//       – STDIN_FILENO, 
//       – uds_stream_fd (if set), 
//       – uds_dgram_fd (if set).
//   • on tcp_listen_fd ready: accept new connection, add to client list
//   • on udp_fd ready: recvfrom, parse_and_update_udp, sendto reply
//   • on any TCP client fd ready: call handle_tcp_client()
//   • on STDIN_FILENO ready: handle “GEN …” console commands
//   • on uds_stream_fd ready: accept a UDS‐STREAM connection, call handle_tcp_client() over it, close it
//   • on uds_dgram_fd ready: recvfrom a “DELIVER …” datagram from a UDS client, parse_and_update_udp, sendto reply back to that UDS client
//   • if timeout triggered, break out and clean up
// ----------------------------------------------------------------------------
int main(int argc, char *argv[]) {
    // 1) Parse command‐line options
    uint64_t init_carbon   = 0;
    uint64_t init_oxygen   = 0;
    uint64_t init_hydrogen = 0;
    int timeout_secs       = 0;
    int tcp_port           = -1;
    int udp_port           = -1;
    char *uds_stream_path  = NULL;
    char *uds_dgram_path   = NULL;

    struct option long_opts[] = {
        {"carbon",       required_argument, 0, 'c'},
        {"oxygen",       required_argument, 0, 'o'},
        {"hydrogen",     required_argument, 0, 'h'},
        {"timeout",      required_argument, 0, 't'},
        {"tcp-port",     required_argument, 0, 'T'},
        {"udp-port",     required_argument, 0, 'U'},
        {"stream-path",  required_argument, 0, 's'},
        {"datagram-path",required_argument, 0, 'd'},
        {"save-file",required_argument, 0, 'f'},
        {0,0,0,0}
    };
    const char *short_opts = "c:o:h:t:T:U:s:d:f:";
    int opt;
    while ((opt = getopt_long(argc, argv, short_opts, long_opts, NULL)) != -1) {
        switch (opt) {
            case 'c':
                init_carbon = strtoull(optarg, NULL, 10);
                break;
            case 'o':
                init_oxygen = strtoull(optarg, NULL, 10);
                break;
            case 'h':
                init_hydrogen = strtoull(optarg, NULL, 10);
                break;
            case 't':
                timeout_secs = atoi(optarg);
                break;
            case 'T':
                tcp_port = atoi(optarg);
                break;
            case 'U':
                udp_port = atoi(optarg);
                break;
            case 's':
                uds_stream_path = optarg;
                break;
            case 'd':
                uds_dgram_path = optarg;
                break;
            case 'f':
                save_file_path = optarg;
                break;
            default:
                fprintf(stderr,
                    "Usage: %s -c <carbon> -o <oxygen> -h <hydrogen> "
                    "[-t <timeout>] -T <tcp_port> -U <udp_port> \\\n"
                    "       [-s <uds_stream_path>] [-d <uds_dgram_path>] -f <file path>\n"
                    " -f <file path>",
                    argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Enforce: −c, −o, −h, −T, −U are all mandatory.
    if (tcp_port <= 0 || udp_port <= 0)
    {
        fprintf(stderr,
            "ERROR: you must specify "
            "-T <tcp_port> -U <udp_port>.\n"
            "Usage: %s -c <carbon> -o <oxygen> -h <hydrogen> "
            "[-t <timeout>] -T <tcp_port> -U <udp_port> \\\n"
            "       [-s <uds_stream_path>] [-d <uds_dgram_path>]  -f <file path>\n",
            argv[0]);
        exit(EXIT_FAILURE);
    }

    // if we did use the f flag
    if (save_file_path) {
        load_atoms_from_file(save_file_path, init_carbon, init_oxygen, init_hydrogen);
    }
    else {
        // אם אין -f, מאתחלים inv לערכי ברירת המחדל
        atom_stock.carbon         = init_carbon;
        atom_stock.oxygen         = init_oxygen;
        atom_stock.hydrogen       = init_hydrogen;
    }

    // 3) If timeout_secs > 0, install SIGALRM handler and call alarm(timeout_secs)
    if (timeout_secs > 0) {
        struct sigaction sa_alrm;
        memset(&sa_alrm, 0, sizeof(sa_alrm));
        sa_alrm.sa_handler = alarm_handler;
        sigemptyset(&sa_alrm.sa_mask);
        sa_alrm.sa_flags = 0;  // not using SA_RESTART
        if (sigaction(SIGALRM, &sa_alrm, NULL) == -1) {
            perror("sigaction(SIGALRM)");
            exit(EXIT_FAILURE);
        }
        alarm(timeout_secs);
    }

    // Convert ports to strings for getaddrinfo
    char tcp_port_str[6], udp_port_str[6];
    snprintf(tcp_port_str, sizeof(tcp_port_str), "%d", tcp_port);
    snprintf(udp_port_str, sizeof(udp_port_str), "%d", udp_port);

    // ----------------------------------------------------------------------------
    // 4) Create TCP listening socket on tcp_port
    // ----------------------------------------------------------------------------
    int tcp_listen_fd;
    {
        struct addrinfo hints_tcp;
        struct addrinfo *servinfo_tcp, *p_tcp;
        memset(&hints_tcp, 0, sizeof(hints_tcp));
        hints_tcp.ai_family   = AF_INET;      // IPv4 only (for simplicity)
        hints_tcp.ai_socktype = SOCK_STREAM;  // TCP
        hints_tcp.ai_flags    = AI_PASSIVE;   // use local IP

        int rv;
        if ((rv = getaddrinfo(NULL, tcp_port_str, &hints_tcp, &servinfo_tcp)) != 0) {
            fprintf(stderr, "getaddrinfo (TCP): %s\n", gai_strerror(rv));
            exit(EXIT_FAILURE);
        }

        int yes = 1;
        for (p_tcp = servinfo_tcp; p_tcp != NULL; p_tcp = p_tcp->ai_next) {
            tcp_listen_fd = socket(p_tcp->ai_family, p_tcp->ai_socktype, p_tcp->ai_protocol);
            if (tcp_listen_fd < 0) {
                perror("socket (TCP)");
                continue;
            }
            if (setsockopt(tcp_listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int)) == -1) {
                perror("setsockopt (TCP)");
                close(tcp_listen_fd);
                exit(EXIT_FAILURE);
            }
            if (bind(tcp_listen_fd, p_tcp->ai_addr, p_tcp->ai_addrlen) < 0) {
                perror("bind (TCP)");
                close(tcp_listen_fd);
                continue;
            }
            // Bound successfully
            break;
        }
        if (p_tcp == NULL) {
            fprintf(stderr, "Error: failed to bind TCP on port %s\n", tcp_port_str);
            exit(EXIT_FAILURE);
        }
        freeaddrinfo(servinfo_tcp);

        if (listen(tcp_listen_fd, BACKLOG) < 0) {
            perror("listen (TCP)");
            exit(EXIT_FAILURE);
        }
        printf("server (TCP): listening on port %s...\n", tcp_port_str);

    }

    // ----------------------------------------------------------------------------
    // 5) Create UDP socket on udp_port
    // ----------------------------------------------------------------------------
    int udp_fd;
    {
        struct addrinfo hints_udp;
        struct addrinfo *servinfo_udp, *p_udp;
        memset(&hints_udp, 0, sizeof(hints_udp));
        hints_udp.ai_family   = AF_INET;      // IPv4 only
        hints_udp.ai_socktype = SOCK_DGRAM;   // UDP
        hints_udp.ai_flags    = AI_PASSIVE;   // use local IP

        int rv2;
        if ((rv2 = getaddrinfo(NULL, udp_port_str, &hints_udp, &servinfo_udp)) != 0) {
            fprintf(stderr, "getaddrinfo (UDP): %s\n", gai_strerror(rv2));
            exit(EXIT_FAILURE);
        }
        for (p_udp = servinfo_udp; p_udp != NULL; p_udp = p_udp->ai_next) {
            udp_fd = socket(p_udp->ai_family, p_udp->ai_socktype, p_udp->ai_protocol);
            if (udp_fd < 0) {
                perror("socket (UDP)");
                continue;
            }
            if (bind(udp_fd, p_udp->ai_addr, p_udp->ai_addrlen) < 0) {
                perror("bind (UDP)");
                close(udp_fd);
                continue;
            }
            // Bound successfully
            break;
        }
        if (p_udp == NULL) {
            fprintf(stderr, "Error: failed to bind UDP on port %s\n", udp_port_str);
            exit(EXIT_FAILURE);
        }
        freeaddrinfo(servinfo_udp);

        printf("server (UDP): listening on port %s...\n", udp_port_str);
    }

    // ----------------------------------------------------------------------------
    // 6) create UDS‐STREAM socket "-s"
    // ----------------------------------------------------------------------------
    int uds_stream_fd = -1;
    if (uds_stream_path) {
        // Remove any existing file at that path, to avoid “address already in use”
        unlink(uds_stream_path);

        uds_stream_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (uds_stream_fd < 0) {
            perror("socket (UDS_STREAM)");
            exit(EXIT_FAILURE);
        }
        struct sockaddr_un uds_stream_addr;
        memset(&uds_stream_addr, 0, sizeof(uds_stream_addr));
        uds_stream_addr.sun_family = AF_UNIX;
        strncpy(uds_stream_addr.sun_path, uds_stream_path,
                sizeof(uds_stream_addr.sun_path) - 1);

        if (bind(uds_stream_fd, (struct sockaddr*)&uds_stream_addr,
                 sizeof(uds_stream_addr)) < 0)
        {
            perror("bind (UDS_STREAM)");
            close(uds_stream_fd);
            exit(EXIT_FAILURE);
        }
        if (listen(uds_stream_fd, BACKLOG) < 0) {
            perror("listen (UDS_STREAM)");
            close(uds_stream_fd);
            exit(EXIT_FAILURE);
        }
        printf("server (UDS_STREAM): listening on path %s\n", uds_stream_path);
    }

    // ----------------------------------------------------------------------------
    // 7) create UDS‐DGRAM socket "-d"
    // ----------------------------------------------------------------------------
    int uds_dgram_fd = -1;
    if (uds_dgram_path) {
        unlink(uds_dgram_path);

        uds_dgram_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (uds_dgram_fd < 0) {
            perror("socket (UDS_DGRAM)");
            exit(EXIT_FAILURE);
        }
        struct sockaddr_un uds_dgram_addr;
        memset(&uds_dgram_addr, 0, sizeof(uds_dgram_addr));
        uds_dgram_addr.sun_family = AF_UNIX;
        strncpy(uds_dgram_addr.sun_path, uds_dgram_path,
                sizeof(uds_dgram_addr.sun_path) - 1);

        if (bind(uds_dgram_fd, (struct sockaddr*)&uds_dgram_addr,
                 sizeof(uds_dgram_addr)) < 0)
        {
            perror("bind (UDS_DGRAM)");
            close(uds_dgram_fd);
            exit(EXIT_FAILURE);
        }
        printf("server (UDS_DGRAM): bound on path %s\n", uds_dgram_path);
    }

    // ----------------------------------------------------------------------------
    // 8) Initialize an array of active TCP client fds
    // ----------------------------------------------------------------------------
    int client_fds[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++) {
        client_fds[i] = -1;  // –1 means “empty slot”
    }

    // ----------------------------------------------------------------------------
    // 9) Print the console prompt and initial inventory
    // ----------------------------------------------------------------------------
    printf("\n=== DRINKS_BAR SERVER READY ===\n");
    printf("Valid console commands (type here):\n");
    printf("  GEN SOFT DRINK\n");
    printf("  GEN VODKA\n");
    printf("  GEN CHAMPAGNE\n\n");
    printf("Press Ctrl+C to terminate.\n\n");
    print_inventory();

    // ----------------------------------------------------------------------------
    // 10) Enter the main select() loop
    // ----------------------------------------------------------------------------
    while (1) {
        if (timed_out) {
            // Timeout triggered ⇒ no activity within the last <timeout_secs> seconds
            printf("TIMEOUT: no activity for %d seconds. Shutting down.\n", timeout_secs);
            break;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;

        // a) Always watch tcp_listen_fd
        FD_SET(tcp_listen_fd, &read_fds);
        if (tcp_listen_fd > max_fd) max_fd = tcp_listen_fd;

        // b) Always watch udp_fd
        FD_SET(udp_fd, &read_fds);
        if (udp_fd > max_fd) max_fd = udp_fd;

        // c) Watch all active TCP client fds
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] != -1) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd) {
                    max_fd = client_fds[i];
                }
            }
        }

        // d) Watch keyboard (STDIN_FILENO)
        FD_SET(STDIN_FILENO, &read_fds);
        if (STDIN_FILENO > max_fd) max_fd = STDIN_FILENO;

        // e) If UDS_STREAM was created, watch uds_stream_fd
        if (uds_stream_path) {
            FD_SET(uds_stream_fd, &read_fds);
            if (uds_stream_fd > max_fd) max_fd = uds_stream_fd;
        }

        // f) If UDS_DGRAM was created, watch uds_dgram_fd
        if (uds_dgram_path) {
            FD_SET(uds_dgram_fd, &read_fds);
            if (uds_dgram_fd > max_fd) max_fd = uds_dgram_fd;
        }

        // Wait until at least one descriptor is ready
        int ready = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if (ready < 0) {
            if (errno == EINTR) {
                // Interrupted by a signal (likely SIGALRM). Recompute if timed_out.
                continue;
            }
            perror("select");
            exit(EXIT_FAILURE);
        }

        // -------------------------------------------------------
        // 10.1 New incoming TCP connection?
        // If tcp_listen_fd is ready, accept() it and store in client_fds[].
        // -------------------------------------------------------
        if (FD_ISSET(tcp_listen_fd, &read_fds)) {
            struct sockaddr_storage client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(tcp_listen_fd,
                                (struct sockaddr*)&client_addr,
                                &addr_len);
            if (new_fd < 0) {
                perror("accept (TCP)");
            } else {
                // store new_fd in the first empty slot
                bool added = false;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] == -1) {
                        client_fds[i] = new_fd;
                        added = true;
                        break;
                    }
                }
                if (!added) {
                    // too many clients; drop this connection
                    close(new_fd);
                } else {
                    // print the new client's IPv4 address
                    char ipstr[INET_ADDRSTRLEN];
                    struct sockaddr_in *sa = (struct sockaddr_in *)&client_addr;
                    inet_ntop(AF_INET, &sa->sin_addr, ipstr, sizeof(ipstr));
                    printf("New TCP client from %s\n", ipstr);
                }
            }
            // Reset alarm if using timeout
            if (timeout_secs > 0) {
                alarm(timeout_secs);
                timed_out = 0;
            }
        }

        // -------------------------------------------------------
        // 10.2 Incoming UDP datagram?
        // If udp_fd is ready, recvfrom() it, parse_and_update_udp(), sendto() the reply.
        // -------------------------------------------------------
        if (FD_ISSET(udp_fd, &read_fds)) {
            char buf[MAXBUF];
            struct sockaddr_storage client_addr;
            socklen_t addr_len = sizeof(client_addr);
            ssize_t numbytes = recvfrom(
                udp_fd,
                buf, sizeof(buf)-1,
                0,
                (struct sockaddr*)&client_addr,
                &addr_len
            );
            if (numbytes < 0) {
                perror("recvfrom (UDP)");
            } else {
                buf[numbytes] = '\0';
                char response[MAXBUF];
                parse_and_update_udp(buf, response, sizeof(response));
                // reply to exactly that client address:
                if (sendto(
                        udp_fd,
                        response, strlen(response),
                        0,
                        (struct sockaddr*)&client_addr,
                        addr_len
                    ) < 0)
                {
                    perror("sendto (UDP)");
                }
            }
            if (timeout_secs > 0) {
                alarm(timeout_secs);
                timed_out = 0;
            }
        }

        // -------------------------------------------------------
        // 10.3 Check each active TCP client descriptor: 
        // if ready, call handle_tcp_client(); if it returns false, close & remove.
        // -------------------------------------------------------
        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = client_fds[i];
            if (fd != -1 && FD_ISSET(fd, &read_fds)) {
                if (!handle_tcp_client(fd)) {
                    close(fd);
                    client_fds[i] = -1;
                }
                // Reset alarm if using timeout
                if (timeout_secs > 0) {
                    alarm(timeout_secs);
                    timed_out = 0;
                }
            }
        }

        // -------------------------------------------------------
        // 10.4 Console keyboard input (STDIN_FILENO)?
        // If ready, read one line, interpret “GEN …” commands.
        // -------------------------------------------------------
        if (FD_ISSET(STDIN_FILENO, &read_fds)) {
            if (save_file_path) {
                load_atoms_from_file(save_file_path, 0, 0, 0);
            }
            char linebuf[MAXBUF];
            if (fgets(linebuf, sizeof(linebuf), stdin) != NULL) {
                // strip trailing newline
                size_t L = strlen(linebuf);
                if (L > 0 && linebuf[L-1] == '\n') {
                    linebuf[L-1] = '\0';
                }
                // Expect “GEN <BEVERAGE>”
                char *cmd = strtok(linebuf, " \t");
                if (!cmd || strcmp(cmd, "GEN") != 0) {
                    printf("ERROR: invalid console command\n");
                } else {
                    char *drink = strtok(NULL, " \t");
                    if (!drink) {
                        printf("ERROR: missing drink type after GEN\n");
                    }
                    else if (strcmp(drink, "SOFT") == 0) {
                        char *maybe_drink = strtok(NULL, " \t");
                        if (!maybe_drink || strcmp(maybe_drink, "DRINK") != 0) {
                            printf("ERROR: did you mean 'GEN SOFT DRINK'?\n");
                        } else {
                            // Soft drink requires 6 C, 14 H, 9 O
                            uint64_t c = atom_stock.carbon / 6;
                            uint64_t h = atom_stock.hydrogen / 14;
                            uint64_t o = atom_stock.oxygen / 9;
                            uint64_t can_make = c;
                            if (h < can_make) can_make = h;
                            if (o < can_make) can_make = o;
                            printf("You can make up to %llu SOFT DRINK(s)\n",
                                   (unsigned long long)can_make);
                        }
                    }
                    else if (strcmp(drink, "VODKA") == 0) {
                        // Vodka requires 8 C, 20 H, 8 O
                        uint64_t c = atom_stock.carbon / 8;
                        uint64_t h = atom_stock.hydrogen / 20;
                        uint64_t o = atom_stock.oxygen / 8;
                        uint64_t can_make = c;
                        if (h < can_make) can_make = h;
                        if (o < can_make) can_make = o;
                        printf("You can make up to %llu VODKA(s)\n",
                               (unsigned long long)can_make);
                    }
                    else if (strcmp(drink, "CHAMPAGNE") == 0) {
                        // Champagne requires 3 C, 9 H, 4 O
                        uint64_t c = atom_stock.carbon / 3;
                        uint64_t h = atom_stock.hydrogen / 9;
                        uint64_t o = atom_stock.oxygen / 4;
                        uint64_t can_make = c;
                        if (h < can_make) can_make = h;
                        if (o < can_make) can_make = o;
                        printf("You can make up to %llu CHAMPAGNE(s)\n",
                               (unsigned long long)can_make);
                    }
                    else {
                        printf("ERROR: unknown drink type '%s'\n", drink);
                    }
                }
            } else {
                // EOF (Ctrl+D) or error reading stdin ⇒ exit loop
                printf("Console closed or error – exiting.\n");
                break;
            }
            if (timeout_secs > 0) {
                alarm(timeout_secs);
                timed_out = 0;
            }
        }

        // -------------------------------------------------------
        // 10.5 Accept a new UDS_STREAM connection (if that socket exists)
        // Once accepted, handle exactly one “ADD …” on that connection and close.
        // -------------------------------------------------------
        if (uds_stream_path && uds_stream_fd >= 0 && FD_ISSET(uds_stream_fd, &read_fds)) {
            int new_un_fd = accept(uds_stream_fd, NULL, NULL);
            if (new_un_fd < 0) {
                perror("accept (UDS_STREAM)");
            } else {
                if (save_file_path) {
                    load_atoms_from_file(save_file_path, 0,0,0);
                }
                // Use the same TCP‐handler for “ADD …” lines
                if (!handle_tcp_client(new_un_fd)) {
                    // either closed immediately or error
                }
                close(new_un_fd);
            }
            if (timeout_secs > 0) {
                alarm(timeout_secs);
                timed_out = 0;
            }
        }

        // -------------------------------------------------------
        // 10.6 Receive one UDS_DGRAM datagram “DELIVER …” (if that socket exists)
        // Parse & respond to that client’s address over UDS datagram.
        // -------------------------------------------------------
        if (uds_dgram_path && uds_dgram_fd >= 0 && FD_ISSET(uds_dgram_fd, &read_fds)) {
            char buf[MAXBUF];
            struct sockaddr_un cli_un;
            socklen_t cli_len = sizeof(cli_un);
            ssize_t nbytes = recvfrom(
                uds_dgram_fd,
                buf, sizeof(buf)-1,
                0,
                (struct sockaddr*)&cli_un,
                &cli_len
            );
            if (nbytes < 0) {
                perror("recvfrom (UDS_DGRAM)");
            } else {
                buf[nbytes] = '\0';
                if (save_file_path) {
                    load_atoms_from_file(save_file_path, 0,0,0);
                }
                char response[MAXBUF];
                parse_and_update_udp(buf, response, sizeof(response));
                if (save_file_path) {
                    save_atoms_to_file(save_file_path);
                }
                if (sendto(
                        uds_dgram_fd,
                        response, strlen(response),
                        0,
                        (struct sockaddr*)&cli_un,
                        cli_len
                    ) < 0)
                {
                    perror("sendto (UDS_DGRAM)");
                }
            }
            if (timeout_secs > 0) {
                alarm(timeout_secs);
                timed_out = 0;
            }
        }

    } // end of main select‐loop

    // ----------------------------------------------------------------------------
    // 11) Clean up: close sockets and unlink any UDS files
    // ----------------------------------------------------------------------------
    if (tcp_listen_fd >= 0) close(tcp_listen_fd);
    if (udp_fd >= 0)        close(udp_fd);
    if (uds_stream_fd >= 0) close(uds_stream_fd);
    if (uds_dgram_fd >= 0)  close(uds_dgram_fd);

    if (uds_stream_path)   unlink(uds_stream_path);
    if (uds_dgram_path)    unlink(uds_dgram_path);

    printf("Server exiting cleanly.\n");
    return 0;
}

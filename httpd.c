/* TODO: Description and License */

/* Possible build options: -DDEBUG -DNO_IPV6 */

#define MAX_EVENTS 128 /* Maximum number of events to be returned from
                         a single epoll_wait() call */
/* when you have multiple threads read from the same epoll-fd concurrently. In
 * that case the size of your event array determines how many events get
 * handled by a single thread (i.e. a smaller number might give you greater
 * parallelism).
 *
 * */

#ifndef NO_IPV6 /* TODO: Add IPv6 support */
#   define HAVE_INET6
#endif

#if defined(DEBUG)
 #define DEBUG_PRINT(fmt, args...) fprintf(stderr, "DEBUG: %s:%d:%s(): " fmt, \
             __FILE__, __LINE__, __func__, ##args)
#else
 #define DEBUG_PRINT(fmt, args...) /* Don't do anything in release builds */
#endif


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <getopt.h>
#include <sys/epoll.h>
#include <signal.h>

/* Object representing a connection being processed.
 * Functions: connection_new(), connection_free()
 * */
struct connection {
    int sock;
    enum {
        RECV_REQUEST,
        SEND_HEADER,
        SEND_REPLY,
        DONE
    } state;

    char request;
    size_t request_length;

};

/* --- GLOBALS --- */
static int sockserv;    /* Server socket we accept connections from. */
static int epoll_set;   /* fd of the epoll set used for i/o multiplexing. */
static volatile int running = 1; /* Volatile so there's no problem in signal handler*/
static FILE *logfile = NULL; /* Log to console by default*/;
static size_t nopen_connections = 0;
//static size_T nbytes_served;


/* Config variables. They can be overriden by command line options */
static int port = 8080;                 /* port to listen to */
static const char *bindaddr_str = NULL; /* Bind interface. Default: NULL: 0.0.0.0 */
static int sockserv_backlog = -1; /* somaxconn */

/* --- END GLOBALS --- */

static void err(int errcode, char *errmsg) {
    fprintf(stderr, "Error: %s\n", errmsg);
    exit(errcode);
}

/*
 * Error checked wrapper around malloc()
 * */
static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if(p == NULL)
        err(1, "malloc() failed. Big trouble.");
    return p;
}

static void print_help(char *program_name) {
#define SOME_TABS "\t\t\t\t"
    printf(
            "httpd: TODO: Fill this\n\n"
            "Usage: %s [options] <www_directory>\n\n"
            "Available options:\n"
            "--help, -h" SOME_TABS "Display this help message and exit.\n"
            "--version, -v" SOME_TABS "Print version number and exit. \n"
            "--port, -p <port>" SOME_TABS "Specify listening port.\n" //TODO: COMPLETE ALL THIS
            ,
            program_name
            );
#undef SOME_TABS
}

/* Parse command line options and fill all global config variables.
 * If an optional option is missing, this sets the default 
 * (if needed) */
static void parse_cmdline(int argc, const char **argv) {
    static char *shortopts = "p:a:hv";
    static struct option longopts[] = 
    {
        {"port", required_argument, NULL, 'p'},
        {"bind-address", required_argument, NULL, 'a'},
        {"help", no_argument, NULL, 'h'}, // info: optional_argument
        {"version", no_argument, NULL, 'v'}
    };
    
    opterr = 1;
    int index; char c;
    while((c = getopt_long(argc, argv, shortopts, 
                    longopts, &index)) != -1) {
        //printf("-> c: %c index: %d, optarg: %s\n", c, index, optarg);
        switch(c) {
            case 'p': /* --port, -p */
                port = atoi(optarg); // global
                printf("%d",port);
                if(port == 0) err(1,"Invalid port.");
                
                break;
            case 'a': /* --bind-address, -a */
                bindaddr_str = optarg;

                break;
            case 'h': /* --help, -h */
                print_help(argv[0]);
                exit(0);

                break;
            case 'v': /* --version, -v*/
                  printf("httpd version 0.0.0. See %s --help for usage and more information.\n",
                          argv[0]);
                  exit(0);
            case 0:
            case '?':
                /* Invalid option or missing required argument. 
                 * if opterr is set, glibc will print a message. */
                fprintf(stderr, "See %s --help\n", argv[0]);
                exit(1);
                break;
            default:
                err(1, "Error parsing command line options.");
        }
    }

}

/* <---------------------------------------------> */

/*
 * Allocate a new connection object and initialize it
 * to default values.
 * */
struct connection* connection_new() {
    struct connection *conn = (struct connection *) xmalloc(
            sizeof(struct connection));
    

}

/* Sets the O_NONBLOCK flag on a file descriptor. 
 * Returns -1 on error, 0 on success */
static int set_nonblocking(const int fd) {
    int flags = fcntl(fd, F_GETFL);
    if(flags == -1)
        return -1;
    flags |= O_NONBLOCK;
    if(fcntl(fd, F_SETFL, flags) == -1)
        return -1;

    return 0;
}

/* Initialize the sockserv global. With this socket we 
 * bind to a port and accept connection */
static void init_sockserv(void) {
    struct sockaddr_in serv_addr;
    int optval;

    sockserv = socket(AF_INET, SOCK_STREAM, 0);
    if(sockserv == -1)
        err(1, "socket()");

    /* reuse address */
    optval = 1;
    if(setsockopt(sockserv, SOL_SOCKET, SO_REUSEADDR,
                &optval, sizeof(optval)) == -1)
        err(1, "setsockopt()");

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = bindaddr_str? inet_addr(bindaddr_str) : INADDR_ANY;
    if(serv_addr.sin_addr.s_addr == (in_addr_t)INADDR_NONE)
        err(1, "invalid --bindaddr");
    serv_addr.sin_port = htons(port);
    
    if(bind(sockserv, (struct sockaddr*)&serv_addr, sizeof(serv_addr) ) == -1)
        err(1, "bind()");

    if(listen(sockserv, sockserv_backlog) == -1)
        err(1, "listen()");
    
    if(set_nonblocking(sockserv) == -1)
        err(1, "set_nonblocking()");
}

static void epoll_init(void) {
    struct epoll_event *event = 
        (struct epoll_event *) xmalloc(sizeof(struct epoll_event));

    epoll_set = epoll_create(1024);
    if(epoll_set == -1)
        err(1, "epoll_create()");

    /* We'll receive an epoll event each time a new client conencts */    
    event->data.ptr = NULL; /* NULL in data means this event is from the server socket. */
    event->events = EPOLLIN;
    if(epoll_ctl(epoll_set, EPOLL_CTL_ADD, 
                sockserv, event) == -1)
        err(1, "epoll() EPOLL_CTL_ADD server socket.");
}

/* 
 * Event loop
 * */
static void httpd_epoll(void){
    int nfds_ready;
    struct epoll_event evlist[MAX_EVENTS];

    nfds_ready = epoll_wait(epoll_set, evlist, MAX_EVENTS, -1);

    if(nfds_ready == -1) {
        if(errno == EINTR){ return; }
        else err(1, "epoll_wait()");
    }

    for(size_t i=0; i<nfds_ready; i++) {
        DEBUG_PRINT("Got a ready fds.");      
    }
    
}
/* --- SIGNAL HANDLING --- */
static void handle_signals(int sig) {
    fprintf(stderr, "Signal received. QUITING...\n");
    running = 0;
}

static void handle_sigpipe(int sig) {
    // TODO: If multithreaded, use a lock here! or deprecate this
    // and run a infinite loop.
    fprintf(stderr, "[DEBUG] Received SIGPIPE\n");
}
/* --- END SIGNAL HANDLING --- */

int main(int argc, char **argv) {
    parse_cmdline(argc, argv); /* This will fill all global config vars */    
    
    // TODO: Log file
    logfile = stdout;

    /* Signal handling */
    if(signal(SIGPIPE, handle_sigpipe) == SIG_ERR)
        err(1,"signal(SIGPIPE)");
    if(signal(SIGINT, handle_signals) == SIG_ERR)
        err(1, "signal(SIGINT)");
    if(signal(SIGTERM, handle_signals) == SIG_ERR)
        err(1,"signal(SIGTERM)");

    init_sockserv(); /* Create and bind server socket */
    printf(logfile, "Listening on %s:%d", bindaddr_str ? "0.0.0.0" : bindaddr_str, port);
  
    epoll_init();   /* initialize epoll set */

    while(running) httpd_epoll(); /* Event loop */
    
    /* Cleanup */
    // TODO


    return 0;
}

/* vim:set tabstop=4 shiftwidth=4 expandtab tw=78: */

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

#define RECV_BUFSIZE 1024 /* how much data to read() from socket at once */
#define HTTP_MAXREQUESTSIZE 8*1024 /* 8K just like Apache. */


#ifndef NO_IPV6 /* TODO: Add IPv6 support */
#   define HAVE_INET6
#endif

#if defined(DEBUG)
 #define DEBUG_PRINT(fmt, args...) fprintf(stderr, "DEBUG: %s:%d:%s(): " fmt, \
             __FILE__, __LINE__, __func__, ##args)
 #define DEBUG_PRINT_RAW(fmt, args...) fprintf(stderr, fmt, ##args)
#else
 #define DEBUG_PRINT(fmt, args...) /* Don't do anything in release builds */
 #define DEBUG_PRINT_RAW(fmt, args...)
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
    struct epol_event *ev;
    int client_port;
    char *client_ip;

    enum {
        STATE_INVALID,
        STATE_RECV,
        STATE_SEND_HEADER,
        STATE_SEND_REPLY,
        STATE_DONE
    } state;

    char *input_buffer; /* Buffer where we store input data before 
                           processing headers. Grows dynamically. */
    size_t input_len;    /* bytes read into request buffer */
    size_t input_bufsize; /* actual size of buffer */


};

/* --- GLOBALS --- */
static int sockserv;    /* Server socket we accept connections from. */
static int epoll_set;   /* fd of the epoll set used for i/o multiplexing. */
static volatile int running = 1; /* Volatile so there's no problem in signal handler*/
static char *logfile_str = NULL; /* default NULL: stdout*/
static FILE *logfile = NULL; /* Log to console by default*/;
static size_t open_connections = 0;
//static size_T nbytes_served;


/* Config variables. They can be overriden by command line options */
static int port = 8080;                 /* port to listen to */
static const char *bindaddr_str = NULL; /* Bind interface. Default: NULL: 0.0.0.0 */
static int sockserv_backlog = -1; /* somaxconn TODO:*/

/* --- END GLOBALS --- */

static void err(int errcode, char *errmsg) {
    fprintf(stderr, "Error: %s\n", errmsg);
    exit(errcode);
}

static void xerr(int errcode, char *errmsg, int errno) {
    // TODO
}

/*
 * Error checked wrappers around malloc() and realloc()
 * */
static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if(p == NULL)
        err(1, "malloc() failed. Big trouble.");
    return p;
}
static void *xrealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if(p == NULL)
        err(1, "realloc() failed. Big trouble.");
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
    static char *shortopts = "p:a:l:hv";
    static struct option longopts[] = 
    {
        {"port", required_argument, NULL, 'p'},
        {"bind-address", required_argument, NULL, 'a'},
        {"help", no_argument, NULL, 'h'}, // info: optional_argument
        {"version", no_argument, NULL, 'v'},
        {"logfile", required_argument, NULL, 'l'}
    };
    
    opterr = 0;
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
            case 'l': /* --logfile, -l */
                logfile_str = optarg;
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
static struct connection* connection_new() {
    struct connection *conn = (struct connection *) xmalloc(
            sizeof(struct connection));
    conn->sock = -1;
    conn->ev = NULL;
    conn->state = STATE_RECV;
    conn->input_buffer = NULL;
    conn->input_len = 0; 
    conn->input_bufsize = 0;
    conn->client_ip = NULL;
    conn->client_port = 0;
    return conn;

}

static void connection_free(struct connection *conn) {
    free(conn->input_buffer);
    free(conn->ev);
    free(conn->client_ip);
    free(conn);   
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
    
    if(bind(sockserv, (struct sockaddr*)&serv_addr, 
                sizeof(serv_addr) ) == -1)
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

static struct connection *accept_connection() {
    int socket; /* client socket */
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    struct connection *conn;
    struct epoll_event *ev;

    socket = accept(sockserv, (struct sockaddr *) &client_addr, &client_addr_len);
    if(socket == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { 
            /* too many concurrent connections */
            return NULL;
        } else {
            perror("accept()");
            err(1, "accept()");
        }
    }
    set_nonblocking(socket);
    conn = connection_new();
    conn->sock = socket;
    
    /* add the newly accepted connection to the epoll interest set */
    ev = (struct epoll_event*)xmalloc(sizeof(struct epoll_event));
    ev->events = EPOLLIN; /* Since conn->state is RECV_REPLY we are only interested in input events by now. */
    ev->data.ptr = (void*)conn; /* so that we can identify the connection when we receive an event */
    conn->ev = ev; /* we store it here so we can free() it when the connection is destroyed */

    if(epoll_ctl(epoll_set, EPOLL_CTL_ADD, socket, ev) == -1)
        err(1,"epoll_ctl(client socket)");
    
    conn->client_ip = (char *) xmalloc(INET_ADDRSTRLEN);
    conn->client_ip = inet_ntop(AF_INET, (void*)&(client_addr.sin_addr),
            conn->client_ip, INET_ADDRSTRLEN);
    conn->client_port = client_addr.sin_port;

    open_connections++;
    DEBUG_PRINT("Accepted connection from %s:%d\n", conn->client_ip,
            conn->client_port);
    return conn;
}

static void destroy_connection(struct connection *conn) {
    if(epoll_ctl(epoll_set, EPOLL_CTL_DEL, conn->sock, NULL) == -1)
        err(1, "epoll_ctl(EPOLL_CTL_DEL) in destroy_connection()");
    
    if(close(conn->sock) == -1) {
        DEBUG_PRINT("errno after close: %d\t", errno);
        perror("close()"); 
    }
    open_connections--;
    DEBUG_PRINT("Connection closed %s:%d\n", conn->client_ip, 
            conn->client_port);
    connection_free(conn);
}

/* All data has been read, now it's time to process it 
 * and construct the response, then go to STATE_SEND_HEADER state
 * to inform the event to to start sending data.
 * */
static void process_request(struct connection *conn) {
    // for now, destroy connection.
    conn->state = STATE_DONE;
}

/*
 * epoll() told us there is input available  
 * When we get EOF (connection closed by peer or error )
 * or MAX_HTTP_REQUEST_LEN is exceeded there is no more to read.
 * We can then process the request.
 * */
static void epoll_read(struct connection *conn) {
    
    ssize_t nread;
    size_t available;

    /* Make sure there are RECV_BUFSIZE available space 
     * in conn->input_buffer before a read;
     * */
    available = conn->input_bufsize - conn->input_len;
    if(available < RECV_BUFSIZE) {
        conn->input_buffer = xrealloc(conn->input_buffer, 
                conn->input_bufsize + RECV_BUFSIZE);
        conn->input_bufsize += RECV_BUFSIZE;
    }
    /* END DYNAMICALLY GROWING THE BUFFER */

    /* Read data*/
    nread = read(conn->sock, conn->input_buffer + conn->input_len
            , RECV_BUFSIZE);
    if(nread < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            // TODO: Research erros on tlpi book.
            return; // no problem, we'll come back for next event.
        } else if(errno == ECONNRESET) {
            goto all_request_in_buffer;
        } 
        else {
            perror("read()");
            err(1, "read() in epoll_read()");
        }
    } else if(nread > 0) {
        conn->input_len += nread;
        if(conn->input_len > HTTP_MAXREQUESTSIZE) {
            DEBUG_PRINT("Maximum HTTP request size exceeded.\n");
            //TODO: answer with an error code. refactor this
            //so as not to mix http with low leve i/o
        }
        
        size_t l = conn->input_len;
        if(conn->input_len >= 4 && 
                (       conn->input_buffer[l-4] == '\r'
                    &&  conn->input_buffer[l-3] == '\n'
                    &&  conn->input_buffer[l-2] == '\r'
                    &&  conn->input_buffer[l-1] == '\n'
                ) || 
                
                (
                        conn->input_buffer[l-2] == '\n'
                    &&  conn->input_buffer[l-1] == '\n'
                )
                ) {
                                
                    /* End HEADERS */
                    goto all_request_in_buffer;
                }

        return;
    } else { /* read EOF */ goto all_request_in_buffer; }

all_request_in_buffer:
    DEBUG_PRINT_RAW("Request from %s:%d:\n", conn->client_ip, 
            conn->client_port);
    char c = conn->input_buffer[conn->input_len-1];
    conn->input_buffer[conn->input_len-1] = '\0';
    DEBUG_PRINT_RAW("[%s", conn->input_buffer);
    DEBUG_PRINT_RAW("%c]\n", c);
    conn->input_buffer[conn->input_len-1] = c;
    conn->state = STATE_DONE;

    destroy_connection(conn); 
    /* small optimization: we don't have to go through
    anothe iteration of the epoll loop */
}

static void handle_socket_io_event(struct connection *conn) {
   switch(conn->state) {
        case STATE_RECV:
            epoll_read(conn);
            break;
        case STATE_SEND_HEADER:
           
           break;
        case STATE_SEND_REPLY:
           
           break;
        case STATE_DONE:
        case STATE_INVALID:
            destroy_connection(conn);
           break;

   }
}

/* 
 * Event loop
 * */
static void httpd_epoll(void){
    int nfds_ready;
    struct epoll_event evlist[MAX_EVENTS];
    struct connection *conn;

    nfds_ready = epoll_wait(epoll_set, evlist, MAX_EVENTS, -1);

    if(nfds_ready == -1) {
        if(errno == EINTR){ return; }
        else err(1, "epoll_wait()");
    }

    for(size_t i=0; i<nfds_ready; i++) {
        if(evlist[i].data.ptr == NULL) { /* New connection to accept */
            conn = accept_connection();
            if(conn == NULL) {
                /* Too many concurrent connections */
                return;
            }    
            
        } else {
            /* read or write available on a connection. 
             * The connection is in the data.ptr of the event.
             * */
            conn = (struct connection *) evlist[i].data.ptr;
            handle_socket_io_event(conn);

        }
    }
    
}

/* --- SIGNAL HANDLING --- */
static void handle_signals(int sig) {
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
    if(logfile == NULL) logfile = stdout;
    else {
        logfile = fopen(logfile_str, "a");
        if(logfile == NULL) {
            perror("Cannot open log file\n");
            fprintf(stderr, "Falling back to stdout for logs.\n");
            logfile = stdout;
        }
    }

    /* Signal handling */
    if(signal(SIGPIPE, handle_sigpipe) == SIG_ERR)
        err(1,"signal(SIGPIPE)");
    if(signal(SIGINT, handle_signals) == SIG_ERR)
        err(1, "signal(SIGINT)");
    if(signal(SIGTERM, handle_signals) == SIG_ERR)
        err(1,"signal(SIGTERM)");

    init_sockserv(); /* Create and bind server socket */
    epoll_init();   /* initialize epoll set */
    
    fprintf(logfile, "Listening on %s:%d\n", 
            bindaddr_str ? bindaddr_str : "0.0.0.0",
            port );
    
    while(running) httpd_epoll(); /* Event loop */
    
    /* Cleanup */
    // TODO
    printf("\nQuitting...\n");
    close(sockserv);
    fclose(logfile);

    return 0;
}

/* vim:set tabstop=4 shiftwidth=4 expandtab tw=78: */

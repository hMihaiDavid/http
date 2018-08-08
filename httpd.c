/* TODO: Description and License */

/* Possible build options: -DDEBUG -DNO_IPV6 */

#define MAX_EVENTS 128 /* Maximum number of events to be returned from
                         a single epoll_wait() call */
/* when you have multiple threads read from the same epoll-fd concurrently the size of 
 * your event array determines how many events get handled by a single thread 
 * (i.e. a smaller number might give you greater parallelism).
 * */

#define RECV_BUFSIZE 512 /* how much data to read() from socket at once */
#define SEND_BUFSIZE 1024
#define SENDFILE_COUNT 2048
#define HTTP_MAXREQUESTSIZE 8*1024 /* 8K just like Apache. */

/* DEFAULT REPLIES TO ERROR CODES */
#define REPLY_400 "<!DOCTYPE html><html><head><title>400 Bad Request</title></head><body><h1>400 Bad Request</h1><p>Malformed request detected.</p></body></html>"
#define HEADER_400 "HTTP/1.1 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"

#define REPLY_403 "<!DOCTYPE html><html><head><title>403 Forbidden</title></head><body><h1>403 Forbidden</h1><p>You are forbidden to access the requested resource on this server.</p></body></html>"
#define HEADER_403 "HTTP/1.1 403 Forbidden\r\nContent-Type: text/html\r\n\r\n"

#define REPLY_404 "<!DOCTYPE html><html><head><title>404 Not Found</title></head><body><h1>404 Not Found</h1><p>Sorry. The requested resource was not found on this server.</p></body></html>"
#define HEADER_404 "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n\r\n"

#define REPLY_500 "<!DOCTYPE html><html><head><title>500 Internal Server Error</title></head><body><h1>500 Internal Server Error</h1><p>An error has occurred while processing your request.</p></body></html>"
#define HEADER_500 "HTTP/1.1 500 Internal Server Error\r\nContent-Type: text/html\r\n\r\n"


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
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/sysinfo.h> 

/* Object representing a connection being processed.
 * Functions: connection_new(), connection_free()
 * */
struct connection {
    int sock;
    struct epoll_event *ev;
    int client_port;
    char *client_ip;

    enum {
        STATE_INVALID,
        STATE_RECV,
        STATE_SEND_HEADER,
        STATE_SEND_REPLY,
        STATE_DONE
    } state;
    
    enum {
        REPLY_INVALID,
        REPLY_FROMMEM,
        REPLY_FROMFILE
    } reply_type;

    enum {
        METHOD_INVALID,
        METHOD_UNSUPPORTED,
        METHOD_GET,
        METHOD_HEAD,
    
    } method;

    char *url;
    char *host;
    char *user_agent;
    char *referer;

    char *input_buffer; /* Buffer where we store input data before 
                           processing headers. Grows dynamically in multiples of RECV_BUFSIZE */
    size_t input_len;    /* bytes read into request buffer */
    size_t input_bufsize; /* actual size of buffer. */

	/* Buffer to store the HTTP response header. */
    char *outheader;
    size_t outheader_len;
    size_t outheader_sent;

	/* Stores the reply if reply_type is REPLY_FROMMEM, otherwise unused */
    char *reply;
    size_t reply_len;
    size_t reply_sent;
    
    /* Boolean. If set, reply and outheader will not be free()'ed because they are not
     * on the heap, they are default responses hardcoded in the data segment.  */
    int default_reply;
    
    /* If reply_type is REPLY_FROMFILE, this is the filedesc from which to serve the request. */
    int reply_fd;
};

/* --- GLOBALS --- */
const char *www_path = NULL; /* path to the www directory to serve. */
static int sockserv;    /* Server socket we accept connections from. */
static int epoll_set;   /* fd of the epoll set used for i/o multiplexing. */
static volatile int running = 1; /* Volatile so there's no problem in signal handler*/
static char *logfile_str = NULL; /* default NULL: stdout*/
static FILE *logfile = NULL; /* Log to console by default*/;
static size_t open_connections = 0;
int is_parent = 1; /* boolean */
//static size_T nbytes_served;


/* Config variables. They can be overriden by command line options */
static int port = 8080;                 /* port to listen to */
static const char *bindaddr_str = NULL; /* Bind interface. Default: NULL: 0.0.0.0 */
static int sockserv_backlog = -1; /* somaxconn TODO:*/
static int workers = 0;

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

static ssize_t min(ssize_t a, ssize_t b) {
    return a < b ? a : b;
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
    /* The remaining argument is the path to the www directory */
    if(optind >= argc) {
		/* missing www directory */
		fprintf(stderr, "Missing path to www directory. See %s --help\n", argv[0]);
		exit(1);
	}
	
	www_path = argv[optind];

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
    conn->reply_type = REPLY_FROMMEM;
    conn->default_reply = 0;

    conn->url = NULL;
    conn->host = NULL;
    conn->user_agent = NULL;
    conn->referer = NULL;
    
    conn->input_buffer = NULL;
    conn->input_len = 0; 
    conn->input_bufsize = 0;
    
    conn->outheader = NULL;
    conn->outheader_len = 0;
    conn->outheader_sent = 0;
    
    conn->reply = NULL;
    conn->reply_len = 0;
    conn->reply_sent = 0;
    
    conn->reply_fd = -1;

    conn->client_ip = NULL;
    conn->client_port = 0;
    return conn;

}

static void connection_free(struct connection *conn) {
    free(conn->input_buffer);
    if(!conn->default_reply) {
        free(conn->outheader);
        free(conn->reply);
    }
    free(conn->ev);
    free(conn->client_ip);
    /* // No need to free these ones cause they point inside input_buffer 
     * // for efficiency.
     
    free(conn->url);
    free(conn->host);
    free(conn->user_agent);
    free(conn->referer);
    
    */
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
    if(set_nonblocking(sockserv) == -1)
        err(1, "set_nonblocking()");

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

    if(listen(sockserv, sockserv_backlog) == -1) {
		perror("listen:");
		err(1, "listen()");
	}
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

    socket = accept(sockserv, NULL, NULL);
    if(socket == -1) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { 
            /* probably too many concurrent connections */
            return NULL;
        } else {
            perror("accept():");
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
    
    /* 
    conn->client_ip = (char *) xmalloc(INET_ADDRSTRLEN);
    conn->client_ip = inet_ntop(AF_INET, (void*)&(client_addr.sin_addr),
            conn->client_ip, INET_ADDRSTRLEN);
    conn->client_port = client_addr.sin_port;
	*/
    open_connections++;
    DEBUG_PRINT("Accepted connection from %s:%d\n", conn->client_ip,
            conn->client_port);
    return conn;
}

static void destroy_connection(struct connection *conn) {
    if(epoll_ctl(epoll_set, EPOLL_CTL_DEL, conn->sock, NULL) == -1)
        err(1, "epoll_ctl(EPOLL_CTL_DEL) in destroy_connection()");
    
    if(conn->sock > -1) close(conn->sock);
    if(conn->reply_fd > -1) close(conn->reply_fd);
    open_connections--;
    DEBUG_PRINT("Connection closed %s:%d\n", conn->client_ip, 
            conn->client_port);
    connection_free(conn);
}

static void default_reply(struct connection *conn, int code) {
    conn->default_reply = 1;
    conn->reply_type = REPLY_FROMMEM;
    conn->state = STATE_SEND_HEADER;
    
    switch(code){
        case 400:
            conn->outheader = HEADER_400;
            conn->outheader_len = sizeof(HEADER_400)-1;
            conn->reply = REPLY_400;
            conn->reply_len = sizeof(REPLY_400)-1;
            break;
        case 403:
            conn->outheader = HEADER_403;
            conn->outheader_len = sizeof(HEADER_403)-1;
            conn->reply = REPLY_403;
            conn->reply_len = sizeof(REPLY_403)-1;
            break;
        case 404:
            conn->outheader = HEADER_404;
            conn->outheader_len = sizeof(HEADER_404)-1;
            conn->reply = REPLY_404;
            conn->reply_len = sizeof(REPLY_404)-1;
            break;
        case 500:
        default:
			conn->outheader = HEADER_500;
            conn->outheader_len = sizeof(HEADER_500)-1;
            conn->reply = REPLY_500;
            conn->reply_len = sizeof(REPLY_500)-1;
    }
}

static int sanity_check_url(struct connection *conn) {
	char *url = conn->url;
	for(int i=0; url[i] != '\0' && url[i+1] != '\0'; i++) {
		if(url[i] == '.' && url[i+1] == '.') return 0;
	}
	return 1;
}

static void parse_http_method(struct connection *conn, char *str) {
    if(strcmp("GET", str) == 0) {
        conn->method = METHOD_GET; return;
    } else if(strcmp("HEAD", str) == 0) {
        conn->method = METHOD_HEAD; return;
    } else {
        conn->method = METHOD_INVALID; return;
    }
}

/* TODO: MAKE SURE IT'S MEMORY SAFE */
static void parse_input(struct connection *conn) {
    char *p, c;

    /* parse method */
    p = conn->input_buffer;
    while(*p != '\0' && !isblank(*p)) p++;
    c = *p;
    *p = '\0'; /* terminate http method string */
    parse_http_method(conn, conn->input_buffer);
    if(conn->method == METHOD_INVALID || c == '\0') goto bad_request;
    p++;

    /* parse url */
    while(*p != '\0' && (isblank(*p) || *p == '/')) p++;
    conn->url = p;
    while(*p != '\0' && !isblank(*p)) p++;
    if(*p == '\0') goto bad_request;
    *p = '\0'; /* terminate url */
    p++;
    if(!sanity_check_url(conn)) goto bad_request;
        
    return;
    // TODO: parse header lines (Host, Referer, User-Agent, ...)
    
bad_request:
	default_reply(conn, 400);
	return;
}

static void process_request_get(struct connection *conn) {
	struct stat statbuf;
	char *content_type;
		
	if(stat(conn->url, &statbuf) == -1) {
		if(errno == EACCES) { default_reply(conn, 403); return; }
		else if(errno == ENOENT) { default_reply(conn, 404); return; }
		else { default_reply(conn, 500); return; }
	}
	
	/* Only accept regular files. TODO: Support also directory listings. */
	if(!S_ISREG(statbuf.st_mode)) { default_reply(conn, 404); return; }
	
	// TODO: SET Content-Type correctly ex. Content-Type: text/html; charset=ISO-8859-1 TODO: CHUNKED ENCODING???
	content_type = "text/html";
	
	/* Setting the response header */
	conn->outheader_len = asprintf(&conn->outheader,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %lld\r\n"
		"\r\n"
		,
		content_type, (long long)statbuf.st_size);
	if(conn->outheader_len == -1) { default_reply(conn, 500); return; }
	
	if(conn->method == METHOD_HEAD) return; /* Nothing more to do */
	
	/* serve the file */
	conn->reply_type = REPLY_FROMFILE;
	conn->reply_len = (size_t) statbuf.st_size;
	conn->reply_fd = open(conn->url, O_RDONLY);
	if(conn->reply_fd == -1) {
		if(errno == EACCES) { default_reply(conn, 403); return; }
		else if(errno == ENOENT) { default_reply(conn, 404); return; }
		else { default_reply(conn, 500); return; }
	}	
}

/* All data has been read, now it's time to process it 
 * and construct the response, then go to STATE_SEND_HEADER state
 * to inform the event to to start sending data.
 * */
static void process_request(struct connection *conn) {
    parse_input(conn);
    if(conn->default_reply) return;
    
    switch(conn->method) {
		case METHOD_GET:
			process_request_get(conn);
			break;
		case METHOD_HEAD:
			process_request_get(conn);
			break;
		case METHOD_INVALID:
		case METHOD_UNSUPPORTED:
		default:
			default_reply(conn, 400);
			return;
	}
}

static void epoll_write_reply_fromem(struct connection *conn) {
    ssize_t nsent;
    
    nsent = write(conn->sock, conn->reply + conn->reply_sent, 
            //min(SEND_BUFSIZE, 
            conn->reply_len - conn->reply_sent);//)
	
	if(nsent < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { return; }
        else if(errno == EPIPE || errno == ECONNRESET) goto terminate_connection;
        else  {perror("sendfile()");err(1, "sendfile()");}
    }

    conn->reply_sent += nsent;
    if(conn->reply_sent == conn->reply_len) goto terminate_connection;
    
    return;
    
terminate_connection:
	conn->state = STATE_DONE;
    destroy_connection(conn);
}

static void epoll_write_reply_fromfile(struct connection *conn) {
    ssize_t nsent;
    nsent = sendfile(conn->sock, conn->reply_fd, NULL, 
            min(SENDFILE_COUNT, conn->reply_len - conn->reply_sent));
    if(nsent < 0) {
        if(errno == EAGAIN || errno == EINTR) { DEBUG_PRINT("======> %llu\n", (unsigned long long)nsent); return; }
        else if(errno == EPIPE || errno == ECONNRESET) goto terminate_connection;
        else  {perror("sendfile()");err(1, "sendfile()");}
    }

    conn->reply_sent += nsent;
    if(conn->reply_sent == conn->reply_len) goto terminate_connection;
    
    return;
    
terminate_connection:
	conn->state = STATE_DONE;
    destroy_connection(conn);
}

static void epoll_write_header(struct connection *conn) {
    ssize_t nsent;
    //fprintf(stderr, ".");
    nsent = write(conn->sock, conn->outheader + conn->outheader_sent, 
            conn->outheader_len - conn->outheader_sent);
    if(nsent < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return;
        else if(errno == ECONNRESET || errno == EPIPE)
			goto terminate_connection;
        else err(1, "write() epoll_write_header");
    } 
    
    conn->outheader_sent += nsent;
    if(conn->outheader_sent == conn->outheader_len) {
        if(conn->method == METHOD_HEAD)
			goto terminate_connection;
		else
			conn->state = STATE_SEND_REPLY;
    }
    
    return;
    
terminate_connection:
	conn->state = STATE_DONE;
	destroy_connection(conn);
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
        if(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
			return;
        else if(errno == ECONNRESET)
            goto terminate_connection; 
        else err(1, "read() in epoll_read()");
    } else if(nread > 0) {
        conn->input_len += nread;
        if(conn->input_len > HTTP_MAXREQUESTSIZE) {
            DEBUG_PRINT("Maximum HTTP request size exceeded.\n");
			goto terminate_connection;
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

    } else { /* read EOF */ goto terminate_connection; }
	
	return;

all_request_in_buffer:
    process_request(conn);
    
    /* From now on we are only interested in write events from this connection */ 
    struct epoll_event *ev = conn->ev;
    ev->events = EPOLLOUT; 
    if(epoll_ctl(epoll_set, EPOLL_CTL_MOD,conn->sock, conn->ev) == -1)
        err(1, "epoll_ctl(EPOLL_CTL_MOD) set_epoll_event_to_write");
    
    conn->state = STATE_SEND_HEADER;
    return;
    
terminate_connection:
	conn->state = STATE_DONE;
	destroy_connection(conn);
}

static void handle_socket_io_event(struct connection *conn) {
   switch(conn->state) {
        case STATE_RECV:
            epoll_read(conn);
            break;
        case STATE_SEND_HEADER:
            epoll_write_header(conn);
           break;
        case STATE_SEND_REPLY:
           
           if(conn->reply_type == REPLY_FROMFILE)
               epoll_write_reply_fromfile(conn);
           else if(conn->reply_type == REPLY_FROMMEM)
               epoll_write_reply_fromem(conn);

           break;
        case STATE_DONE:
        case STATE_INVALID:
			DEBUG_PRINT("%s\n", conn->state == STATE_DONE ? "DONE" : "INVALID");
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
            if(conn == NULL) continue;
            
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

#ifdef DEBUG
static void handle_sigpipe(int sig) {
    fprintf(stderr, "[DEBUG] Received SIGPIPE\n");
}
#endif
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
#ifdef DEBUG
    if(signal(SIGPIPE, handle_sigpipe) == SIG_ERR)
#else
	if(signal(SIGPIPE, SIG_IGN) == SIG_ERR)
#endif
        err(1,"signal(SIGPIPE)");
    
    if(signal(SIGINT, handle_signals) == SIG_ERR)
        err(1, "signal(SIGINT)");
    if(signal(SIGTERM, handle_signals) == SIG_ERR)
        err(1,"signal(SIGTERM)");
        
    /* change current directory to www directory */
    if(chdir(www_path) == -1)
		err(1, "Cannot open www directory.");
	fprintf(stderr, "Serving directory: %s\n", www_path);

    init_sockserv(); /* Create and bind server socket */
    
    
    fprintf(logfile, "Listening on %s:%d\n", 
            bindaddr_str ? bindaddr_str : "0.0.0.0",
            port );
            
    if(workers <= 0) workers = get_nprocs();
    if(workers > 1) {
		for(int i=0; i < workers-1; i++) { /* the parent is a worker too. */
			pid_t pid;
			pid = fork();
			if(pid == -1) err(1, "fork()");
			else if(pid == 0) {
				is_parent = 0;
				/* If parent terminates, all children get SIGTERM */
				if(prctl(PR_SET_PDEATHSIG, SIGTERM))
					err(1, "prctl(): One less worker process!");
				break;
			}
		}
	}
	
    epoll_init();   /* initialize epoll set and add sockserv to it.*/
    
    while(running) httpd_epoll(); /* Event loop */
    
    /* Cleanup */
    // TODO
    if(is_parent ) printf("\nQuitting...\n");
    close(sockserv);
    fclose(logfile);

    return 0;
}

/* vim:set tabstop=4 shiftwidth=4 expandtab tw=78: */

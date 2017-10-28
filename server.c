#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>

#define BACKLOG 10

#define EXIT_USAGE_ERROR 1
#define EXIT_GETADDRINFO_ERROR 2
#define EXIT_BIND_FAILURE 3
#define EXIT_LISTEN_FAILURE 4

#define MAX_LINE 64

#define DEFAULT_DICTIONARY "words"
#define DEFAULT_PORT "12345"
#define TOK_DELIM "\n"
#define WORKER_COUNT 3

typedef struct{ //this is the connected socket queue
    int *buf; //this is the queue 
    int n; //maximum number of items
    int front; //first item in the queue
    int rear; //last item in the queue
    sem_t mutex; //mutually exclusive access to the queue
    sem_t slots; //semaphore for available slots
    sem_t items; //semaphore for available items
} sbuf_t;

int getlistenfd(char *);
ssize_t readLine(int fd, void *buffer, size_t n);
char ** getDict(char *, int * );
void * serviceClient(void *);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);

int main(int argc, char ** argv){
    int listenfd; //listen socket descriptor
    int connectedfd; //connected socket descriptor
    struct sockaddr_storage client_addr; //the client address struct
    socklen_t client_addr_size; //size of client address
    char line[MAX_LINE]; //this is the line read from the client
    ssize_t bytes_read; //number of bytes read from readLine
    char client_name[MAX_LINE]; 
    char client_port[MAX_LINE]; 
    char *port; //port configuration string
    char *dict; //dict configuration string
    
    //setting the dict and port variables
    switch(argc){
        case 1: //there is no dictionary or port argument given by user
            port = DEFAULT_PORT;
            dict = DEFAULT_DICTIONARY;
            break;
        case 2:
            port = DEFAULT_PORT;
            dict = argv[1];
            break;
        case 3:
            port = argv[2];
            dict = argv[1];
            break;
        default:
            printf("There is error in the number of arguments!\n");
            exit(0);
    }
    //make dictionary data structure available to all threads
    int wordsInDict = 0;
    char ** dictionary = getDict(dict, &wordsInDict);

    //create an array of threads
    pthread_t threadPool[WORKER_COUNT];

    int i;
    for (i = 0; i < WORKER_COUNT; i++){
        //Start running the threads
        pthread_create(&threadPool[i], NULL, &serviceClient, dictionary);
    }

    //this is the listener socket on the server side
    listenfd = getlistenfd(port);

    //infinite loop
    for (;;) {
        //accept client connection
        client_addr_size=sizeof(client_addr);
        if ((connectedfd=accept(listenfd, (struct sockaddr*)&client_addr, &client_addr_size))==-1) {
            fprintf(stderr, "accept error\n");
            continue;
        }
        if (getnameinfo((struct sockaddr*)&client_addr, client_addr_size,
                client_name, MAX_LINE, client_port, MAX_LINE, 0)!=0) {
            fprintf(stderr, "error getting name information about client\n");
        } else {
            printf("accepted connection from %s:%s\n", client_name, client_port);
        }
        while ((bytes_read=readLine(connectedfd, line, MAX_LINE-1))>0) {
          printf("just read %s", line);
          write(connectedfd, line, bytes_read);
        }
        printf("connection closed\n");
        close(connectedfd);
    }
}

/*
 * given a port number or service as string, return a
 * descriptor that can be passed to accept() */
int getlistenfd(char * port){
    int listenfd, status;
    struct addrinfo hints, *res, *p;

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM; //TCP
    hints.ai_family = AF_INET; //IPV4

    if ((status = getaddrinfo(NULL, port, &hints, &res)) != 0){
        fprintf(stderr, "getaddrinfo error %s\n", gai_strerror(status));
        exit(EXIT_GETADDRINFO_ERROR);
    }

    //try to bind to the first available address/port in the list
    for (p = res; p != NULL; p = p->ai_next){
        if ((listenfd=socket(p->ai_family, p->ai_socktype, p->ai_protocol)) < 0){
            continue;
        }

        if (bind(listenfd, p->ai_addr, p->ai_addrlen) == 0){
            break;
        }
    }

    freeaddrinfo(res);
    if (p == NULL){
        exit(EXIT_BIND_FAILURE);
    }
    
    if (listen(listenfd, BACKLOG) < 0){
        close(listenfd);
        exit(EXIT_LISTEN_FAILURE);
    }
    return listenfd;
}

/* FROM KERRISK
   Read characters from 'fd' until a newline is encountered. If a newline
   character is not encountered in the first (n - 1) bytes, then the excess
   characters are discarded. The returned string placed in 'buf' is
   null-terminated and includes the newline character if it was read in the
   first (n - 1) bytes. The function return value is the number of bytes
   placed in buffer (which includes the newline character if encountered,
   but excludes the terminating null byte). */
ssize_t readLine(int fd, void *buffer, size_t n) {
    ssize_t numRead;                    /* # of bytes fetched by last read() */
    size_t totRead;                     /* Total bytes read so far */
    char *buf;
    char ch;

    if (n <= 0 || buffer == NULL) {
        errno = EINVAL;
        return -1;
    }

    buf = buffer;                       /* No pointer arithmetic on "void *" */

    totRead = 0;
    for (;;) {
        numRead = read(fd, &ch, 1);

        if (numRead == -1) {
            if (errno == EINTR)         /* Interrupted --> restart read() */
                continue;
            else
                return -1;              /* Some other error */
        } else if (numRead == 0) {      /* EOF */
            if (totRead == 0)           /* No bytes read; return 0 */
                return 0;
            else                        /* Some bytes read; add '\0' */
                break;
        } else {                        /* 'numRead' must be 1 if we get here */
            if (totRead < n - 1) {      /* Discard > (n - 1) bytes */
                totRead++;
                *buf++ = ch;
            }

            if (ch == '\n')
                break;
        }
    }

    *buf = '\0';
    return totRead;
}

//the getDict function takes in the name of the dictionary file and an integer pointer
//the file pointer opens the dictionary file and stores into buffer
//the buffer is tokenized by newline character and stored into tokens
//after the dictionary file is processed, the buffer is freed
//the tokens are returned
//the function also keeps record of number of words in the dictionary
//which is updated by pointer wordsInDict
char ** getDict(char * dictArg, int * wordsInDict){
    //open dictionary file and read as string into buffer
    FILE * fp;
    if((fp = fopen(dictArg, "r")) == NULL){ //error checking for file opening
        printf("There is error opening the file!\n");
        exit(0);
    }
    fseek(fp, 0, SEEK_END);
    int fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char * buffer = malloc(fsize);
    fread(buffer, fsize, 1, fp);
    fclose(fp);

    //tokenize the string into separate words
    int bufsize = 64;
    char **tokens = malloc(bufsize * sizeof(char*)); //allocate space for tokens
    char *token;

    if (!tokens){ //check for allocation error
        printf("allocation error\n");
        exit(EXIT_FAILURE);
    }

    token = strtok(buffer, TOK_DELIM); //get the token with selected delimiter
    while (token != NULL){ //while there is more tokens
        tokens[*wordsInDict] = token; //insert the token into tokens
        (*wordsInDict)++; //increment the position

        if (*wordsInDict >= bufsize){ //if position exceeds buffer size
            bufsize += 64; //add more buffer size
            tokens = realloc(tokens, bufsize * sizeof(char*)); //reallocate tokens memory
            if (!tokens){ //check for reallocation error
                printf("reallocation error\n");
                exit(EXIT_FAILURE);
            }
        }

        token = strtok(NULL, TOK_DELIM); //get the next token
    }
    tokens[*wordsInDict] = NULL; //last token is null terminator
    return tokens;
}

//From Bryant and O'Hallaron
//Create an empty, bounded, shared FIFO buffer with n slots
void sbuf_init(sbuf_t *sp, int n){
   sp->buf = calloc(n, sizeof(int)); //allocate memory for n int items
   sp->n = n; 
   sp->front = sp->rear = 0; //queue is empty if rear and front are equal
   sem_init(&sp->mutex, 0, 1); //binary semaphore for mutually exclusive access to queue
   sem_init(&sp->slots, 0, n); //n empty slots initially
   sem_init(&sp->items, 0, 0); //0 items initially
}

void sbuf_deinit(sbuf_t *sp){
    free(sp->buf);
}

void sbuf_insert(sbuf_t *sp, int item){
    sem_wait(&sp->slots); //sem_wait is P(). wait for available slot
    sem_wait(&sp->mutex); //this thread has the lock now
    sp->buf[(++sp->rear)%(sp->n)] = item; //insert the item
    sem_post(&sp->mutex); //sem_post is V(). unlock the mutex
    sem_post(&sp->items); //announce available item
}

int sbuf_remove(sbuf_t *sp){
    int item;
    sem_wait(&sp->items); //wait for available item
    sem_wait(&sp->mutex); //lock the queue
    item = sp->buf[(++sp->front)%(sp->n)]; //remove the item
    sem_post(&sp->mutex); //unlock the buffer
    sem_post(&sp->slots); //announce available slot
    return item;
}

void * serviceClient(void * dict){
    printf("first word: %s\n", ((char **) dict)[0]);
}

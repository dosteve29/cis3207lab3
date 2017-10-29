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

#define MAX_LINE 512

#define DEFAULT_DICTIONARY "words"
#define DEFAULT_PORT "12345"
#define TOK_DELIM " \n"
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

//function prototypes
int getlistenfd(char *);
ssize_t readLine(int fd, void *buffer, size_t n);
char ** getDict(char *, int * );
void serviceClient(int);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);
void * threadMain(void * theQueue);
int spellChecker(char * word);
int myReadLine(int theSocket, char * buffer);

//dictionary variables made global for easy access
char ** dictionary;
int wordsInDict = 0;

int main(int argc, char ** argv){
    int listenfd; //listen socket descriptor
    int connectedfd; //connected socket descriptor
    struct sockaddr_storage client_addr; //the client address struct
    socklen_t client_addr_size; //size of client address
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
    //this is the listener socket on the server side
    listenfd = getlistenfd(port);

    //make dictionary data structure available to all threads
    dictionary = getDict(dict, &wordsInDict);

    //create the queue for holding incoming connections for the worker threads
    sbuf_t *theQueue = calloc(7, sizeof(*theQueue)); //allocate memory for theQueue
    sbuf_init(theQueue, 5); //initialize with 5 slots for theQueue
    
    //create an array of threads
    pthread_t threadPool[WORKER_COUNT];

    int i;
    for (i = 0; i < WORKER_COUNT; i++){
        //Start running the threads
        pthread_create(&threadPool[i], NULL, &threadMain, theQueue);
    }

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

        //add connected socket to the queue
        sbuf_insert(theQueue, connectedfd); //sbuf_insert automatically signals new socket in the queue
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

//Clean up buffer sp
void sbuf_deinit(sbuf_t *sp){
    free(sp->buf);
}

//Insert item onto the rear of shared buffer sp
void sbuf_insert(sbuf_t *sp, int item){
    sem_wait(&sp->slots); //sem_wait is P(). wait for available slot
    sem_wait(&sp->mutex); //this thread has the lock now
    sp->buf[(++sp->rear)%(sp->n)] = item; //insert the item
    sem_post(&sp->mutex); //sem_post is V(). unlock the mutex
    sem_post(&sp->items); //announce available item
}

//Remove and return the first item from buffer sp
int sbuf_remove(sbuf_t *sp){
    int item;
    sem_wait(&sp->items); //wait for available item
    sem_wait(&sp->mutex); //lock the queue
    item = sp->buf[(++sp->front)%(sp->n)]; //remove the item
    sem_post(&sp->mutex); //unlock the buffer
    sem_post(&sp->slots); //announce available slot
    return item;
}

//the threads created from main will start this function
void * threadMain(void * theQueue){
    while(1){ //infinite loop. THE THREAD LIVES FOREVER...until parent process is dead
        sbuf_t * queuePtr = (sbuf_t *)theQueue; //pointer to casted void theQueue
        
        /*
         * here, sbuf_remove will block the thread until there is an available item
         * this is possible by sem_wait(&sp->items) in sbuf_remove
         * if there is a signal of available item, then a thread with this block will wake
         * and remove a socket from the queue
         */
        int theSocket = sbuf_remove(queuePtr);
        serviceClient(theSocket); //call the serviceClient function to spellcheck until there is no more words
        close(theSocket); //close this working socket
        printf("connection closed\n"); //print to the server that the connection closed
    }
}

//this reads from the client until there is no more to say. and splits up the line into tokens
void serviceClient(int theSocket){
    ssize_t bytes_read; //byte read from client
    char line[MAX_LINE]; //this is the line read from client
    memset(line, '\0', sizeof(line)); //memset helps with debugging
    while((bytes_read = read(theSocket, line, MAX_LINE)) > 0){ //while there is more to read from the client
        if ('\n' == line[strlen(line) - 1]){ //get rid of newline
            line[strlen(line) - 1] = ' ';
        }
        char newLine[bytes_read -1];
        memset(newLine, '\0', sizeof(newLine));
        strncpy(newLine, line, strlen(line) - 2);

        char *token = strtok(newLine, " "); //token start
        while (token != NULL){ //while there is more token
            if (spellChecker(token)){
                printf("%sThis is correct!\n", token);
            } else{
                printf("%s:This is not correct!\n", token);
            }
            memset(token, '\0', strlen(token));
            token = strtok(NULL, " ");
        }
        memset(newLine, '\0', strlen(newLine));
        memset(line, '\0', strlen(line));
    }
}

//this is the spellchecker function
int spellChecker(char * word){
    int i;
    for (i = 0; i < wordsInDict; i++){
        if (strncmp(word, dictionary[i], strlen(word) - 1) == 0)        
            return 1;
    }
    return 0;
}

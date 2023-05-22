#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <signal.h>
#include <pthread.h>
#include <semaphore.h>

#define MAXPENDING 5 /* Maximum outstanding connection requests */

pthread_mutex_t mutex; // For correct info messaging

int servClntSock;
int servHrdrSock;
int servObsrvSock;

int info_pipe[2];

struct Observer
{
    int socket;
    int is_active;
};

struct Observer observers[15];

void DieWithError(char *errorMessage)
{
    close(servClntSock);
    close(servHrdrSock);
    close(servObsrvSock);
    pthread_mutex_destroy(&mutex);
    close(info_pipe[0]);
    close(info_pipe[1]);
    perror(errorMessage);
    exit(0);
}

int createSocket(int port, in_addr_t servInAddr)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        DieWithError("socket() failed");

    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr)); /* Zero out structure */
    servAddr.sin_family = AF_INET;          /* Internet address family */
    servAddr.sin_addr.s_addr = servInAddr;  /* Any incoming interface */
    servAddr.sin_port = htons(port);        /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        DieWithError("bind() failed");

    return servSock;
}

int AcceptConnection(int servSock)
{
    int clntSock;                    /* Socket descriptor for client */
    struct sockaddr_in echoClntAddr; /* Client address */
    unsigned int clntLen;            /* Length of client address data structure */

    /* Set the size of the in-out parameter */
    clntLen = sizeof(echoClntAddr);

    /* Wait for a client to connect */
    if ((clntSock = accept(servSock, (struct sockaddr *)&echoClntAddr,
                           &clntLen)) < 0)
        DieWithError("accept() failed");

    printf("Handling %s\n", inet_ntoa(echoClntAddr.sin_addr));

    return clntSock;
}

void HandleTCPClient(int clntSocket, int hrdrSocket, int obsrvSocket)
{
    pid_t pid;       /* Client's id */
    int recvMsgSize; /* Size of received message */
    char str[150];

    /* Receive client */
    if ((recvMsgSize = recv(clntSocket, &pid, sizeof(int), 0)) < 0)
        DieWithError("recv() failed");

    sprintf(str, "Client %d is in the queue\n", pid);
    write(info_pipe[1], str, strlen(str));

    /* Send client to hairdresser */
    if (send(hrdrSocket, &pid, sizeof(int), 0) != sizeof(int))
        DieWithError("send() failed");

    sprintf(str, "Client %d leaves the queue for a haircut\n", pid);
    write(info_pipe[1], str, strlen(str));

    /* Receive notification about the end of the haircut */
    if ((recvMsgSize = recv(hrdrSocket, &pid, sizeof(int), 0)) < 0)
        DieWithError("recv() failed");

    sprintf(str, "Client %d got a haircut\nHaidresser is sleeping\n", pid);
    write(info_pipe[1], str, strlen(str));

    /* Release client */
    if (send(clntSocket, &pid, sizeof(int), 0) != sizeof(int))
        DieWithError("send() failed");

    close(clntSocket); /* Close client socket */

    sprintf(str, "Client %d left\n", pid);
    write(info_pipe[1], str, strlen(str));
}

void *AcceptObserver()
{
    int obsSock;
    for (;;)
    {
        obsSock = AcceptConnection(servObsrvSock);
        for (int i = 0; i < 15; i++)
        {
            if (observers[i].is_active == 0)
            {
                printf("Set observer to position %d\n", i);
                observers[i].socket = obsSock;
                observers[i].is_active = 1;
                break;
            }
        }
    }
}
void setObservers()
{
    for (int i = 0; i < 15; i++)
    {
        observers[i].is_active = 0;
    }

    pthread_t thread;
    pthread_create(&thread, NULL, AcceptObserver, NULL);
}

void *WriteInfo()
{
    char str[150];
    ssize_t rdBytes;
    for (;;)
    {
        rdBytes = read(info_pipe[0], str, 150);
        if (rdBytes < 0)
        {
            DieWithError("Can't read from pipe");
        }
        str[rdBytes] = '\0';
        pthread_mutex_lock(&mutex);
        for (int i = 0; i < 15; ++i)
        {
            if (observers[i].is_active == 1)
            {
                if (send(observers[i].socket, str, strlen(str), 0) < 0)
                {
                    observers[i].is_active = 0;
                    close(observers[i].socket);
                    printf("Observer disconnected\n");
                }
            }
        }
        pthread_mutex_unlock(&mutex);
    }
}

void StartWriter()
{
    if (pipe(info_pipe) < 0)
    {
        DieWithError("Can\'t open the info pipe\n");
    }

    pthread_t thread;
    pthread_create(&thread, NULL, WriteInfo, NULL);
}

void sigfunc(int sig)
{
    if (sig != SIGINT && sig != SIGTERM)
    {
        return;
    }

    close(servClntSock);
    close(servHrdrSock);
    pthread_mutex_destroy(&mutex);
    close(info_pipe[0]);
    close(info_pipe[1]);
    printf("disconnected\n");
    exit(0);
}

int main(int argc, char *argv[])
{
    signal(SIGINT, sigfunc);
    signal(SIGTERM, sigfunc);

    int clntSock;                    /* Socket descriptor for client */
    int hrdrSock;                    /* Socket descriptor for hairdresser */
    int obsrvSock;                   /* Socket descriptor for observer */
    struct sockaddr_in echoServAddr; /* Local address */
    struct sockaddr_in echoClntAddr; /* Client address */
    unsigned int servClntPort;
    unsigned int servHrdrPort;
    unsigned int servObsrvPort;

    if (argc != 5) /* Test for correct number of arguments */
    {
        fprintf(stderr, "Usage:  %s <Server Address> <Port for Clients> <Port for Haidresser>\n", argv[0]);
        exit(1);
    }

    in_addr_t servAddr;
    if ((servAddr = inet_addr(argv[1])) < 0)
    {
        DieWithError("Invalid address");
    }

    servClntPort = atoi(argv[2]);
    servHrdrPort = atoi(argv[3]);
    servObsrvPort = atoi(argv[4]);

    servClntSock = createSocket(servClntPort, servAddr);
    servHrdrSock = createSocket(servHrdrPort, servAddr);
    servObsrvSock = createSocket(servObsrvPort, servAddr);

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servHrdrSock, MAXPENDING) < 0)
        DieWithError("listen() failed");
    hrdrSock = AcceptConnection(servHrdrSock);

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servObsrvSock, MAXPENDING) < 0)
        DieWithError("listen() failed");
    setObservers(); // Accept observers in another thread

    pthread_mutex_init(&mutex, NULL);
    StartWriter();

    char str[150];
    sprintf(str, "Hairdresser's is open\n");
    write(info_pipe[1], str, strlen(str));

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servClntSock, MAXPENDING) < 0)
        DieWithError("listen() failed");

    for (;;)
    {
        clntSock = AcceptConnection(servClntSock);
        HandleTCPClient(clntSock, hrdrSock, obsrvSock);
    }
}

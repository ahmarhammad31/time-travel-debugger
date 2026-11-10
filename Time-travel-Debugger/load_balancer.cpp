// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int CLIENT_TIMEOUT_SEC = 30;          // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the load balancer forever
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
const int UPLOAD_TIMEOUT_SEC = 5;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;


public:
    // Implement these functions:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while(!isEmpty())
        {
            dequeue();
        }
    }
    void enqueue(const T &val)
    {
        Node* n = new Node();
        n->data = val;
        n->next = nullptr;
        if (rear == nullptr)
        {
            front = n;
        }
        else
        {
            rear->next = n;
        }
        rear = n;
        count++;
    }
    T dequeue()
    {
        if (isEmpty())
        {
            throw runtime_error("Queue is empty");
        }
        Node* temp = front;
        T val = temp->data;
        front = front->next;
        if (front == nullptr)
        {
            rear = nullptr;
        }
        delete temp;
        count--;
        return val;
    }
    bool isEmpty()
    {
        return front == nullptr;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

// ---- Basic socket helpers ----


int createListenSocket(int port)
{ 
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
 
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
 
    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        exit(1);
    }
    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        exit(1);
    }
    return sock;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;
 
    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
 
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 || connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.
    string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    return system(command.c_str()) == 0;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
        return sock;
 
    if (!startServer(serverPort))
        return -1;
 
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++)
    {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock;
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        sendError(c.sock, "backend server failed to start");
        return;
    }
 
    timeval wait;
    wait.tv_sec = UPLOAD_TIMEOUT_SEC;
    wait.tv_usec = 0;
    setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
 
    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE + 4];
 
    TrailerChecker upload;
    bool serverOk = true;
    while (true)
    {
        ssize_t n = recv(c.sock, in, sizeof(in), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size_t bodyLen = upload.feed(in, n, out);
        if (bodyLen > 0 && serverOk)
            serverOk = sendAll(serverSock, out, bodyLen);
    }
 
    if (!upload.verify())
    {
        close(serverSock);
        sendError(c.sock, "upload checksum mismatch");
        return;
    }
    if (!serverOk)
    {
        close(serverSock);
        sendError(c.sock, "backend server failed to start");
        return;
    }
    sendAll(serverSock, upload.trailer(), 4);
    shutdown(serverSock, SHUT_WR);
 
    TrailerChecker reply;
    bool clientOk = true;
    size_t totalReceived = 0;
    while (true)
    {
        ssize_t n = recv(serverSock, in, sizeof(in), 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        totalReceived += n;
        size_t bodyLen = reply.feed(in, n, out);
        if (bodyLen > 0 && clientOk)
            clientOk = sendAll(c.sock, out, bodyLen);
    }
    close(serverSock);
 
    if (totalReceived == 0)
    {
        sendError(c.sock, "backend server closed the connection without a reply");
        return;
    }
    if (clientOk && reply.verify())
        sendAll(c.sock, reply.trailer(), 4);
}


void refuseClient(int sock, const string &message)
{
    sendError(sock, message);
    shutdown(sock, SHUT_WR);
 
    timeval wait;
    wait.tv_sec = 0;
    wait.tv_usec = 100000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
 
    char junk[IO_BUFFER_SIZE];
    while (recv(sock, junk, sizeof(junk), 0) > 0)
    {
    }
    close(sock);
}

// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).
    
    while (flag)
    {
        sockaddr_in addr;
        socklen_t addrLen = sizeof(addr);
        int sock = accept4(listenSock, (sockaddr *)&addr, &addrLen, SOCK_CLOEXEC);
        if (sock < 0)
        {
            perror("accept");
            continue;
        }
 
        ClientInfo client;
        client.sock = sock;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        client.address = ip;
        client.port = ntohs(addr.sin_port);
        client.arrivedAt = time(nullptr);
 
        bool queueFull = false;
        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
            queueFull = true;
        else
            pendingClients.enqueue(client);
        queueMutex.unlock();
 
        if (queueFull)
            refuseClient(sock, "server busy, try again later");
    }
}
void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.
    
    while (flag)
    {
        ClientInfo client;
        bool gotClient = false;
 
        queueMutex.lock();
        if (!pendingClients.isEmpty())
        {
            client = pendingClients.dequeue();
            gotClient = true;
        }
        queueMutex.unlock();
 
        if (!gotClient)
        {
            usleep(50000);
            continue;
        }
 
        if (difftime(time(nullptr), client.arrivedAt) > CLIENT_TIMEOUT_SEC)
        {
            close(client.sock);
            continue;
        }
 
        relayOneClient(client);
        close(client.sock);
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().
    if (argc != 3)
    {
        cerr << "usage: ./load_balancer <server_ip> <server_port>" << endl;
        return 1;
    }
    serverHost = argv[1];
    serverPort = atoi(argv[2]);
 
    signal(SIGINT, handler);// whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit

    int listenSock = createListenSocket(lbPort);
    cout << "load balancer listening on port " << lbPort << endl;
 
    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();
 
    dispatcherLoop();
 
    close(listenSock);
    return 0;
}
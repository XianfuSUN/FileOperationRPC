/** Author: Xianfu Sun
 * RPC protocals:
 * the string sent to the server is structured like this:
 * operation\nparma\nparam2....
 *
 * We only speak to a fixed client
 * the socket will be opened if none opened
 * and close the socket if the final file is closed
 * if no file is currently open, the other operations
 * that does not rely on the opened file need to close the socket itself
 */

#define _GNU_SOURCE
#define MAXLEN 512  //maxium message to read at one time by the client
#define FD_OFFSET 1024 //offset of RPC to the normal descriptors

#include <dlfcn.h>
#include <stdio.h>
#include<stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include "dirtree.h"

//SOCK_FD record the child process's descriptor that is currently connected to
int SOCK_FD;
//if it is now connect to a server
int CONNECT = 0;
//number of file has been opened by the client
int FILE_OPENED = 0;
// The following lines declares function pointers with the same prototype as the original functions.
int (*orig_open)(const char *pathname, int flags, ...);
ssize_t (*orig_read)(int fd, void* buf, size_t count);
ssize_t (*orig_write)(int fd, const void* buf, size_t count);
int (*orig_close)(int fd);
struct dirtreenode* (*orig_getdirtree)(const char* path);
void (*orig_freedirtree)(struct dirtreenode* dt);
off_t (*orig_lseek)(int fd, off_t offset, int whence);
int (*orig_stat)(int ver, const char* filename, struct stat* buf);
int (*orig_unlink)(const char* pathname);
ssize_t (*orig_getdirentries)(int fd, char *buf, size_t nbytes , off_t* basep);

/**
 * the structure to store the parameters
 * passed to the server
 */
typedef struct message {
    char operation[20];//operation name
    void* params;//pointer to a list of parameter
}Message;

//function prototypes
char* sendServer(char* msg, int len);
int connectServer();
int serialize(struct dirtreenode *root, char *buf);
int deserialize(char *buf, struct dirtreenode **parent, int *offset);

/**
 * deserialize the buf into the directory tree
 * @param buf the buf that contains the data
 * @param parent the parent node
 * @param offsetp the offset indicate where should the data be read from
 * @return success on 0, failed on 1
 */
int deserialize(char *buf, struct dirtreenode **parent, int *offsetp) {
    int offset = *offsetp;
    //read number of directories and lenght of name
    int numdir = *(int *)(buf + offset);
    int name_len = * (int *)(buf + offset + sizeof(int));

    //malloc a new node
    struct dirtreenode *node = (struct dirtreenode *)malloc(sizeof(struct dirtreenode));
    //malloc a name on the heap
    char *name = (char *)malloc(name_len + 1);
    memcpy(name, buf + 2*sizeof(int) + offset, name_len);
    name[name_len] = '\0';
    //set the name of current name and number of dirs
    node->name = name;
    node->num_subdirs = numdir;

    *offsetp = offset + 2 * sizeof(int) + name_len;//update position in buf
    if (numdir > 0) {
        //malloc a lists of subdir's pointers
        struct dirtreenode **subdirs = (struct dirtreenode **)malloc(numdir*sizeof(struct dirtreenode *));
        //set this list to the node's subdirs field
        node->subdirs = subdirs;
        //connect to other subdirs
        int i;
        for ( i = 0; i < numdir; i++) {
            deserialize(buf, node->subdirs + i, offsetp);
        }
    } else {
        node->subdirs = NULL;
    }
    //connect the new node to the parent
    *parent = node;
    return 0;

}
/**
 * interposed open library
 * package the parameters and send them to the server
 * @param pathname
 * @param flags
 * @param ...
 * @return same as normal open, set the errno appropriately
 */
int open(const char *pathname, int flags, ...) {
    mode_t m=0;
    if (flags & O_CREAT) {
        va_list a;
        va_start(a, flags);
        m = va_arg(a, mode_t);
        va_end(a);
    }
    int altLen = strlen(pathname)*2;
    int len = (MAXLEN>altLen)?MAXLEN:altLen;

    //set up the packaged arguments
    Message *sent = (Message *)malloc(len);
    char *operation = "open";
    int pathlen = strlen(pathname);
    strcpy(sent->operation, operation);
    memcpy(&sent->params, (void *)&flags, sizeof(int));
    memcpy(&sent->params + sizeof(int), (void *)&m, sizeof(int));
    memcpy(&sent->params + 2 *sizeof(int), (void *)pathname, pathlen + 1);

    char *receive;
    receive = sendServer((char *)sent, len);

    int fd = *(int*)receive;
    fprintf(stderr, "the descriptor allocated is:%d\n", fd);

    if (fd < 0) { //handling operations on error
        int err_code = * (int *)(receive + 4);
        errno = err_code;
        fprintf(stderr, "open failed ,err code:%d\n", errno);
        free(receive - sizeof(int));
        return fd;
    } else {
        FILE_OPENED += 1;
    }
    free(receive - sizeof(int));
    free(sent);
    return fd + FD_OFFSET;
}

/**
 * read the file
 * @param fd
 * @param buf
 * @param count
 * @return success on 0, -1 on failed
 */
ssize_t read(int fd, void* buf, size_t count) {
    if (fd < FD_OFFSET) { //read on a local descriptor
        return orig_read(fd, buf, count);
    }
    //set up the packaged paramters
    int off_fd = fd - FD_OFFSET;
    int len = sizeof(Message) + MAXLEN;
    fprintf(stderr, "in read lib, the fd is: %d, the count is %zu\n", fd, count);
    Message *sent = (Message *)malloc(len);
    char *operation = "read";
    strcpy(sent->operation, operation);
    memcpy(&sent->params, (char *)&off_fd, sizeof(int));//copy fd
    memcpy(&sent->params + sizeof(int), (char *)&count, sizeof(size_t));//copy count

    //malloc a buffer to store the read contents
    char *receive;
    receive = sendServer((char *)sent, len);
    ssize_t read_rv = *(ssize_t *)receive;
    fprintf(stderr, "return value from read is %zu\n", read_rv);

    if (read_rv > 0) {
        memcpy(buf, (void *)(receive + sizeof(size_t)), count);
    } else { //handling error
        errno = *(int *)(receive + sizeof(size_t));
        fprintf(stderr, "errors in read!, err code:%d\n", errno);
    }
    free(receive - sizeof(int));
    free(sent);
    return read_rv;
}

/**
 * package the parameters for the write function
 * @param fd
 * @param buf
 * @param count
 * @return the size of written bytes or -1 on error.
 */
ssize_t write(int fd, const void* buf, size_t count) {
    if (fd < FD_OFFSET) {//write on a local descriptor
        return orig_write(fd, buf, count);
    }
    // set up the length
    int off_fd = fd - FD_OFFSET;
    int length = (MAXLEN > 2 * count)?MAXLEN:2*count;
    Message *msg = (Message *)malloc(length);
    char *operation = "write";
    strcpy(msg->operation, operation);
    memcpy(&msg->params, (void *)&off_fd, sizeof(int));
    memcpy(&msg->params + sizeof(int), (void *)&count, sizeof(size_t));
    memcpy(&msg->params +sizeof(int) + sizeof(size_t), buf, count);

    char *receive;
    receive = sendServer((char *)msg, length);

    ssize_t rv = *(ssize_t*)receive;
    fprintf(stderr, "the bytes written is %zu\n", rv);

    if (rv < 0) { //handling error
        int err_code = * (int *)(receive + sizeof(ssize_t));
        errno = err_code;
        fprintf(stderr, "write failed, err code: %d\n", errno);
    }
    free(receive - sizeof(int));
    free(msg);
    return rv;
}

/**
 * close the file
 * @param fd descriptor of the file
 * @return success on 0, failed on -1
 */
int close(int fd) {
    char* receive;
    fprintf(stderr, "fd in client is %d\n", fd);

    if (fd < FD_OFFSET) { //close a local file
        return orig_close(fd);
    } else if (fd == FD_OFFSET) { //close the socket to the server
        return orig_close(SOCK_FD);
    }

    //set up parameters
    int off_fd = fd - FD_OFFSET;
    int len = sizeof(Message) + sizeof(int);
    Message *msg = (Message *)malloc(len);
    char *operation = "close";
    strcpy(msg->operation, operation);
    memcpy(&msg->params, (void *)&off_fd, sizeof(int));

    receive = sendServer((char *)msg, len);
    int state = *(int*)receive;
    fprintf(stderr, "the close state is %d\n", state);
    if (state >= 0) {
        FILE_OPENED -= 1; //deduct one opened file
    } else { //handling the error
        errno = *(int *)(receive + sizeof(int));
        fprintf(stderr, "close error!, errno: %d\n", errno);
    }
    if (FILE_OPENED <= 0) { //close the socket if final file closed
        close(FD_OFFSET);
        CONNECT = 0;
    }
    free(receive - sizeof(int));
    free(msg);
    return state;
}

/**
 * the server will construct a dirtree and send the serialized
 * version to the client, the client deseirialized it and return
 * the root node of the tree.
 * @param path, path
 * @return root node to the dir tree
 */
struct dirtreenode* getdirtree(const char* path) {
    int length = strlen(path) + MAXLEN;
    Message *sent = (Message *)malloc(length);
    char *operation = "getdirtree";
    strcpy(sent->operation, operation);
    memcpy(&sent->params, path, strlen(path));

    char *receive;
    receive = sendServer((char *)sent, length);

    int offset = 0;
    struct dirtreenode *root;
    deserialize(receive, &root, &offset);
    free(sent);
    free(receive - sizeof(int));
    return root;
}

/**
 *free the directory tree
 * @param dt structure to the dirtree
 */
void freedirtree(struct dirtreenode* dt) {
    fprintf(stderr, "free dirtree\n");
    //no need to be a RPC, the structure is malloced on local memory
    return orig_freedirtree(dt);
}

/**
 *lseek
 * @param fd
 * @param offset
 * @param whence
 * @return
 */
off_t lseek(int fd, off_t offset, int whence) {
    if (fd < FD_OFFSET) { //lseek a local file
        return orig_lseek(fd, offset, whence);
    }
    //set up parameters
    int off_fd = fd - FD_OFFSET;
    int length = sizeof(Message) + MAXLEN;
    Message *sent = (Message *)malloc(length);
    char *operation = "lseek";
    strcpy(sent->operation, operation);
    memcpy(&sent->params, (void *)&off_fd, sizeof(int));
    memcpy(&sent->params + sizeof(int), (void *)&offset, sizeof(off_t));
    memcpy(&sent->params + sizeof(int) + sizeof(off_t),
            (void *)&whence, sizeof(int));

    char * receive;
    receive = sendServer((char *)sent, length);
    off_t ls_rv = *(off_t *)receive;
    if (ls_rv < 0) { //handling error
        errno = *(int *)(receive + sizeof(off_t));
        fprintf(stderr, "lseek failed, err code: %d\n", errno);
    }
    free(sent);
    free(receive - sizeof(int));
    return ls_rv;
}

/**
 * stat function
 * @param ver
 * @param pathname
 * @param buf
 * @return
 */
int __xstat(int ver, const char* pathname, struct stat *buf) {
    int length = strlen(pathname) + sizeof(int) + MAXLEN;
    Message *sent = (Message *)malloc(length);
    char *operation = "stat";
    strcpy(sent->operation, operation);
    memcpy(&sent->params, (void *)&ver, sizeof(int));
    memcpy(&sent->params + sizeof(int), pathname, strlen(pathname));
    fprintf(stderr, "in client stat, ver %d, pathname %s\n", ver, pathname);

    char *receive;
    receive = sendServer((char *)sent, length);

    int stat_rv = *(int *)receive;
    if (stat_rv > 0) {
        memcpy(buf, (struct stat *)(receive + sizeof(int)), sizeof(struct stat));
    } else {//handling error
        errno = *(int *)(receive + sizeof(int));
        fprintf(stderr, "stat failed, err code:%d\n", errno);
    }
    free(sent);
    free(receive - sizeof(int));
    return stat_rv;
}

/**
 * unlink the file
 * @param pathname
 * @return
 */
int unlink(const char* pathname) {
    int length = strlen(pathname) + MAXLEN;
    Message *sent = (Message *)malloc(length);
    char *operation = "unlink";
    strcpy(sent->operation, operation);
    memcpy(&sent->params, pathname, strlen(pathname));
    char *receive;
    receive = sendServer((char *)sent, length);

    int rv = *(int *)receive;
    if (rv < 0) { // handling error
        errno = *(int *)(receive + sizeof(int));
        fprintf(stderr, "unlink faield, err code:%d\n", errno);
    }
    free(sent);
    free(receive - sizeof(int));
    return rv;
}

/**
 * get the directory entries
 * @param fd
 * @param buf
 * @param nbytes
 * @param basep
 * @return
 */
ssize_t getdirentries(int fd, char *buf, size_t nbytes , off_t* basep) {
    if (fd < FD_OFFSET) { //operation on a local file
        return orig_getdirentries(fd, buf, nbytes, basep);
    }
    int off_fd = fd - FD_OFFSET;
    int length = MAXLEN + 2 * sizeof(int) + sizeof(size_t);
    Message *sent = (Message *)malloc(length);
    char *operation = "getdirentries";
    strcpy(sent->operation, operation);
    memcpy(&sent->params, (void *)&off_fd, sizeof(int));
    memcpy(&sent->params + sizeof(int), (void *)&nbytes, sizeof(size_t));
    memcpy(&sent->params + sizeof(int) + sizeof(size_t),
            (void *)basep, sizeof(off_t));

    //expecting to receive a ssize_t return value, a off_t basep and nbytes
    //wrote to the buffer
    char *receive;
    receive = sendServer((char *)sent, length);
    free(sent);

    ssize_t rv = *(ssize_t *)receive;
    if (rv > 0) {
        off_t basep_val = *(off_t *)(receive + sizeof(ssize_t));
        * basep = basep_val;
        char *buf_rv = (char *)(receive + sizeof(ssize_t) + sizeof(off_t));
        memcpy(buf, buf_rv, nbytes);

    } else { //handling error
        errno = * (int *)(receive + sizeof(ssize_t));
        fprintf(stderr, "getdirentries failed, err code:%d\n", errno);
    }
    free(receive - sizeof(int));
    return rv;
}

/**
 * connect to the server and assign the global variable the
 * socket to a global variable
 * @return 1 if sucess, -1 if fail
 */
int connectServer() {
    int sockfd, res;
    char* serverip;
    char* serverport;
    int port;

    // Get environment variable indicating the ip address of the server
    serverip = getenv("server15440");
    if (serverip) {
        fprintf(stderr, "Got environment variable server15440: %s\n", serverip);
    }
    else {
        fprintf(stderr, "Environment variable server15440 not found.  Using 127.0.0.1\n");
        serverip = "127.0.0.1";
    }

    // Get environment variable indicating the port of the server
    serverport = getenv("serverport15440");
    if (serverport) {
        fprintf(stderr, "Got environment variable serverport15440: %s\n", serverport);
    }
    else {
        fprintf(stderr, "Environment variable serverport15440 not found.  Using 15940\n");
        serverport = "10707";
    }
    port = (unsigned short)atoi(serverport);

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    //set up address structure
    struct sockaddr_in srv;
    memset(&srv, 0, sizeof(srv));
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = inet_addr(serverip);
    srv.sin_port = htons(port);

    res = connect(sockfd, (struct sockaddr*)&srv, sizeof(srv));
    if (res < 0) {
        fprintf(stderr, "connect to the server failed\n");
        return -1;
    } else {
        SOCK_FD = sockfd; //assign this sockfd to the global variable
        return 1;
    }
}

/**
 * add the size of the msg at the front and then send to the server
 * receive the response from the server and return sizeof(int) + pointer
 * because the first four bytes will be the length and is not used
 * @param msg msg to be sent
 * @param receive buffer for the received message
 * @param sendlen length of the message
 * @return
 */
char *sendServer(char* msg, int sendlen) {
    if (!CONNECT) {
        int res = connectServer();
        if (res < 0) {
            fprintf(stderr, "connect to the server failed!\n");
            return NULL;
        } else {
            CONNECT = 1;
        }
    }

    int rv = 0;
    char *buf2 = (char *)malloc(MAXLEN);
    char *receive;

    char *sent_msg = (char *) malloc(sendlen + sizeof(int));
    int bytes_sent = sendlen + sizeof(int);
    fprintf(stderr, "the client about to sent %d\n", bytes_sent);
    memcpy(sent_msg, (char *) &bytes_sent, sizeof(int));
    memcpy(sent_msg + sizeof(int), msg, sendlen);
    // send the msg
    send(SOCK_FD, sent_msg, bytes_sent, 0);

    //should use multiple receive here
    int anticipated = 0; //bytes anticipated to receive
    int bytes_received = 0; //already received bytes
    int read = 0; //have read the first four bytes or not
    do {
        rv = recv(SOCK_FD, buf2, MAXLEN, 0);
        bytes_received += rv;
        if (!read) {
            if (bytes_received < 4 && rv > 0) {
                buf2  = (char *)(buf2 + bytes_received);
                continue;
            } else {
                anticipated = *(int *)buf2;
                receive = (char *)malloc(anticipated + 1);
                fprintf(stderr, "the client anticipates to receive %d bytes from server\n", anticipated);
                read = 1;
                //ignore the first four bytes in buf2, which is only the size of message
            }
        }
        memcpy(receive + bytes_received - rv, buf2, rv);
        memset(buf2, '\0', MAXLEN);
    } while(bytes_received < anticipated && rv > 0);

    fprintf(stderr, "total bytes received %d\n", bytes_received);
    free(sent_msg);
    free(buf2);
    //ignore the first four bytes, each function should free
    //receive  - sizeof(int)
    return receive + sizeof(int);
}

//This function is automatically called when program is started
void _init(void) {
    // set function pointer orig_open to point to the original open function
    orig_open = dlsym(RTLD_NEXT, "open");
    orig_read = dlsym(RTLD_NEXT, "read");
    orig_write = dlsym(RTLD_NEXT, "write");
    orig_close = dlsym(RTLD_NEXT, "close");
    orig_getdirtree = dlsym(RTLD_NEXT, "getdirtree");
    orig_freedirtree = dlsym(RTLD_NEXT, "freedirtree");
    orig_lseek = dlsym(RTLD_NEXT, "lseek");
    orig_stat = dlsym(RTLD_NEXT, "__xstat");
    orig_unlink = dlsym(RTLD_NEXT, "unlink");
    orig_getdirentries = dlsym(RTLD_NEXT, "getdirentries");

    fprintf(stderr, "Init mylib\n");
}

/**
 * this function called when library is unloaded
 */
void _fini(void) {
    fprintf(stderr, "library closed\n");
    close(FD_OFFSET);//close the client socket
}
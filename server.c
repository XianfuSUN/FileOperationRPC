/**
* Author: Xianfu Sun
* we use a individual child process to handle one client
* the child process exit if the client close the sockets
*/

#include <stdio.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <stdbool.h>
#include <errno.h>
#include<sys/stat.h>
#include<dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdarg.h>
#include "dirtree.h"
#include <signal.h>
#include <sys/wait.h>

#define MAXMSGLEN 100
#define MAXSIZE 1024

/**
 * the structure to store the parameters
 * passed to the server
 */
typedef struct message {
    char operation[20];//name of the operation
    void* params;//pointer to the parameters
}Message;

//function prototypes
int do_open(Message* msg, int sessfd);
ssize_t do_write(Message* msg, int sessfd);
int do_close(Message* msg, int sessfd);
ssize_t do_read(Message *msg, int sessfd);
int do_stat(Message *msg, int sessfd);
int do_lseek(Message *msg, int sessfd);
int do_unlink(Message *msg, int sessfd);
ssize_t do_getdirentries(Message *msg, int sessfd);
int do_getdirtree(Message *msg, int sessfd);
int do_stuff(int sessfd);
int send_back(int sessfd, char *response, int len);


/**
 * global variables
 */
int MAX_SERIALIZED_SIZE = MAXSIZE; //max size for serialization

/**
 * signal handler for sigchld, reap the zombie children
 * @param sig
 */
void handle_sigchld(int sig) {
    int saved_errno = errno;
    while (waitpid((pid_t)(-1), 0, WNOHANG) > 0) {}
    errno = saved_errno;
}

/**
 * use DFS to serialized dirtree
 * copy the serialzied dirtree to the buf, if buf is not enough, reallocate it
 * the node follows, numofsubdir, lenofname, name
 * @param root root node to the tree
 * @param buf buf to write the data
 * @param sizep the pointer to the size of current buffer
 * @return a new buf to the data
 */
char *serialize(struct dirtreenode *root, char *buf, int* sizep) {
    int nodesize = strlen(root->name) + 2 * sizeof(int);//cur node size
    int cursize = *sizep;
    if (nodesize + cursize > MAX_SERIALIZED_SIZE) {
        //if not enough memory, realloc to the double size
        buf = (char *)realloc(buf, 2*MAX_SERIALIZED_SIZE);
        MAX_SERIALIZED_SIZE *= 2;
    }
    *sizep = cursize + nodesize;//update cur_size
    //copy current node inside the buffer
    int subdir_num = root->num_subdirs;
    int name_len = strlen(root->name);//copy name
    memcpy(buf + cursize, (char *)&subdir_num, sizeof(int));//copy subdir_num
    memcpy(buf + cursize + sizeof(int), (char *)&name_len, sizeof(int));//write name length
    memcpy(buf + cursize + 2*sizeof(int), root->name, name_len);//write name
    //pre-order traverse
    int i;
    for (i = 0; i < subdir_num; i++) {
        buf = serialize(root->subdirs[i], buf, sizep);//serialze other subtrees
    }
    return buf;
}

/**
 * get the dirtree and serialized it and send to the client
 * @param msg package of arguments
 * @param sessfd fd to contact client
 * @return success on 0
 */
int do_getdirtree(Message *msg, int sessfd) {
    fprintf(stderr, "do getdirtrees\n");
    char *pathname = (char *)(&msg->params);
    fprintf(stderr, "the pathname is %s\n", pathname);
    struct dirtreenode *dir = getdirtree(pathname);
    char *buf = (char *)malloc(MAXSIZE);
    int size = 0;
    buf = serialize(dir, buf, &size);
    send_back(sessfd, buf, size);
    free(msg);
    free(buf);
    freedirtree(dir);
    return 0;
}

/**
 * unpack the message and open the file and return the
 * file descriptor
 * @param msg the message needed to be unpack
 * @return the file descriptor that it opens
 */
int do_open(Message* msg, int sessfd) {
    fprintf(stderr, "do open!\n");
    int flag = * (int *)(&msg->params);
    int mode = *(int *)(&msg->params + sizeof(int));
    char *pathname = (char *)(&msg->params + 2 * sizeof(int));
    fprintf(stderr, "pathname in do_open: %s\n", pathname);

    int fd = open(pathname, flag, mode);
    fprintf(stderr, "the descriptor is %d\n", fd);

    if (fd > 0) {
        char response[5];
        memcpy(response, (char *) &fd, 4);
        response[4] = '\0';
        send_back(sessfd, response, 5);
    } else {
        fprintf(stderr, "server: errors in open, err code: %d\n", errno);
        char response[9];
        memcpy(response, (char *) &fd, 4);
        int errocode = errno;
        memcpy(response + 4, (char *) &errocode, 4);
        response[8] = '\0';
        send_back(sessfd, response, 9);
    }
    free(msg);
    return fd;
}

/**
 * unpack the reading data read it into a buf and send back to client
 * @param msg packed parameters, it is like fd, count
 * @param sessfd
 * @return return value of read
 */
ssize_t do_read(Message *msg, int sessfd) {
    fprintf(stderr, "do read!\n");
    int fd = * (int *)(&msg->params);
    size_t count = *(size_t *)(&msg->params + sizeof(int));
    char *buf = (char *)malloc(count);
    char *response = (char *)malloc(count + sizeof(ssize_t));

    ssize_t read_rv = read(fd, buf, count);

    if (read_rv >= 0) {
        memcpy(response, (char *)&read_rv, sizeof(ssize_t));//copy return value
        memcpy(response + sizeof(ssize_t), buf, count);//copy the read content
        int length = count + sizeof(ssize_t);
        send_back(sessfd, response, length);
    } else {
        //handle error
        fprintf(stderr, "do_read faield, error code:%d\n",errno);
        int length = sizeof(ssize_t) + sizeof(int);
        int error_code = errno;
        memcpy(response, (char *)&read_rv, sizeof(ssize_t));//copy return val
        memcpy(response + sizeof(ssize_t), (char *)&error_code, sizeof(errno));//copy errno
        send_back(sessfd, response, length);
    }
    free(buf);
    free(msg);
    free(response);
    return 0;
}

/**
 * unpack the parameters for the write call
 * the message is packed as oeration\nfd\ncount\nbuf\n
 * @param msg package of the parameters
 * @return return value of the write
 */
ssize_t do_write(Message *msg, int sessfd){
    fprintf(stderr, "do write!\n");
    //char* operation = msg->operation
    int fd = * (int *) &msg->params;
    size_t count = *(size_t *)(&msg->params + sizeof(int));
    void *writeBuf = (void *)(&msg->params + sizeof(int) + sizeof(size_t));
    fprintf(stderr, "read fd is %d, count is %zu\n", fd, count);
    ssize_t size = write(fd, writeBuf, count);
    if (size > 0) {
        char response[sizeof(ssize_t) + 1];
        memcpy(response, (char *) &size, sizeof(ssize_t));//copy return val
        response[8] = '\0';
        send_back(sessfd, response, sizeof(ssize_t) + 1);
    } else {
        fprintf(stderr, "server: errors in write, %d\n", errno);
        char response[sizeof(ssize_t) + sizeof(int) + 1];
        memcpy(response, (char *) &size, sizeof(ssize_t) + sizeof(int) + 1);//copy return val
        int err_code = errno;
        memcpy(response + 8, (char *) &err_code, sizeof(errno));//copy errno
        response[12] = '\0';
        send_back(sessfd, response, sizeof(ssize_t) + sizeof(size_t) + 1);
    }
    free(msg);
    return size;
}

/**
 * unpack the message and close the file descriptor
 * @param msg packaged msg
 * @param sessfd descriptor connect to the client
 * @return 0 on success
 */
int do_close(Message *msg, int sessfd) {
    fprintf(stderr, "do close\n");
    int fd = *(int *)&msg->params;
    fprintf(stderr, "close the descriptor %d\n", fd);
    int state = close(fd);
    fprintf(stderr, "close op %d\n", state);
    char *response = (char *)malloc(sizeof(int) * 2);
    if (state >= 0) {
        memcpy(response, (char *) &state, sizeof(int));
        send_back(sessfd, response, sizeof(int));
    } else {
        fprintf(stderr, "do_close failed, errcode: %d\n", errno);
        int err_code = errno;
        memcpy(response, (char *)&state, sizeof(int));//copy return val
        memcpy(response + sizeof(int), (char *)&err_code, sizeof(int)); //copy errno
        send_back(sessfd, response, 2*sizeof(int));
    }
    free(msg);
    free(response);
    return fd;
}

/**
 * stat operation
 * @param msg parameters package
 * @param sessfd
 * @return success on 0
 */
int do_stat(Message *msg, int sessfd) {
    fprintf(stderr, "do_stat\n");
    int ver = *(int *)&msg->params;
    char *pathname = (char *)(&msg->params + sizeof(int));
    struct stat *buf = (struct stat *)malloc(sizeof(struct stat));
    int rv_stat = __xstat(ver, pathname, buf);

    int length = sizeof(int) + sizeof(struct stat);
    char *response = (char *)malloc(length);
    if (rv_stat > 0) {
        memcpy(response, (char *)&rv_stat, sizeof(int));//copy return val
        memcpy(response + sizeof(int), (char *)buf, sizeof(struct stat));//copy stat
        send_back(sessfd, response, length);
    } else {
        fprintf(stderr, "do_stat failed, err code %d", errno);
        int err_code = errno;
        memcpy(response, (char *)&rv_stat, sizeof(int));//copy return val
        memcpy(response + sizeof(int), (char *)&err_code, sizeof(errno));//copy errno
        send_back(sessfd, response, length);
    }
    free(msg);
    free(buf);
    free(response);
    return rv_stat;
}

/**
 * do lseek and send back response to the client
 * @param msg
 * @param sessfd
 * @return success on 0
 */
int do_lseek(Message *msg, int sessfd) {
    fprintf(stderr, "do_lseek\n");
    int fd = *(int *)&msg->params;
    off_t offset = *(off_t *)(&msg->params + sizeof(int));
    int whence = *(int *)(&msg->params + sizeof(int) + sizeof(off_t));
    off_t ls_rv = lseek(fd, offset, whence);

    char *response = (char *)malloc(2 * sizeof(off_t));
    int length = 2 * sizeof(off_t);
    memcpy(response, (char *)&ls_rv, sizeof(off_t));//copy return val
    if (ls_rv < 0) {
        fprintf(stderr, "do_lseek failed, erro code: %d\n", errno);
        int err_code = errno;
        memcpy(response + sizeof(off_t), (char *)&err_code, sizeof(errno));//copy errno
    }
    send_back(sessfd, response, length);
    free(msg);
    free(response);
    return ls_rv;
}

/**
 * do unlink
 * @param msg
 * @param sessfd
 * @return
 */
int do_unlink(Message *msg, int sessfd) {
    fprintf(stderr, "do_unlink!\n");
    char *pathname = (char *)(&msg->params);
    int rv = unlink(pathname);

    char *response = (char *)malloc(2 * sizeof(int));
    int length = 2 * sizeof(int);
    memcpy(response, (char *)&rv, sizeof(int));//copy return val
    if (rv < 0) {
        int errcode = errno;
        fprintf(stderr, "do_unlink failed, err code:%d\n", errno);
        memcpy(response + sizeof(int), (char *)&errcode, sizeof(errno));//copy errno
    }
    send_back(sessfd, response, length);
    free(msg);
    free(response);
    return rv;
}

/**
 * do get direntries
 * @param msg
 * @param sessfd
 * @return
 */
ssize_t do_getdirentries(Message *msg, int sessfd) {
    fprintf(stderr, "do_getdirentries!\n");
    int fd = *(int *)&msg->params;
    size_t nbytes = *(size_t *)(&msg->params + sizeof(int));
    off_t base = *(off_t *)(&msg->params + sizeof(int) + sizeof(size_t));

    char *buf = (char *)malloc(nbytes);
    ssize_t rv = getdirentries(fd, buf, nbytes, &base);

    int length = sizeof(ssize_t) + sizeof(off_t) + nbytes;
    char *response = (char *)malloc(length);
    memcpy(response, (char *)&rv, sizeof(ssize_t));//copy return val

    if(rv > 0) {
        memcpy(response + sizeof(ssize_t), (char *)&base, sizeof(off_t));//copy base
        memcpy(response + sizeof(ssize_t) + sizeof(off_t), buf, nbytes);//copy content of entries
    } else {
        memcpy(response + sizeof(ssize_t), (char *)&errno, sizeof(errno));//copy errno
    }
    send_back(sessfd, response, length);
    free(buf);
    free(msg);
    free(response);
    return rv;

}
/**
 * send back the return value and other responses of RPC
 * @param sessfd socket to the client
 * @param response response msg want to send
 * @param len length of the mssage
 * @return 1 on success, -1 on failure
 */
int send_back(int sessfd, char *response, int len) {
    char *send_msg = (char *)malloc(len + sizeof(int));
    int length = len + sizeof(int);
    memcpy(send_msg, (char *)&length, sizeof(int));
    memcpy(send_msg + sizeof(int), response, len);
    int rv = send(sessfd, send_msg, length, 0);
    free(send_msg);
    if (rv < 0) {
        err(1, 0);
        return -1;
    } else {
        return 1;
    }
}

/**
 * child process handling the RPC
 * receive the process and read the operation
 * then pass the read parameters to corresponding functions
 * @param sessfd
 * @return
 */
int do_stuff(int sessfd) {
    char* operation;
    char* receive;
    char* buf = (char *)malloc(MAXMSGLEN);
    int rv = 0;

    //keep hearing from one client
    while (true) {
        int anticipated = 0; //bytes anticipated to receive
        int received_bytes = 0; //already received bytes
        int read = 0; //have read the first four bytes or not
        //receive from the client until all the bytes received
        do {
            rv = recv(sessfd, buf, MAXMSGLEN, 0);
            received_bytes += rv;

            if (received_bytes < 4 && rv > 0) { //keep receiving until read at leat 4 bytes
                buf = (char *)buf + rv;
                continue;
            } else if (rv <= 0) {
                break; //client break the connection or error
            }else if (!read) {
                read = 1;
                anticipated = *(int *) buf; //read anticipated bytes
                fprintf(stderr, "the server anticipate to receive %d bytes\n", anticipated);
                receive = (char *)malloc(anticipated + 1);//malloc the memory to receive the request
            }
            memcpy(receive + received_bytes - rv, buf, rv);
            memset(buf, '\0', MAXMSGLEN);

        } while (received_bytes < anticipated && rv > 0);//quit receiving once received enough bytes

        if (received_bytes == 0) {
            fprintf(stderr, "the server find the client close the socket\n");
            break; //the client close the socket
        }
        //handling the operation
        receive[anticipated] = '\0';
        fprintf(stderr, "total received bytes %d\n", received_bytes);
        //ignore the first four bytes and free the original buffer
        Message *packet = (Message *) malloc(received_bytes - 4);
        memcpy(packet, (Message *)(receive + 4), received_bytes - 4);
        free(receive);

        operation = packet->operation;
        fprintf(stderr, "the operation is %s\n", operation);

        //send the packet to corresponding function
        if (!strcmp("open", operation)) {//each of these function are responsible to free the packet
            do_open(packet, sessfd);
        } else if (!strcmp("write", operation)) {
            do_write(packet, sessfd);
        } else if (!strcmp("close", operation)) {
            do_close(packet, sessfd);
        } else if (!strcmp("read", operation)) {
            //we need to create a buffer for the read
            do_read(packet, sessfd);
        } else if (!strcmp("lseek", operation)) {
            do_lseek(packet, sessfd);
        } else if (!strcmp("stat", operation)) {
            do_stat(packet, sessfd);
        } else if (!strcmp("unlink", operation)) {
            do_unlink(packet, sessfd);
        } else if (!strcmp("getdirentries", operation)) {
            do_getdirentries(packet, sessfd);
        } else if (!strcmp("getdirtree", operation)) {
            do_getdirtree(packet, sessfd);
        }
    }//big child process while loop

    fprintf(stderr, "outside the child process main loop!\n");
    // either client closed connection, or error
    if (rv<0)  {err(1,0); return -1;}
    free(buf);
    close(sessfd);
    return 1;
}


int main(int argc, char**argv) {
    char *serverport;
    unsigned short port;
    int sockfd, sessfd, rv;
    struct sockaddr_in srv, cli;
    socklen_t sa_size;

    // Get environment variable indicating the port of the server
    serverport = getenv("serverport15440");//should not hard code
    if (serverport) {
        port = (unsigned short)atoi(serverport);
        fprintf(stderr, "get the port number %s", serverport);
    }
    else {
        fprintf(stderr, "not get the port num");
        port=10707;
    }

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);	// TCP/IP socket
    if (sockfd<0) err(1, 0);			// in case of error

    // setup address structure to indicate server port
    memset(&srv, 0, sizeof(srv));			// clear it first
    srv.sin_family = AF_INET;			// IP family
    srv.sin_addr.s_addr = htonl(INADDR_ANY);	// don't care IP address
    srv.sin_port = htons(port);			// server port

    // bind to our port
    rv = bind(sockfd, (struct sockaddr*)&srv, sizeof(struct sockaddr));
    if (rv<0) err(1,0);

    // start listening for connections
    rv = listen(sockfd, 5);
    if (rv<0) err(1,0); //errors in connection

    while(true) {
        // wait for next client, get session socket
        sa_size = sizeof(struct sockaddr_in);
        sessfd = accept(sockfd, (struct sockaddr *)&cli, &sa_size);
        if (sessfd<0) err(1,0);
        int pid = fork();
        if (pid == 0) {   //child process
            do_stuff(sessfd);
            close(sockfd);
            exit(0); //child process exit
        }
        fprintf(stderr, "fork child process %d\n", pid);
        close(sessfd);
    }
    // close socket
    close(sockfd);
    return 0;
}
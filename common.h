#ifndef COMMON_H
#define COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <sys/inotify.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <sys/select.h>

#define SERVER_PORT 5000
#define BACKLOG 10
#define BUF_SIZE 8192
#define SEND_CHUNK 65536
#define LIST_INTERVAL_SEC 2   
#define MAX_CLIENTS 256
#define CMD_BUF 4096

extern const char *watch_dir; 
extern volatile int running;
extern int inotify_fd;

/* Client structure */
typedef struct {
    int sock;
    struct sockaddr_in addr;
    char ip[INET_ADDRSTRLEN];
    pthread_t thread;
    pthread_mutex_t send_lock;
    int active;
} client_t;

/* Globals from main */
extern client_t *clients[MAX_CLIENTS];
extern pthread_mutex_t clients_lock;

#endif

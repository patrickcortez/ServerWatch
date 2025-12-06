#ifndef NET_H
#define NET_H

#include "common.h"

int setup_server_socket(void);
ssize_t send_all(int sock, const void *buf, size_t len);
int client_send_locked(client_t *c, const void *buf, size_t len);
ssize_t recv_line(int sock, char *buf, size_t maxlen);
int recv_to_fd(int sock, int fd_out, long long count);
void broadcast_file_list(const char *list_str);
void remove_client(client_t *c);

#endif

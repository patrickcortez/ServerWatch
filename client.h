#ifndef CLIENT_H
#define CLIENT_H

#include "common.h"

void *client_thread_fn(void *arg);
int send_file_to_client(client_t *c, const char *filename);
int handle_upload(client_t *c, const char *header);

#endif

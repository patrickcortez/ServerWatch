#include "net.h"

ssize_t send_all(int sock, const void *buf, size_t len) {
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t n = send(sock, p, remaining, 0);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        remaining -= n;
        p += n;
    }
    return (ssize_t)len;
}

int client_send_locked(client_t *c, const void *buf, size_t len) {
    pthread_mutex_lock(&c->send_lock);
    ssize_t res = send_all(c->sock, buf, len);
    pthread_mutex_unlock(&c->send_lock);
    return (res == (ssize_t)len) ? 0 : -1;
}

ssize_t recv_line(int sock, char *buf, size_t maxlen) {
    size_t idx = 0;
    while (idx + 1 < maxlen) {
        char ch;
        ssize_t n = recv(sock, &ch, 1, 0);
        if (n == 1) {
            buf[idx++] = ch;
            if (ch == '\n') break;
        } else if (n == 0) {
            return 0;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            return -1;
        }
    }
    buf[idx] = '\0';
    return (ssize_t)idx;
}

int recv_to_fd(int sock, int fd_out, long long count) {
    char buf[BUF_SIZE];
    long long remaining = count;
    while (remaining > 0) {
        ssize_t toread = (remaining > (long long)sizeof(buf)) ? (ssize_t)sizeof(buf) : (ssize_t)remaining;
        ssize_t r = recv(sock, buf, toread, 0);
        if (r > 0) {
            ssize_t w = write(fd_out, buf, (size_t)r);
            if (w != r) {
                perror("write");
                return -1;
            }
            remaining -= r;
        } else if (r == 0) {
            fprintf(stderr, "Client closed connection during upload\n");
            return -1;
        } else {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            perror("recv");
            return -1;
        }
    }
    return 0;
}

void remove_client(client_t *c) {
    if (!c) return;
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i] == c) {
            clients[i] = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&clients_lock);
    close(c->sock);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
}

void broadcast_file_list(const char *list_str) {
    if (!list_str) return;
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        client_t *c = clients[i];
        if (c && c->active) {
            size_t total = 5 + strlen(list_str) + 4; 
            char *buf = malloc(total + 1);
            if (!buf) continue;
            strcpy(buf, "LIST\n");
            strcat(buf, list_str);
            strcat(buf, "END\n");

            if (client_send_locked(c, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Failed to send list to %s — marking inactive and closing socket\n", c->ip);
                c->active = 0;
                close(c->sock);
            }
            free(buf);
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

int setup_server_socket(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in srv;
    srv.sin_family = AF_INET;
    srv.sin_addr.s_addr = INADDR_ANY;
    srv.sin_port = htons(SERVER_PORT);

    if (bind(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    if (listen(sock, BACKLOG) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }
    return sock;
}

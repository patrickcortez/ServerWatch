#include "common.h"
#include "net.h"
#include "client.h"
#include "file_ops.h"
#include "metadata.h"

/* Global definitions */
const char *watch_dir = NULL;
volatile int running = 1;
int listen_fd = -1;
int inotify_fd = -1;
client_t *clients[MAX_CLIENTS];
pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;

void int_handler(int signo) {
    (void)signo;
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
    if (inotify_fd >= 0) close(inotify_fd);
}

void *accept_loop(void *arg) {
    (void)arg;
    while (running) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);
        int client_sock = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_sock < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }

        client_t *c = calloc(1, sizeof(client_t));
        if (!c) { close(client_sock); continue; }
        c->sock = client_sock;
        c->addr = cli_addr;
        inet_ntop(AF_INET, &cli_addr.sin_addr, c->ip, sizeof(c->ip));
        c->active = 1;
        pthread_mutex_init(&c->send_lock, NULL);

        pthread_mutex_lock(&clients_lock);
        int added = 0;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (!clients[i]) {
                clients[i] = c;
                added = 1;
                break;
            }
        }
        pthread_mutex_unlock(&clients_lock);
        if (!added) {
            fprintf(stderr, "Max clients reached, rejecting %s\n", c->ip);
            close(client_sock);
            pthread_mutex_destroy(&c->send_lock);
            free(c);
            continue;
        }

        if (pthread_create(&c->thread, NULL, client_thread_fn, c) != 0) {
            perror("pthread_create");
            remove_client(c);
            continue;
        }
        pthread_detach(c->thread);
    }
    return NULL;
}

void *inotify_thread_fn(void *arg) {
    (void)arg;
    char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
    const struct inotify_event *event;
    ssize_t len;
    
    while (running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(inotify_fd, &fds);
        struct timeval tv = {1, 0};
        
        int ret = select(inotify_fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (ret == 0) continue; 

        len = read(inotify_fd, buf, sizeof(buf));
        if (len == -1) {
            if (errno == EAGAIN) continue;
            perror("inotify read");
            break;
        }
        
        int valid_change = 0;
        char *ptr;
        for (ptr = buf; ptr < buf + len; ptr += sizeof(struct inotify_event) + event->len) {
            event = (const struct inotify_event *) ptr;
            if (event->len > 0) {
                if (event->name[0] == '.') continue;
                if (event->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE)) {
                    valid_change = 1;
                }
            }
        }
        
        if (valid_change) {
            char *list = build_file_list();
            if (list) {
                broadcast_file_list(list);
                free(list);
            }
        }
    }
    return NULL;
}

void *watcher_thread_fn(void *arg) {
    (void)arg;
    while (running) {
        struct timespec req = { 60, 0 }, rem;
        while (nanosleep(&req, &rem) == -1) {
            if (errno == EINTR) {
                if (!running) break;
                req = rem;
                continue;
            } else {
                break;
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <path_to_folder>\n", argv[0]);
        return 1;
    }
    watch_dir = argv[1];

    struct stat st;
    if (stat(watch_dir, &st) < 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: watch directory '%s' does not exist or is not a directory.\n", watch_dir);
        return 1;
    }

    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    listen_fd = setup_server_socket();
    if (listen_fd < 0) return 1;
    fprintf(stdout, "Server listening on port %d\n", SERVER_PORT);

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        perror("inotify_init1");
    } else {
        int wd = inotify_add_watch(inotify_fd, watch_dir,
            IN_CREATE | IN_DELETE | IN_MODIFY | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF);
        if (wd < 0) {
            perror("inotify_add_watch");
            close(inotify_fd);
            inotify_fd = -1;
        } else {
            fprintf(stdout, "Watching dir %s for changes\n", watch_dir);
        }
    }

    load_metadata_cache();

    pthread_t accept_thread, watcher_thread, inotify_thread;
    if (pthread_create(&accept_thread, NULL, accept_loop, NULL) != 0) {
        perror("pthread_create accept");
        close(listen_fd);
        return 1;
    }
    if (pthread_create(&watcher_thread, NULL, watcher_thread_fn, NULL) != 0) {
        perror("pthread_create watcher");
        running = 0;
        close(listen_fd);
        return 1;
    }
    if (inotify_fd >= 0 && pthread_create(&inotify_thread, NULL, inotify_thread_fn, NULL) != 0) {
        perror("pthread_create inotify");
    }

    pthread_join(accept_thread, NULL);
    pthread_join(watcher_thread, NULL); 
    if (inotify_fd >= 0) pthread_join(inotify_thread, NULL);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i]) {
            close(clients[i]->sock);
            clients[i]->active = 0;
        }
    }
    pthread_mutex_unlock(&clients_lock);

    if (inotify_fd >= 0) close(inotify_fd);
    if (listen_fd >= 0) close(listen_fd);

    fprintf(stdout, "Server shutting down.\n");
    return 0;
}

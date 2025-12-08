// Compile: gcc -o file_server file_share.c -pthread -lssl -lcrypto
// Run: ./file_server <dir> <password>


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
#include <openssl/ssl.h>
#include <openssl/err.h>

#define SERVER_PORT 5000
#define BACKLOG 10
#define BUF_SIZE 8192
#define SEND_CHUNK 65536
#define LIST_INTERVAL_SEC 2   
#define MAX_CLIENTS 256
#define CMD_BUF 4096

const char *watch_dir = NULL; 
const char *server_password = NULL;
SSL_CTX *ctx = NULL;

typedef struct {
    int sock;
    SSL *ssl;
    struct sockaddr_in addr;
    char ip[INET_ADDRSTRLEN];
    pthread_t thread;
    pthread_mutex_t send_lock;
    int active;
    int authenticated;
    char username[64];
} client_t;

static client_t *clients[MAX_CLIENTS];
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int running = 1;
static int listen_fd = -1;
static int inotify_fd = -1;

#define MAX_METADATA_ENTRIES 1024
typedef struct {
    char filename[256];
    char username[64];
    time_t timestamp;
} metadata_t;

ssize_t send_all(client_t *c, const void *buf, size_t len) {
    const char *p = buf;
    size_t remaining = len;
    while (remaining > 0) {
        int n = SSL_write(c->ssl, p, remaining);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return -1;
        }
        remaining -= n;
        p += n;
    }
    return (ssize_t)len;
}

/* Safe send for a client (locks per-client send_lock) */
int client_send_locked(client_t *c, const void *buf, size_t len) {
    pthread_mutex_lock(&c->send_lock);
    ssize_t res = send_all(c, buf, len);
    pthread_mutex_unlock(&c->send_lock);
    return (res == (ssize_t)len) ? 0 : -1;
}

/* Save metadata for a file (append to .file_metadata) */
void save_metadata(const char *filename, const char *username) {
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/.file_metadata", watch_dir);
    FILE *f = fopen(meta_path, "a");
    if (f) {
        // Format: filename|username|timestamp
        fprintf(f, "%s|%s|%ld\n", filename, username, (long)time(NULL));
        fclose(f);
    }
}

/* Load all metadata into an array. Returns count. Caller must free invalid entries? No, just static array for now. */
int load_metadata(metadata_t *entries, int max_entries) {
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/.file_metadata", watch_dir);
    FILE *f = fopen(meta_path, "r");
    if (!f) return 0;
    
    char line[512];
    int count = 0;
    while (fgets(line, sizeof(line), f) && count < max_entries) {
        // Parse: filename|username|timestamp
        char *p = line;
        char *pipe1 = strchr(p, '|');
        if (!pipe1) continue;
        *pipe1 = '\0';
        
        char *pipe2 = strchr(pipe1 + 1, '|');
        if (!pipe2) continue;
        *pipe2 = '\0';
        
        // Copy to specialized struct
        strncpy(entries[count].filename, p, sizeof(entries[count].filename)-1);
        strncpy(entries[count].username, pipe1+1, sizeof(entries[count].username)-1);
        entries[count].timestamp = atol(pipe2 + 1);
        
        count++;
    }
    fclose(f);
    // Note: We might have duplicate entries for same filename. 
    // We should compact them or just search backwards later.
    return count;
}

/* Find metadata for filename. Returns 1 if found, 0 otherwise. */
int find_metadata(const char *fname, const metadata_t *entries, int count, char *out_user, time_t *out_time) {
    // Search backwards to find latest
    for (int i = count - 1; i >= 0; --i) {
        if (strcmp(entries[i].filename, fname) == 0) {
            strcpy(out_user, entries[i].username);
            *out_time = entries[i].timestamp;
            return 1;
        }
    }
    return 0;
}

/* Read the directory and build newline-separated file list in dynamically allocated string */
char *build_file_list(void) {
    DIR *d = opendir(watch_dir);
    if (!d) {
        perror("opendir");
        return NULL;
    }
    struct dirent *entry;
    size_t cap = 8192;
    size_t len = 0;
    char *out = malloc(cap);
    if (!out) { closedir(d); return NULL; }
    out[0] = '\0';

    // Load metadata once
    metadata_t *meta = calloc(MAX_METADATA_ENTRIES, sizeof(metadata_t));
    int meta_count = load_metadata(meta, MAX_METADATA_ENTRIES);

    while ((entry = readdir(d)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (entry->d_name[0] == '.') continue; // hide hidden files (like .file_metadata)

        // Build full path and check it's a regular file
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", watch_dir, entry->d_name);

        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && strchr(entry->d_name, '|') == NULL) {
            size_t name_len = strlen(entry->d_name);
            // ensure capacity
            if (len + name_len + 2 >= cap) {
                cap *= 2;
                char *tmp = realloc(out, cap);
                if (!tmp) { free(meta); free(out); closedir(d); return NULL; }
                out = tmp;
            }
            strcat(out, entry->d_name);
            
            // Append metadata: |username|timestamp
            char user[64] = "Unknown";
            time_t ts = st.st_mtime; // fallback to file mtime
            char fetched_user[64];
            time_t fetched_ts;
            if (find_metadata(entry->d_name, meta, meta_count, fetched_user, &fetched_ts)) {
                strcpy(user, fetched_user);
                ts = fetched_ts;
            }
            
            char meta_str[128];
            snprintf(meta_str, sizeof(meta_str), "|%s|%ld", user, (long)ts);
            // Ensure capacity again? simplistic check above might not cover this extension
            // For safety let's re-check cap or just hope 8192 is big enough (it reallocs above based on name len only)
            // safer to resize slightly liberally
             if (len + strlen(meta_str) + 100 >= cap) { // check again
                 cap *= 2;
                 out = realloc(out, cap);
             }
             strcat(out, meta_str);

            strcat(out, "\n");
            len = strlen(out);
        }
    }

    closedir(d);
    return out; // caller must free
}

/* Broadcast file list to all clients */
void broadcast_file_list(const char *list_str) {
    if (!list_str) return;
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        client_t *c = clients[i];
        if (c && c->active) {
            // Protocol: send "LIST\n<content>END\n"
            size_t total = 5 + strlen(list_str) + 4; // "LIST\n" + content + "END\n"
            char *buf = malloc(total + 1);
            if (!buf) continue;
            strcpy(buf, "LIST\n");
            strcat(buf, list_str);
            strcat(buf, "END\n");

            if (client_send_locked(c, buf, strlen(buf)) < 0) {
                fprintf(stderr, "Failed to send list to %s — marking inactive and closing socket\n", c->ip);
                // Mark inactive & close socket; client thread will detect disconnect and run remove_client()
                c->active = 0;
                close(c->sock);
            }
            free(buf);
        }
    }
    pthread_mutex_unlock(&clients_lock);
}

/* Send a file to a specific client (thread-safe wrt sending) */
int send_file_to_client(client_t *c, const char *filename) {
    if (!c || !filename) return -1;

    // Lock the client for the entire duration of the transfer to prevent interleaving
    pthread_mutex_lock(&c->send_lock);

    char fullpath[PATH_MAX];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", watch_dir, filename);

    struct stat st;
    if (stat(fullpath, &st) < 0) {
        // send error
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ERROR:File not found\n");
        send_all(c, errbuf, strlen(errbuf));
        pthread_mutex_unlock(&c->send_lock);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ERROR:Not a regular file\n");
        send_all(c, errbuf, strlen(errbuf));
        pthread_mutex_unlock(&c->send_lock);
        return -1;
    }

    off_t filesize = st.st_size;
    // Send header: FILE:<filename>:<size>\n
    char header[1024];
    snprintf(header, sizeof(header), "FILE:%s:%lld\n", filename, (long long)filesize);
    if (send_all(c, header, strlen(header)) < 0) {
        pthread_mutex_unlock(&c->send_lock);
        return -1;
    }

    // Open and send file in chunks
    int fd = open(fullpath, O_RDONLY);
    if (fd < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ERROR:Open failed\n");
        send_all(c, errbuf, strlen(errbuf));
        pthread_mutex_unlock(&c->send_lock);
        return -1;
    }

    char *buf = malloc(SEND_CHUNK);
    if (!buf) { 
        close(fd); 
        pthread_mutex_unlock(&c->send_lock);
        return -1; 
    }

    ssize_t r;
    // off_t sent = 0;
    while ((r = read(fd, buf, SEND_CHUNK)) > 0) {
        if (send_all(c, buf, (size_t)r) < 0) {
            fprintf(stderr, "Send failed while sending file to %s\n", c->ip);
            free(buf);
            close(fd);
            pthread_mutex_unlock(&c->send_lock);
            return -1;
        }
        // sent += r;
    }
    free(buf);
    close(fd);

    // Optionally send a DONE marker
    char done[] = "\nDONE\n";
    send_all(c, done, strlen(done));
    
    pthread_mutex_unlock(&c->send_lock);
    
    fprintf(stdout, "Sent file '%s' (%lld bytes) to %s\n", filename, (long long)filesize, c->ip);
    return 0;
}

/* Remove client and free */
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
    if (c->ssl) {
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
    }
    close(c->sock);
    pthread_mutex_destroy(&c->send_lock);
    free(c);
}

/* Read a line terminated by '\n' from socket into buf (null-terminated), return length or -1 on error/closed */
ssize_t recv_line(client_t *c, char *buf, size_t maxlen) {
    size_t idx = 0;
    while (idx + 1 < maxlen) {
        char ch;
        int n = SSL_read(c->ssl, &ch, 1);
        if (n == 1) {
            buf[idx++] = ch;
            if (ch == '\n') break;
        } else if (n == 0) {
            // closed
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

/* Receive exactly `count` bytes from socket and write to `fd_out`. Returns 0 on success, -1 on error */
int recv_to_fd(client_t *c, int fd_out, long long count) {
    char buf[BUF_SIZE];
    long long remaining = count;
    while (remaining > 0) {
        ssize_t toread = (remaining > (long long)sizeof(buf)) ? (ssize_t)sizeof(buf) : (ssize_t)remaining;
        int r = SSL_read(c->ssl, buf, toread);
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

/* Helper: sanitize filename (reject path separators or "..") */
int sanitize_filename(const char *fname) {
    if (!fname || fname[0] == '\0') return -1;
    if (strstr(fname, "/") != NULL) return -1;
    if (strstr(fname, "..") != NULL) return -1;
    if (strchr(fname, '|') != NULL) return -1;
    // you can add more checks here (allowed characters, length, etc.)
    return 0;
}

/* Handle an upload from a client. header format: upload:<filename>:<size> */
int handle_upload(client_t *c, const char *header) {
    // Parse header
    // header assumed trimmed and without trailing newline
    // Parse header
    // header assumed trimmed and without trailing newline
    // Format: upload:<filename>:<size>:<username>
    const char *p = header + 7; // skip "upload:"
    if (*p == '\0') {
        char err[] = "ERROR:No filename/size provided\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    // find next colon
    const char *colon = strchr(p, ':');
    if (!colon) {
        char err[] = "ERROR:Bad upload header (expect upload:<filename>:<size>)\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    size_t fname_len = (size_t)(colon - p);
    if (fname_len == 0 || fname_len >= PATH_MAX) {
        char err[] = "ERROR:Bad filename\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    char filename[PATH_MAX];
    memcpy(filename, p, fname_len);
    filename[fname_len] = '\0';

    if (sanitize_filename(filename) != 0) {
        char err[] = "ERROR:Invalid filename\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    const char *size_str = colon + 1;
    char *next_colon = strchr(size_str, ':'); // look for username separator
    
    // Validate filename length just to be sure (already capped by PATH_MAX check/copy but let's be strict)
    if (strlen(filename) > 255) { // Common fs limit
        char err[] = "ERROR:Filename too long\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }
    
    // If next_colon exists, size ends there. Else check null.
    // backward compatibility: if no username provided
    
    char username[64] = "Unknown";
    size_t size_len = next_colon ? (size_t)(next_colon - size_str) : strlen(size_str);
    char size_buf[32];
    if (size_len >= sizeof(size_buf)) return -1;
    memcpy(size_buf, size_str, size_len);
    size_buf[size_len] = '\0';
    
    if (next_colon) {
        strncpy(username, next_colon + 1, sizeof(username)-1);
    }
    
    if (size_buf[0] == '\0') {
        char err[] = "ERROR:Missing size\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    char *endptr = NULL;
    long long filesize = strtoll(size_buf, &endptr, 10);
    if (endptr == size_buf || filesize < 0) {
        char err[] = "ERROR:Bad size\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    // Build temp path and final path
    char finalpath[PATH_MAX];
    if (snprintf(finalpath, sizeof(finalpath), "%s/%s", watch_dir, filename) >= (int)sizeof(finalpath)) {
        char err[] = "ERROR:Filename too long\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    // create unique temp file inside watch_dir
    char tmppath[PATH_MAX];
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pid_t pid = getpid();
    if (snprintf(tmppath, sizeof(tmppath), "%s/.upload_tmp_%ld_%d.tmp", watch_dir, (long)ts.tv_sec, (int)pid) >= (int)sizeof(tmppath)) {
        char err[] = "ERROR:Temp path too long\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    int outfd = open(tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (outfd < 0) {
        perror("open tmp");
        char err[] = "ERROR:Could not create temp file\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    fprintf(stdout, "Receiving upload '%s' (%lld bytes) from %s -> temp %s\n", filename, filesize, c->ip, tmppath);

    // Receive exactly filesize bytes from socket into outfd
    if (filesize > 0) {
        if (recv_to_fd(c, outfd, filesize) != 0) {
            close(outfd);
            unlink(tmppath);
            char err[] = "ERROR:Receive failed\n";
            client_send_locked(c, err, strlen(err));
            return -1;
        }
    }

    close(outfd);

    // Atomically rename temp to final
    if (rename(tmppath, finalpath) != 0) {
        perror("rename");
        unlink(tmppath);
        char err[] = "ERROR:Could not finalize upload\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    // success
    char ok[] = "UPLOAD_OK\n";
    client_send_locked(c, ok, strlen(ok));
    
    save_metadata(finalpath + strlen(watch_dir) + 1, username); // extract basename

    fprintf(stdout, "Upload saved: %s (%lld bytes) user=%s\n", finalpath, filesize, username);

    // Broadcast file list immediately so clients get update quicker
    char *list = build_file_list();
    if (list) {
        broadcast_file_list(list);
        free(list);
    }

    return 0;
}

/* Client handler thread */
void *client_thread_fn(void *arg) {
    client_t *c = (client_t *)arg;
    fprintf(stdout, "Client connected: %s\n", c->ip);

    char line[CMD_BUF];
    if (SSL_accept(c->ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        goto cleanup;
    }

    // Auth handshake
    // 1. Client must send AUTH:<password>
    // 2. Server replies AUTH_OK or AUTH_FAIL
    
    // Read auth line
    if (recv_line(c, line, sizeof(line)) > 0) {
        // trim
        size_t len = strlen(line);
        while(len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) line[--len] = '\0';
        
        if (strncmp(line, "AUTH:", 5) == 0) {
            char *pass = line + 5;
            if (strcmp(pass, server_password) == 0) {
                c->authenticated = 1;
                client_send_locked(c, "AUTH_OK\n", 8);
                fprintf(stdout, "Client authenticated: %s\n", c->ip);
            } else {
                client_send_locked(c, "AUTH_FAIL\n", 10);
                fprintf(stdout, "Client sent wrong password: %s\n", c->ip);
            }
        }
    }

    if (!c->authenticated) {
        // give brief moment to receive FAIL
        usleep(100000);
        goto cleanup;
    }

    while (running && c->active) {
        ssize_t n = recv_line(c, line, sizeof(line));
        if (n > 0) {
            // line contains '\n' terminated string
            // trim whitespace/newline
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) { line[n-1] = '\0'; n--; }

            if (n == 0) continue;

            fprintf(stdout, "Cmd from %s: '%s'\n", c->ip, line);

            // expected: download:<filename>
            if (strncmp(line, "download:", 9) == 0) {
                const char *fname = line + 9;
                if (strlen(fname) == 0) {
                    char err[] = "ERROR:No filename provided\n";
                    client_send_locked(c, err, strlen(err));
                } else {
                    // send file (this will lock sending for this client)
                    send_file_to_client(c, fname);
                }
            } else if (strncmp(line, "upload:", 7) == 0) {
                // upload handling: upload:<filename>:<size>
                if (handle_upload(c, line) != 0) {
                    fprintf(stderr, "Upload failed for client %s\n", c->ip);
                    // handle_upload already sent an error to client
                } else {
                    // success already reported and broadcasted
                }
            } else if (strcmp(line, "LIST?") == 0) {
                // Respond with current file list immediately
                char *list = build_file_list();
                if (list) {
                    char *buf = malloc(strlen(list) + 64);
                    if (buf) {
                        sprintf(buf, "LIST\n%sEND\n", list);
                        client_send_locked(c, buf, strlen(buf));
                        free(buf);
                    }
                    free(list);
                } else {
                    char err[] = "ERROR:Could not build file list\n";
                    client_send_locked(c, err, strlen(err));
                }
            } else {
                // unknown command
                char err[] = "ERROR:Unknown command\n";
                client_send_locked(c, err, strlen(err));
            }
        } else if (n == 0) {
            fprintf(stdout, "Client %s disconnected.\n", c->ip);
            break;
        } else {
            perror("recv_line");
            break;
        }
    }

    c->active = 0;
cleanup:
    remove_client(c);
    return NULL;
}

/* Accept loop: accepts new clients and spawns threads */
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

        // create client_t
        client_t *c = calloc(1, sizeof(client_t));
        if (!c) { close(client_sock); continue; }
        c->sock = client_sock;
        c->addr = cli_addr;
        inet_ntop(AF_INET, &cli_addr.sin_addr, c->ip, sizeof(c->ip));
        c->active = 1;
        c->authenticated = 0;
        
        c->ssl = SSL_new(ctx);
        SSL_set_fd(c->ssl, c->sock);
        
        pthread_mutex_init(&c->send_lock, NULL);

        // add to clients array
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

        // start client thread
        if (pthread_create(&c->thread, NULL, client_thread_fn, c) != 0) {
            perror("pthread_create");
            remove_client(c);
            continue;
        }
        pthread_detach(c->thread);
    }
    return NULL;
}

/* Periodic broadcaster thread: sends list on inotify events */
void *watcher_thread_fn(void *arg) {
    (void)arg;
    
    // Initial broadcast
    char *list = build_file_list();
    if (list) {
        broadcast_file_list(list);
        free(list);
    }
    
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(inotify_fd, &rfds);
        
        // Wait for event or timeout (heartbeat)
        struct timeval tv;
        tv.tv_sec = LIST_INTERVAL_SEC; 
        tv.tv_usec = 0;
        
        int retval = select(inotify_fd + 1, &rfds, NULL, NULL, &tv);
        if (retval == -1) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        } else if (retval > 0) {
            // Event occurred
            if (FD_ISSET(inotify_fd, &rfds)) {
                // Drain the inotify buffer
                char buf[4096] __attribute__ ((aligned(__alignof__(struct inotify_event))));
                ssize_t len;
                while ((len = read(inotify_fd, buf, sizeof(buf))) > 0) {
                    // Just drain; we don't strictly parse events because we rebuild the whole list anyway.
                    // A real optimization would process specific events, but for < 1000 files, rebuild is fast.
                }
                if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("read inotify");
                }
                
                // Debounce: Wait a small bit to coalesce rapid events (e.g. copy or delete many files)
                usleep(200000); 
                
                // Flush any further pending events during debounce
                while (read(inotify_fd, buf, sizeof(buf)) > 0);
            }
        }
        
        // Update and broadcast
        list = build_file_list();
        if (list) {
            broadcast_file_list(list);
            free(list);
        }
    }
    return NULL;
}

/* Initialize openssl */
void init_openssl() {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();
    const SSL_METHOD *method = TLS_server_method();
    ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(ctx, "server.crt", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "server.key", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

/* Setup listening socket */
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

void int_handler(int signo) {
    (void)signo;
    running = 0;
    if (listen_fd >= 0) close(listen_fd);
    if (inotify_fd >= 0) close(inotify_fd);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <path_to_folder> <password>\n", argv[0]);
        return 1;
    }
    watch_dir = argv[1];
    server_password = argv[2];

    // Verify watch_dir exists
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

    init_openssl();

    inotify_fd = inotify_init1(IN_NONBLOCK);
    if (inotify_fd < 0) {
        perror("inotify_init1");
        // Not fatal; we'll still do periodic polling
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

    // Start accept thread
    pthread_t accept_thread, watcher_thread;
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

    pthread_join(accept_thread, NULL);
    pthread_join(watcher_thread, NULL);

    // Cleanup clients
    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (clients[i]) {
            close(clients[i]->sock);
            clients[i]->active = 0;
            // don't free here because client threads may be cleaning up
        }
    }
    pthread_mutex_unlock(&clients_lock);

    if (inotify_fd >= 0) close(inotify_fd);
    if (listen_fd >= 0) close(listen_fd);
    if (ctx) SSL_CTX_free(ctx);
    EVP_cleanup();

    fprintf(stdout, "Server shutting down.\n");
    return 0;
}

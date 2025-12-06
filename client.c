#include "client.h"
#include "net.h"
#include "file_ops.h"
#include "metadata.h"

int send_file_to_client(client_t *c, const char *filename) {
    if (!c || !filename) return -1;

    char fullpath[PATH_MAX];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", watch_dir, filename);

    struct stat st;
    if (stat(fullpath, &st) < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ERROR:File not found\n");
        client_send_locked(c, errbuf, strlen(errbuf));
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ERROR:Not a regular file\n");
        client_send_locked(c, errbuf, strlen(errbuf));
        return -1;
    }

    off_t filesize = st.st_size;
    char header[1024];
    snprintf(header, sizeof(header), "FILE:%s:%lld\n", filename, (long long)filesize);
    if (client_send_locked(c, header, strlen(header)) < 0) {
        return -1;
    }

    int fd = open(fullpath, O_RDONLY);
    if (fd < 0) {
        char errbuf[512];
        snprintf(errbuf, sizeof(errbuf), "ERROR:Open failed\n");
        client_send_locked(c, errbuf, strlen(errbuf));
        return -1;
    }

    char *buf = malloc(SEND_CHUNK);
    if (!buf) { close(fd); return -1; }

    ssize_t r;
    while ((r = read(fd, buf, SEND_CHUNK)) > 0) {
        if (client_send_locked(c, buf, (size_t)r) < 0) {
            fprintf(stderr, "Send failed while sending file to %s\n", c->ip);
            free(buf);
            close(fd);
            return -1;
        }
    }
    free(buf);
    close(fd);

    char done[] = "\nDONE\n";
    client_send_locked(c, done, strlen(done));
    fprintf(stdout, "Sent file '%s' (%lld bytes) to %s\n", filename, (long long)filesize, c->ip);
    return 0;
}

int handle_upload(client_t *c, const char *header) {
    const char *p = header + 7; 
    if (*p == '\0') {
        char err[] = "ERROR:No filename/size provided\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

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
    char *next_colon = strchr(size_str, ':'); 
    
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

    char finalpath[PATH_MAX];
    if (snprintf(finalpath, sizeof(finalpath), "%s/%s", watch_dir, filename) >= (int)sizeof(finalpath)) {
        char err[] = "ERROR:Filename too long\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

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

    if (filesize > 0) {
        if (recv_to_fd(c->sock, outfd, filesize) != 0) {
            close(outfd);
            unlink(tmppath);
            char err[] = "ERROR:Receive failed\n";
            client_send_locked(c, err, strlen(err));
            return -1;
        }
    }

    close(outfd);

    if (rename(tmppath, finalpath) != 0) {
        perror("rename");
        unlink(tmppath);
        char err[] = "ERROR:Could not finalize upload\n";
        client_send_locked(c, err, strlen(err));
        return -1;
    }

    char ok[] = "UPLOAD_OK\n";
    client_send_locked(c, ok, strlen(ok));
    
    save_metadata(finalpath + strlen(watch_dir) + 1, username); 

    fprintf(stdout, "Upload saved: %s (%lld bytes) user=%s\n", finalpath, filesize, username);

    char *list = build_file_list();
    if (list) {
        broadcast_file_list(list);
        free(list);
    }

    return 0;
}

void *client_thread_fn(void *arg) {
    client_t *c = (client_t *)arg;
    fprintf(stdout, "Client connected: %s\n", c->ip);

    char line[CMD_BUF];
    while (running && c->active) {
        ssize_t n = recv_line(c->sock, line, sizeof(line));
        if (n > 0) {
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) { line[n-1] = '\0'; n--; }

            if (n == 0) continue;

            fprintf(stdout, "Cmd from %s: '%s'\n", c->ip, line);

            if (strncmp(line, "download:", 9) == 0) {
                const char *fname = line + 9;
                if (strlen(fname) == 0) {
                    char err[] = "ERROR:No filename provided\n";
                    client_send_locked(c, err, strlen(err));
                } else {
                    send_file_to_client(c, fname);
                }
            } else if (strncmp(line, "upload:", 7) == 0) {
                if (handle_upload(c, line) != 0) {
                    fprintf(stderr, "Upload failed for client %s\n", c->ip);
                }
            } else if (strcmp(line, "LIST?") == 0) {
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
    remove_client(c);
    return NULL;
}

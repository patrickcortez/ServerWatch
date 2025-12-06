#include "metadata.h"

static metadata_node_t *metadata_head = NULL;
static pthread_mutex_t metadata_lock = PTHREAD_MUTEX_INITIALIZER;

void add_metadata_cache(const char *filename, const char *username, time_t timestamp) {
    metadata_node_t *node = malloc(sizeof(metadata_node_t));
    if (!node) return;
    strncpy(node->filename, filename, sizeof(node->filename)-1);
    node->filename[sizeof(node->filename)-1] = '\0';
    strncpy(node->username, username, sizeof(node->username)-1);
    node->username[sizeof(node->username)-1] = '\0';
    node->timestamp = timestamp;

    pthread_mutex_lock(&metadata_lock);
    node->next = metadata_head;
    metadata_head = node;
    pthread_mutex_unlock(&metadata_lock);
}

void load_metadata_cache(void) {
    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/.file_metadata", watch_dir);
    FILE *f = fopen(meta_path, "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        char *pipe1 = strchr(p, '|');
        if (!pipe1) continue;
        *pipe1 = '\0';

        char *pipe2 = strchr(pipe1 + 1, '|');
        if (!pipe2) continue;
        *pipe2 = '\0';

        add_metadata_cache(p, pipe1 + 1, (time_t)atol(pipe2 + 1));
    }
    fclose(f);
}

void save_metadata(const char *filename, const char *username) {
    time_t now = time(NULL);
    
    add_metadata_cache(filename, username, now);

    char meta_path[PATH_MAX];
    snprintf(meta_path, sizeof(meta_path), "%s/.file_metadata", watch_dir);
    FILE *f = fopen(meta_path, "a");
    if (f) {
        fprintf(f, "%s|%s|%ld\n", filename, username, (long)now);
        fclose(f);
    }
}

int find_metadata(const char *fname, char *out_user, time_t *out_time) {
    int found = 0;
    pthread_mutex_lock(&metadata_lock);
    metadata_node_t *cur = metadata_head;
    while (cur) {
        if (strcmp(cur->filename, fname) == 0) {
            strcpy(out_user, cur->username);
            *out_time = cur->timestamp;
            found = 1;
            break;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&metadata_lock);
    return found;
}

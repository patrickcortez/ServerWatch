#include "file_ops.h"
#include "metadata.h"

int sanitize_filename(const char *fname) {
    if (!fname || fname[0] == '\0') return -1;
    if (strstr(fname, "/") != NULL) return -1;
    if (strstr(fname, "..") != NULL) return -1;
    return 0;
}

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

    while ((entry = readdir(d)) != NULL) {

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (entry->d_name[0] == '.') continue; 

        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", watch_dir, entry->d_name);

        struct stat st;
        if (stat(full, &st) == 0 && S_ISREG(st.st_mode)) {
            size_t name_len = strlen(entry->d_name);
            if (len + name_len + 2 >= cap) {
                cap *= 2;
                char *tmp = realloc(out, cap);
                if (!tmp) { free(out); closedir(d); return NULL; }
                out = tmp;
            }
            strcat(out, entry->d_name);
            
            char user[64] = "Unknown";
            time_t ts = st.st_mtime; 
            char fetched_user[64];
            time_t fetched_ts;
            if (find_metadata(entry->d_name, fetched_user, &fetched_ts)) {
                strcpy(user, fetched_user);
                ts = fetched_ts;
            }
            
            char meta_str[128];
            snprintf(meta_str, sizeof(meta_str), "|%s|%ld", user, (long)ts);

             if (len + strlen(meta_str) + 100 >= cap) { 
                 cap *= 2;
                 out = realloc(out, cap);
             }
             strcat(out, meta_str);

            strcat(out, "\n");
            len = strlen(out);
        }
    }

    closedir(d);
    return out; 
}

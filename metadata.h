#ifndef METADATA_H
#define METADATA_H

#include "common.h"

/* Metadata Cache Node */
typedef struct metadata_node {
    char filename[256];
    char username[64];
    time_t timestamp;
    struct metadata_node *next;
} metadata_node_t;

void load_metadata_cache(void);
void save_metadata(const char *filename, const char *username);
int find_metadata(const char *fname, char *out_user, time_t *out_time);
void add_metadata_cache(const char *filename, const char *username, time_t timestamp);

#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "utils.h"
#include "resource_cache.h"

#define RESOURCE_CACHE_SIZE 100
#define MAX_FILENAME_LENGTH 100

typedef struct {
    char filename[MAX_FILENAME_LENGTH];
    char *buf;
    long bufs;
} resource_cache_entry_t;

static resource_cache_entry_t cache[RESOURCE_CACHE_SIZE];
static int resource_cache_pos = 0;

void
resource_cache_get_file_buffer(const char *filename, char **f, long *fs)
{
    for (int i = 0; i < resource_cache_pos; i++) {
        if (strcmp(cache[i].filename, filename) == 0) {
            *f = cache[i].buf;
            if (fs) {
                *fs = cache[i].bufs;
            }
            return;
        }
    }

    int len = strlen(filename);
    if (len + 1 > MAX_FILENAME_LENGTH) {
        fprintf(stderr, "resource_cache_get_file_buffer: Filename too long.\n");
        exit(1);
    }

    if (resource_cache_pos == RESOURCE_CACHE_SIZE) {
        fprintf(stderr, "resource_cache_get_file_buffer: Ran out of cache space.\n");
        exit(1);
    }

    char *buf;
    long bufs;
    int ret = load_file_to_new_buffer(filename, &buf, &bufs);
    if (ret != 0) {
        exit(1);
    }

    resource_cache_entry_t *entry = &cache[resource_cache_pos];
    memcpy(entry->filename, filename, len + 1);
    entry->buf = buf;
    entry->bufs = bufs;
    resource_cache_pos++;

    *f = buf;
    if (fs) {
        *fs = bufs;
    }
}

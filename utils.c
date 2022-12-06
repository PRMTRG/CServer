#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>

#include "utils.h"

static struct timespec saved_time;

/*
 * The created buffer is zero-terminated.
 * Length (fs) does not include the terminating character.
 */
int
load_file_to_new_buffer(const char *filename, char **f, long *fs)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "load_file_to_new_buffer: Failed to open file: %s.\n", filename);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);
    char *fbuf = malloc(fsize + 1);
    if (!fbuf) {
        fprintf(stderr, "load_file_to_new_buffer: malloc() failed.\n");
        exit(1);
    }
    long nwritten = fread(fbuf, 1, fsize, fp);
    if (nwritten != fsize) {
        fprintf(stderr, "load_file_to_new_buffer: fread() failed.\n");
        exit(1);
    }
    fclose(fp);
    fbuf[nwritten] = '\0';
    *f = fbuf;
    if (fs)
        *fs = nwritten;
    return 0;
}

void
parse_long(long **l, char *str)
{
    errno = 0;
    int succ = 1;
    char *endptr;
    long res = strtol(str, &endptr, 10);
    if (errno == ERANGE) {
        /* Value is outside long int range. */
        succ = 0;
    } else if (endptr == str) {
        /* Didn't parse anything. */
        succ = 0;
    } else if (*endptr) {
        /* Didn't parse the whole string. */
        succ = 0;
    }
    if (succ) {
        **l = res;
    } else {
        *l = NULL;
    }
}

void
append_to_buffer_realloc_if_necessary(char **buf, long *bufpos, long *bufs, char *str, long len)
{
    if (*bufpos + len > *bufs) {
        long newsize = *bufpos + len;
        char *newbuf = realloc(*buf, newsize);
        if (!newbuf) {
            fprintf(stderr, "append_to_buffer_realloc_if_necessary: realloc() failed.\n");
            exit(1);
        }
        *buf = newbuf;
        *bufs = newsize;
    }
    memcpy(&(*buf)[*bufpos], str, len);
    *bufpos += len;
}

void
string_to_lowercase(char *str)
{
    while (*str) {
        *str = tolower(*str);
        str++;
    }
}

char *
copy_string(const char *str)
{
    int size = strlen(str) + 1;
    char *res = malloc(size);
    if (!res) {
        fprintf(stderr, "copy_string: malloc() failed.\n");
        exit(1);
    }
    memcpy(res, str, size);
    return res;
}

/* Doesn't check for collisions because who cares. */
void
gen_filename(char *buf, const int maxlen, const char *ext, const int extlen)
{
    const int hashlen = 20;

    if (maxlen < hashlen + extlen + 1) {
        fprintf(stderr, "gen_filename: Buffer too small.\n");
        exit(1);
    }

    for (int i = 0; i < hashlen; i++) {
        buf[i] = 'A' + (rand() % ('Z' - 'A'));
    }
    for (int i = 0; i < extlen; i++) {
        buf[hashlen + i] = ext[i];
    }
    buf[hashlen + extlen] = '\0';
}

void
save_file(const char *buf, const long bufs, const char *filename, const char *directory)
{
    int dlen = strlen(directory);
    int flen = strlen(filename);

    char path[256];
    if (dlen + flen + 1 + 1 > 256) {
        fprintf(stderr, "save_file: Path buffer too small.\n");
        exit(1);
    }

    memcpy(path, directory, dlen);
    path[dlen] = '/';
    memcpy(&path[dlen + 1], filename, flen);
    path[dlen + 1 + flen] = '\0';

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        fprintf(stderr, "save_file: Failed to open file.\n");
        exit(1);
    }
    long nwritten = fwrite(buf, 1, bufs, fp);
    if (nwritten != bufs) {
        perror("save_file: fwrite()");
        exit(1);
    }

    fclose(fp);
}

void
start_timer(void)
{
    if (clock_gettime(CLOCK_MONOTONIC, &saved_time) == -1) {
        fprintf(stderr, "start_timer: clock_gettime() failed.\n");
        exit(1);
    }
}

void
stop_timer(void)
{
    printf("res = %ld.%09ld\n", saved_time.tv_sec, saved_time.tv_nsec);
    if (clock_gettime(CLOCK_MONOTONIC, &saved_time) == -1) {
        fprintf(stderr, "stop_timer: clock_gettime() failed.\n");
        exit(1);
    }
    printf("res = %ld.%09ld\n", saved_time.tv_sec, saved_time.tv_nsec);
}

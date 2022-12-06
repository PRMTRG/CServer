#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "utils.h"
#include "response.h"
#include "forum.h"
#include "resource_cache.h"
#include "templating.h"

#define MAX_FUN_ARG_LEN 50

static long
snp_post_in_thread_img(char *s, long n, char *format,
        char *name, char *timestamp, long post_id, char *filename, char *comment)
{
    return snprintf(s, n, format,
            post_id, name, timestamp, post_id, post_id, post_id, filename, filename, comment);
}

static long
snp_post_in_thread_noimg(char *s, long n, char *format,
        char *name, char *timestamp, long post_id, char *comment)
{
    return snprintf(s, n, format,
            post_id, name, timestamp, post_id, post_id, post_id, comment);
}

static long
snp_post_in_catalog(char *s, long n, char *format,
        char *subject, char *name, char *timestamp, long post_id, char *filename, char *comment)
{
    return snprintf(s, n, format,
            subject, name, timestamp, post_id, post_id, filename, filename, comment, post_id);
}

static void
tfun_include(char **buf, long *bufpos, long *bufs, const char *filename)
{
    int len = strlen(filename);
    const char template_parts_dir[] = "templates/parts/";
    char part_path[100];
    memcpy(part_path, template_parts_dir, sizeof(template_parts_dir) - 1);
    if (sizeof(template_parts_dir) + len > 100) {
        fprintf(stderr, "tfun_include: Template part path+filename too large.\n");
        exit(1);
    }
    memcpy(&part_path[sizeof(template_parts_dir) - 1], filename, len + 1);

    char *fbuf;
    long fbufs;
    resource_cache_get_file_buffer(part_path, &fbuf, &fbufs);

    if (*bufpos + fbufs + 1 > *bufs) {
        long newsize = *bufpos + fbufs + 1;
        char *newbuf = realloc(*buf, newsize);
        if (!newbuf) {
            fprintf(stderr, "tfun_include: realloc() failed.\n");
            exit(1);
        }
        *buf = newbuf;
        *bufs = newsize;
    }

    memcpy(&(*buf)[*bufpos], fbuf, fbufs);
    *bufpos += fbufs;
    (*buf)[(*bufpos)++] = '\n';
}

static void
tfun_title(char **buf, long *bufpos, long *bufs, const char *title)
{
    const char t[] = "<title>%s</title>\n";
    int nwritten = snprintf(&(*buf)[*bufpos], *bufs - *bufpos, t, title);
    if (*bufpos + nwritten > *bufs) {
        fprintf(stderr, "tfun_title: Buffer too small.\n");
        exit(1);
    }
    *bufpos += nwritten;
}

static void
tfun_new_post_form(char **buf, long *bufpos, long *bufs, const long thread_id)
{
    char *format;
    resource_cache_get_file_buffer("templates/parts/new_post_form.html", &format, NULL);

    int nwritten = snprintf(&(*buf)[*bufpos], *bufs - *bufpos, format, thread_id);
    if (*bufpos + nwritten > *bufs) {
        fprintf(stderr, "tfun_new_post_form: Buffer too small.\n");
        exit(1);
    }
    *bufpos += nwritten;
}

static void
tfun_posts_in_thread(char **buf, long *bufpos, long *bufs, post_t *posts, long nposts)
{
    char *format_img;
    resource_cache_get_file_buffer("templates/parts/post_in_thread_img.html", &format_img, NULL);
    char *format_noimg;
    resource_cache_get_file_buffer("templates/parts/post_in_thread_noimg.html", &format_noimg, NULL);

    for (long i = 0; i < nposts; i++) {
        post_t *p = &posts[i];
        if (p->hidden) {
            continue;
        }
        while (1) {
            int ok = 1;
            long nwritten;
            if (*p->filename) {
                nwritten = snp_post_in_thread_img(&(*buf)[*bufpos], *bufs - *bufpos, format_img,
                        p->name, p->timestamp, p->post_id, p->filename, p->comment);
            } else {
                nwritten = snp_post_in_thread_noimg(&(*buf)[*bufpos], *bufs - *bufpos, format_noimg,
                        p->name, p->timestamp, p->post_id, p->comment);
            }
            if (*bufpos + nwritten + 1 > *bufs) {
                ok = 0;
                long newsize = *bufs * 2;
                if (newsize >= MAX_RESP_SIZE_1) {
                    fprintf(stderr, "tfun_posts_in_thread: Buffer size would exceed MAX_RESP_SIZE_1.\n");
                    exit(1);
                }
                char *newbuf = realloc(*buf, newsize);
                if (!newbuf) {
                    fprintf(stderr, "tfun_posts_in_thread: realloc() failed.\n");
                    exit(1);
                }
                *buf = newbuf;
                *bufs = newsize;
            }
            if (ok) {
                *bufpos += nwritten;
                break;
            }
        }
        (*buf)[(*bufpos)++] = '\n';
    }
}

static void
tfun_posts_in_catalog(char **buf, long *bufpos, long *bufs, thread_t *threads, long nthreads)
{
    char *format;
    resource_cache_get_file_buffer("templates/parts/post_in_catalog.html", &format, NULL);

    for (long i = 0; i < nthreads; i++) {
        thread_t *t = &threads[i];
        post_t *p = &t->posts[0];
        while (1) {
            int ok = 1;
            long nwritten = snp_post_in_catalog(&(*buf)[*bufpos], *bufs - *bufpos, format,
                    t->subject, p->name, p->timestamp, p->post_id, p->filename, p->comment);
            if (*bufpos + nwritten + 1 > *bufs) {
                ok = 0;
                long newsize = *bufs * 2;
                if (newsize >= MAX_RESP_SIZE_1) {
                    fprintf(stderr, "tfun_posts_in_catalog: Buffer size would exceed MAX_RESP_SIZE_1.\n");
                    exit(1);
                }
                char *newbuf = realloc(*buf, newsize);
                if (!newbuf) {
                    fprintf(stderr, "tfun_posts_in_catalog: realloc() failed.\n");
                    exit(1);
                }
                *buf = newbuf;
                *bufs = newsize;
            }
            if (ok) {
                *bufpos += nwritten;
                break;
            }
        }
        (*buf)[(*bufpos)++] = '\n';
    }
}

static int
parse_template_line(char *line, char **cmd, char **arg)
{
    int len = strlen(line);
    if (len < 10 || line[len - 2] != '}' || line[len - 3] != '}') {
        return 1;
    }
    *cmd = line + 3;
    char *space = strchr(*cmd, ' ');
    if (!space) {
        return 1;
    }
    *space = '\0';
    *arg = space + 1;
    space = strchr(*arg, ' ');
    if (!space) {
        return 1;
    }
    *space = '\0';
    return 0;
}

static long
get_line(const char *buf, long *bufpos, long bufs, char *linebuf, long linebufs)
{
    if (*bufpos == bufs)
        return 0;

    int i;
    for (i = 0; i < linebufs - 1; ) {
        char c = buf[(*bufpos)++];
        linebuf[i++] = c;
        if (c == '\n')
            break;
        if (*bufpos == bufs)
            break;
    }
    if (i == linebufs - 1) {
        fprintf(stderr, "get_line: Line buffer too small.\n");
        exit(1);
    }
    linebuf[i] = '\0';
    return i;
}

static int
parse_template(char *fbuf, long *fbufpos, long fbufs, char **buf, long *bufpos, long *bufs, char *arg)
{
    long linebufs = 1024;
    char linebuf[linebufs];
    long len;
    while ((len = get_line(fbuf, fbufpos, fbufs, linebuf, linebufs)) > 0) {
        if (linebuf[0] != '{' || linebuf[1] != '{') {
            append_to_buffer_realloc_if_necessary(buf, bufpos, bufs, linebuf, len);
        } else {
            char *cmd;
            char *tmp_arg;
            if (parse_template_line(linebuf, &cmd, &tmp_arg) != 0) {
                fprintf(stderr, "parse_template: Failed to parse template.\n");
                exit(1);
            }
            if (strcmp(cmd, "include") == 0) {
                tfun_include(buf, bufpos, bufs, tmp_arg);
            } else if (strcmp(cmd, "fun") == 0) {
                memcpy(arg, tmp_arg, MAX_FUN_ARG_LEN);
                return 1;
            } else {
                fprintf(stderr, "parse_template: Invalid template command.\n");
                exit(1);
            }
        }
    }
    return 0;
}

void
template_thread(handler_t *h, long thread_id, const int headers_only)
{
    const char *filename = "templates/thread.html";

    post_t *posts;
    long nposts;
    int ret = posts_get_by_thread_id(thread_id, &posts, &nposts);
    if (ret != 0) {
        fprintf(stderr, "template_thread: posts_get_by_thread_id() failed.\n");
        serve_error_404(h);
        return;
    }

    char title[100];
    snprintf(title, 100, "Thread no. %ld", thread_id);

    long bufs = 4 * 1024 + nposts * 1024;
    if (bufs > MAX_RESP_SIZE_1) {
        fprintf(stderr, "template_thread: Buffer size would exceed MAX_RESP_SIZE_1.\n");
        exit(1);
    }
    char *buf = malloc(bufs);
    if (!buf) {
        fprintf(stderr, "template_thread: malloc() failed.\n");
        exit(1);
    }
    long bufpos = 0;
    char *fbuf;
    long fbufpos = 0;
    long fbufs;
    resource_cache_get_file_buffer(filename, &fbuf, &fbufs);

    while (1) {
        char arg[MAX_FUN_ARG_LEN];
        int ret = parse_template(fbuf, &fbufpos, fbufs, &buf, &bufpos, &bufs, arg);
        if (ret == 0) {
            break;
        }
        if (strcmp(arg, "title") == 0) {
            tfun_title(&buf, &bufpos, &bufs, title);
        } else if (strcmp(arg, "new_post_form") == 0) {
            tfun_new_post_form(&buf, &bufpos, &bufs, thread_id);
        } else if (strcmp(arg, "posts_in_thread") == 0) {
            tfun_posts_in_thread(&buf, &bufpos, &bufs, posts, nposts);
        } else {
            fprintf(stderr, "template_thread: Invalid template command argument: %s.\n", arg);
            exit(1);
        }
    }

    if (headers_only) {
        serve_html_file_from_buffer(h, NULL, bufpos);
        free(buf);
    } else {
        serve_html_file_from_buffer(h, buf, bufpos);
    }
}

void
template_catalog(handler_t *h, const int headers_only)
{
    const char *filename = "templates/catalog.html";

    thread_t *threads;
    long nthreads;
    threads_get(&threads, &nthreads);

    long bufs = 4 * 1024 + nthreads * 1024;
    if (bufs > MAX_RESP_SIZE_1) {
        fprintf(stderr, "template_catalog: Buffer size would exceed MAX_RESP_SIZE_1.\n");
        exit(1);
    }
    char *buf = malloc(bufs);
    if (!buf) {
        fprintf(stderr, "template_catalog: malloc() failed.\n");
        exit(1);
    }
    long bufpos = 0;
    char *fbuf;
    long fbufpos = 0;
    long fbufs;
    resource_cache_get_file_buffer(filename, &fbuf, &fbufs);

    while (1) {
        char arg[MAX_FUN_ARG_LEN];
        int ret = parse_template(fbuf, &fbufpos, fbufs, &buf, &bufpos, &bufs, arg);
        if (ret == 0) {
            break;
        }
        if (strcmp(arg, "posts_in_catalog") == 0) {
            if (nthreads > 0) {
                tfun_posts_in_catalog(&buf, &bufpos, &bufs, threads, nthreads);
            } else {
                tfun_include(&buf, &bufpos, &bufs, "no_threads_active.html");
            }
        } else {
            fprintf(stderr, "template_catalog: Invalid template command argument: %s.\n", arg);
            exit(1);
        }
    }

    if (headers_only) {
        serve_html_file_from_buffer(h, NULL, bufpos);
        free(buf);
    } else {
        serve_html_file_from_buffer(h, buf, bufpos);
    }
}

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "response.h"
#include "resource_cache.h"

static void
response_add_status_line(char *buf, long *bufpos, const int code)
{
    const char protocol[] = "HTTP/1.0";
    const char c200str[] = "200 OK";
    const char c303str[] = "303 SEE OTHER";
    const char c400str[] = "400 BAD REQUEST";
    const char c404str[] = "404 NOT FOUND";
    const char c500str[] = "500 INTERNAL SERVER ERROR";
    const char endline[] = "\r\n";
    int l;

    l = sizeof(protocol) - 1;
    memcpy(&buf[*bufpos], protocol, l);
    *bufpos += l;

    buf[(*bufpos)++] = ' ';

    switch(code) {
        case 200: {
            l = sizeof(c200str) - 1;
            memcpy(&buf[*bufpos], c200str, l);
            *bufpos += l;
        } break;
        case 400: {
            l = sizeof(c400str) - 1;
            memcpy(&buf[*bufpos], c400str, l);
            *bufpos += l;
        } break;
        case 303: {
            l = sizeof(c303str) - 1;
            memcpy(&buf[*bufpos], c303str, l);
            *bufpos += l;
        } break;
        case 404: {
            l = sizeof(c404str) - 1;
            memcpy(&buf[*bufpos], c404str, l);
            *bufpos += l;
        } break;
        case 500: {
            l = sizeof(c500str) - 1;
            memcpy(&buf[*bufpos], c500str, l);
            *bufpos += l;
        } break;
        default: {
            fprintf(stderr, "response_add_status_line: Invalid code.\n");
            exit(1);
        } break;
    }

    l = sizeof(endline) - 1;
    memcpy(&buf[*bufpos], endline, l);
    *bufpos += l;
}

static void
response_add_header_field(char *buf, long *bufpos, const char *name, const char *value)
{
    const char sep[] = ": ";
    const char endline[] = "\r\n";
    int l;

    l = strlen(name);
    memcpy(&buf[*bufpos], name, l);
    *bufpos += l;

    l = sizeof(sep) - 1;
    memcpy(&buf[*bufpos], sep, l);
    *bufpos += l;

    l = strlen(value);
    memcpy(&buf[*bufpos], value, l);
    *bufpos += l;

    l = sizeof(endline) - 1;
    memcpy(&buf[*bufpos], endline, l);
    *bufpos += l;
}

static void
response_add_content_type(char *buf, long *bufpos, const char *mime_type)
{
    const char a[] = "Content-Type: ";
    const char b1[] = "; charset=utf-8\r\n";
    const char b2[] = "\r\n";
    int l;

    l = sizeof(a) - 1;
    memcpy(&buf[*bufpos], a, l);
    *bufpos += l;

    l = strlen(mime_type);
    memcpy(&buf[*bufpos], mime_type, l);
    *bufpos += l;

    if (strcmp(mime_type, "text/html") == 0) {
        l = sizeof(b1) - 1;
        memcpy(&buf[*bufpos], b1, l);
        *bufpos += l;
    } else {
        l = sizeof(b2) - 1;
        memcpy(&buf[*bufpos], b2, l);
        *bufpos += l;
    }
}

static void
response_add_content_length(char *buf, long *bufpos, const long len)
{
    const char a[] = "Content-Length: ";
    const char b[] = "\r\n";
    int l;

    l = sizeof(a) - 1;
    memcpy(&buf[*bufpos], a, l);
    *bufpos += l;

    int ret = sprintf(&buf[*bufpos], "%ld", len);
    if (ret < 1) {
        fprintf(stderr, "response_add_content_length: sprintf() failed.\n");
        exit(1);
    }
    *bufpos += ret;

    l = sizeof(b) - 1;
    memcpy(&buf[*bufpos], b, l);
    *bufpos += l;
}

static void
response_add_header_end(char *buf, long *bufpos)
{
    const char endline[] = "\r\n";
    int l;

    l = sizeof(endline) - 1;
    memcpy(&buf[*bufpos], endline, l);
    *bufpos += l;
}

static int
handler_send_buffer(int sock, handler_args_t *args)
{
    handler_args_send_buffer_t *a = &args->send_buffer;

    if (a->headers_bufpos < a->headers_bufs) {
        long nwritten = write(sock, a->headers_buf, a->headers_bufs - a->headers_bufpos);
        if (nwritten < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
            }

            fprintf(stderr, "handler_send_buffer: Failed write.\n");
            return -1;
        }

        a->headers_bufpos += nwritten;
        if (a->headers_bufpos == a->headers_bufs && !a->body_buf) {
            return 0;
        }
    }

    if (a->headers_bufpos == a->headers_bufs && a->body_buf) {
        long nwritten = write(sock, a->body_buf, a->body_bufs - a->body_bufpos);
        if (nwritten < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 1;
            }

            fprintf(stderr, "handler_send_buffer: Failed write.\n");
            return -1;
        }

        a->body_bufpos += nwritten;
        if (a->body_bufpos == a->body_bufs) {
            return 0;
        }
    }

    return 1;
}

static void
handler_after_send_buffer(handler_args_t *args)
{
    handler_args_send_buffer_t *a = &args->send_buffer;
    if (a->body_buf && a->free_body) {
        free(a->body_buf);
    }
}

static void
handler_init_send_buffer(handler_t *h, char *headers_buf, long headers_bufs, char *body_buf, long body_bufs, int free_body)
{
    h->handler = handler_send_buffer;
    h->handler_after = handler_after_send_buffer;
    h->args.send_buffer.headers_buf = headers_buf;
    h->args.send_buffer.headers_bufs = headers_bufs;
    h->args.send_buffer.body_buf = body_buf;
    h->args.send_buffer.body_bufs = body_bufs;
    h->args.send_buffer.free_body = free_body;
}

static void
write_headers(char **buf, long *bufs, const long body_size, const char *mime_type, const int code)
{
    long bufpos = 0;
    response_add_status_line(*buf, &bufpos, code);
    response_add_header_field(*buf, &bufpos, "Server", "UwU");
    if (mime_type) {
        response_add_content_type(*buf, &bufpos, mime_type);
        response_add_content_length(*buf, &bufpos, body_size);
    }
    response_add_header_field(*buf, &bufpos, "Connection", "close");
    *bufs = bufpos;
}

static void
serve_file_from_disk_with_code(handler_t *h, const char *filename, const char *mime_type, const int code, const int headers_only)
{
    FILE *fp = fopen(filename, "rb");
    if (!fp) {
        fprintf(stderr, "serve_file_from_disk_with_code: Failed to open file %s.\n", filename);
        serve_error_404(h);
        return;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    rewind(fp);

    char *headers_buf = h->resp_headers_buf;
    long headers_bufs;
    write_headers(&headers_buf, &headers_bufs, fsize, mime_type, code);
    response_add_header_end(headers_buf, &headers_bufs);

    if (headers_only) {
        handler_init_send_buffer(h, headers_buf, headers_bufs, NULL, 0, 0);
    } else {
        char *body_buf = malloc(fsize);
        if (!body_buf) {
            fprintf(stderr, "serve_file_from_disk_with_code: malloc() failed.\n");
            exit(1);
        }
        long nwritten = fread(body_buf, 1, fsize, fp);
        if (nwritten != fsize) {
            fprintf(stderr, "serve_file_from_disk_with_code: fread() failed.\n");
            exit(1);
        }
        handler_init_send_buffer(h, headers_buf, headers_bufs, body_buf, fsize, 1);
    }
    fclose(fp);
}

static void
serve_file_from_buffer_with_code(handler_t *h, char *buf, const long bufs, const char *mime_type, const int code)
{
    char *headers_buf = h->resp_headers_buf;
    long headers_bufs;
    write_headers(&headers_buf, &headers_bufs, bufs, mime_type, code);
    response_add_header_end(headers_buf, &headers_bufs);

    if (buf) {
        handler_init_send_buffer(h, headers_buf, headers_bufs, buf, bufs, 1);
    } else {
        handler_init_send_buffer(h, headers_buf, headers_bufs, NULL, 0, 0);
    }
}

static void
serve_file_from_cache_with_code(handler_t *h, char *filename, const char *mime_type, const int code, const int headers_only)
{
    char *body_buf;
    long body_bufs;
    resource_cache_get_file_buffer(filename, &body_buf, &body_bufs);

    char *headers_buf = h->resp_headers_buf;
    long headers_bufs;
    write_headers(&headers_buf, &headers_bufs, body_bufs, mime_type, code);
    response_add_header_end(headers_buf, &headers_bufs);

    if (headers_only) {
        handler_init_send_buffer(h, headers_buf, headers_bufs, NULL, 0, 0);
    } else {
        handler_init_send_buffer(h, headers_buf, headers_bufs, body_buf, body_bufs, 0);
    }
}

void
serve_file_from_disk(handler_t *h, const char *filename, const char *mime_type, const int headers_only)
{
    serve_file_from_disk_with_code(h, filename, mime_type, 200, headers_only);
}

void
serve_html_file_from_disk(handler_t *h, const char *filename, const int headers_only)
{
    serve_file_from_disk_with_code(h, filename, "text/html", 200, headers_only);
}

void
serve_file_from_buffer(handler_t *h, char *buf, const long bufs, const char *mime_type)
{
    serve_file_from_buffer_with_code(h, buf, bufs, mime_type, 200);
}

void
serve_html_file_from_buffer(handler_t *h, char *buf, const long bufs)
{
    serve_file_from_buffer_with_code(h, buf, bufs, "text/html", 200);
}

void
serve_redirect_303(handler_t *h, char *location)
{
    char *headers_buf = h->resp_headers_buf;
    long headers_bufs;
    write_headers(&headers_buf, &headers_bufs, 0, NULL, 303);
    response_add_header_field(headers_buf, &headers_bufs, "Location", location);
    response_add_header_end(headers_buf, &headers_bufs);

    handler_init_send_buffer(h, headers_buf, headers_bufs, NULL, 0, 0);
}

void
serve_error_400(handler_t *h)
{
    serve_file_from_cache_with_code(h, "html/400.html", "text/html", 400, 0);
}

void
serve_error_404(handler_t *h)
{
    serve_file_from_cache_with_code(h, "html/404.html", "text/html", 404, 0);
}

void
serve_error_500(handler_t *h)
{
    serve_file_from_cache_with_code(h, "html/500.html", "text/html", 500, 0);
}

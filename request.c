#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include "utils.h"
#include "request.h"

static const char headers_end_str[] = "\r\n\r\n";
static const char headers_line_delim[] = "\r\n";
static const char multipart_formdata_str[] = "multipart/form-data";

enum read_headers_result
read_headers(int sock, int *headers_end_state, char *buf, long *bufpos, long bufs, long *headers_len, long *rem_len)
{
    long nread = read(sock, &buf[*bufpos], bufs - *bufpos);
    if (nread == -1) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return READ_HEADERS_CONTINUE;
        }

        perror("read_headers: read()");
        return READ_HEADERS_FAILED_CLOSE_CONNECTION;
    }
    if (nread == 0) {
        //fprintf(stderr, "read_headers: Reached EOF without finding the end of headers.\n");
        if (*bufpos == 0) {
            return READ_HEADERS_FAILED_CLOSE_CONNECTION;
        }
        return READ_HEADERS_FAILED_SEND_400;
    }

    int headers_end_found = 0;
    for (long i = *bufpos; i < *bufpos + nread; i++) {
        char c = buf[i];
        if (c != '\r' && c != '\n' && c != ' ' && !isalnum(c) && !ispunct(c)) {
            fprintf(stderr, "read_headers: Illegal character in headers.\n");
            return READ_HEADERS_FAILED_SEND_400;
        }
        if (c == headers_end_str[*headers_end_state]) {
            (*headers_end_state)++;
        } else {
            *headers_end_state = 0;
        }
        if (*headers_end_state == sizeof(headers_end_str) - 1) {
            headers_end_found = 1;
            *headers_len = i + 1;
            break;
        }
    }
    *bufpos += nread;
    if (headers_end_found) {
        *rem_len = *bufpos - *headers_len;
        return READ_HEADERS_DONE;
    }
    if (*bufpos == bufs) {
        fprintf(stderr, "read_headers: Headers section too large.\n");
        return READ_HEADERS_FAILED_SEND_400;
    }
    return READ_HEADERS_CONTINUE;
}

static void
parse_route(char *route, char **path, char **params)
{
    *path = route;
    *params = NULL;
    char *question_mark = strchr(route, '?');
    if (question_mark) {
        *question_mark = '\0';
        if (*(question_mark + 1))
            *params = question_mark + 1;
    }
}

static void
parse_content_type(char *value, char **mime_type, char **boundary)
{
    char *semicolon = strchr(value, ';');
    if (!semicolon) {
        *mime_type = value;
        *boundary = NULL;
        return;
    }

    *semicolon = '\0';
    *mime_type = value;

    char *b = semicolon + 1;
    while (*b == ' ') {
        b++;
    }
    char *equals = strchr(b, '=');
    if (!equals) {
        *boundary = NULL;
        return;
    }

    *equals = '\0';

    if (strcmp(b, "boundary") != 0) {
        *boundary = NULL;
        return;
    }

    *boundary = equals + 1;
}

int
parse_headers(request_t *req, char *buf, long bufs)
{
    req->ct = RCT_NONE;

    if (bufs < (int)sizeof("GET / HTTP\r\n\r\n") - 1) {
        /* Consider "GET / HTTP\r\n\r\n" as the minimal valid request. */
        fprintf(stderr, "parse_headers: Request too small.\n");
        return 1;
    }
    if (buf[0] == headers_line_delim[0] && buf[1] == headers_line_delim[1]) {
        fprintf(stderr, "parse_headers: First line of request is empty.\n");
        return 1;
    }
    buf[bufs - (sizeof(headers_end_str) - 1)] = '\0';

    int request_line = 1;
    char *cur_line = buf;
    while (1) {
        char *next_line = NULL;
        char *next_delim = strstr(cur_line, headers_line_delim);
        if (next_delim) {
            *next_delim = '\0';
            next_line = next_delim + (sizeof(headers_line_delim) - 1);
        }
        if (request_line) {

            /* HTTP request line format: */
            /* Method Route Protocol */
            request_line = 0;
            char *s1 = strchr(cur_line, ' ');
            if (!s1) {
                return 1;
            }
            *s1 = '\0';

            if (strcmp(cur_line, "GET") == 0) {
                req->meth = RM_GET;
            } else if (strcmp(cur_line, "HEAD") == 0) {
                req->meth = RM_HEAD;
            } else if (strcmp(cur_line, "POST") == 0) {
                req->meth = RM_POST;
            } else {
                fprintf(stderr, "parse_headers: Invalid request method.\n");
                return 1;
            }

            cur_line = s1 + 1;
            char *s2 = strchr(cur_line, ' ');
            if (!s2) {
                return 1;
            }
            *s2 = '\0';
            parse_route(cur_line, &req->path, &req->params);

        } else {

            /* HTTP request header field format: */
            /* FieldName: (optional whitespace) field_value (optional whitespace) CRLF */
            char *colon = strchr(cur_line, ':');
            if (!colon) {
                fprintf(stderr, "parse_headers: No colon in header line.\n");
                return 1;
            }
            *colon = '\0';
            char *field_name = cur_line;
            char *field_value = colon + 1;
            while (*field_value == ' ')
                field_value++;
            int len = strlen(field_value);
            while (*(field_value + len) == ' ') {
                *(field_value + len) = '\0';
                len--;
            }
            string_to_lowercase(field_name);

            if (strcmp(field_name, "content-type") == 0) {

                char *mime_type;
                char *boundary;
                parse_content_type(field_value, &mime_type, &boundary);

                if (strcmp(mime_type, multipart_formdata_str) != 0) {
                    fprintf(stderr, "parse_headers: Invalid Content-Type: %s.\n", field_value);
                    return 1;
                }
                if (!boundary) {
                    fprintf(stderr, "parse_headers: Missing boundary.\n");
                    return 1;
                }

                int len = strlen(boundary);
                if (len < 27) {
                    fprintf(stderr, "parse_headers: Boundary too small.\n");
                    return 1;
                }
                if (len > 70) {
                    fprintf(stderr, "parse_headers: Boundary too large.\n");
                    return 1;
                }

                for (int i = 0; i < len; i++) {
                    char c = boundary[i];
                    if (!isalnum(c) && c != '\'' && c != '-' && c != '_') {
                        fprintf(stderr, "parse_headers: Illegal character in boundary.\n");
                        return 1;
                    }
                }

                req->ct = RCT_MULTIPART_FORMDATA;
                req->boundary[0] = '-';
                req->boundary[1] = '-';
                memcpy(&req->boundary[2], boundary, len + 1);

            } else if (strcmp(field_name, "content-length") == 0) {

                long *lp = &req->content_length;
                parse_long(&lp, field_value);
                if (!lp) {
                    fprintf(stderr, "parse_headers: Failed to parse Content-Length value.\n");
                    return 1;
                }
                if (req->content_length < 1 || req->content_length > 1024 * 1024 * 5) {
                    fprintf(stderr, "parse_headers: Invalid Content-Length value.\n");
                    return 1;
                }

            }

        }
        if (!next_line)
            break;
        cur_line = next_line;
    }

    if (req->meth == RM_POST && req->ct == RCT_NONE) {
        fprintf(stderr, "parse_headers: POST request with no Content-Type field.\n");
        return 1;
    }
    if (req->meth == RM_POST && req->content_length == 0) {
        fprintf(stderr, "parse_headers: POST request with no Content-Length field.\n");
        return 1;
    }

    return 0;
}

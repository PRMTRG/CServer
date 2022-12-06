#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "utils.h"
#include "response.h"
#include "request.h"
#include "templating.h"
#include "forum.h"
#include "config.h"
#include "routing.h"

typedef struct {
    const char *key;
    const enum parameter_value_type type;
    char *val_s;
    long long val_i;
    const int optional;
    int ok;
} parameter_t;

enum upload_content_type {
    UCT_NONE       = 0,
    UCT_IMAGE_PNG  = 1 << 0,
    UCT_IMAGE_JPEG = 1 << 1,
};

typedef struct {
    const char *key;
    const unsigned int accepted_content_types;
    unsigned int content_type;
    const int optional;
    char *value;
    long value_len;
    int ok;
} form_field_t;

typedef struct {
    parameter_t *p;
    int np;
    form_field_t *ff;
    int nff;
    char *path_rem;
    int headers_only;
} routeargs_t;

typedef struct {
    const enum request_method meth;
    const char *path;
    const int path_wildcard;
    const parameter_t *p;
    const int np;
    const form_field_t *ff;
    const int nff;
    const long max_body_size;
    void (*const fun)(handler_t *h, routeargs_t *args);
} route_t;

typedef struct {
    const unsigned char sig[20];
    const unsigned char mask[20];
    const int len;
} filesig_t;

static enum upload_content_type
validate_uploaded_file(const char *buf, const long bufs, const unsigned int content_type, const int maxsize)
{
    if (!buf)
        return UCT_NONE;
    
    if (bufs < 100 || bufs > maxsize)
        return UCT_NONE;
    
    switch (content_type) {
        case UCT_IMAGE_PNG: {
            static const filesig_t png_signature = {
                .sig = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A },
                .len = 8,
            };

            for (int i = 0; i < png_signature.len; i++) {
                if (png_signature.sig[i] != (unsigned char) buf[i]) {
                    fprintf(stderr, "validate_uploaded_file: Invalid png signature.\n");
                    return UCT_NONE;
                }
            }

            return UCT_IMAGE_PNG;
        } break;
        
        case UCT_IMAGE_JPEG: {
            // TODO: Remove the one redundant signature.
            static const filesig_t jpg_signatures[] = {
                {
                    .sig = { 0xFF, 0xD8, 0xFF, 0xDB },
                    .len = 4,
                }, {
                    .sig = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01 },
                    .len = 12,
                }, {
                    .sig = { 0xFF, 0xD8, 0xFF, 0xEE },
                    .len = 4,
                }, {
                    .sig  = { 0xFF, 0xD8, 0xFF, 0xE1, 0x00, 0x00, 0x45, 0x78, 0x69, 0x66, 0x00, 0x00 },
                    .mask = { 0,    0,    0,    0,    1,    1,    0,    0,    0,    0,    0,    0    },
                    .len = 12,
                }, {
                    .sig = { 0xFF, 0xD8, 0xFF, 0xE0 },
                    .len = 4,
                }
            };

            for (int i = 0; i < (int) (sizeof(jpg_signatures) / sizeof(filesig_t)); i++) {
                const filesig_t *fs = &jpg_signatures[i];
                int ok = 1;
                for (int j = 0; j < fs->len; j++) {
                    if (fs->sig[j] != (unsigned char) buf[j] && !fs->mask[j]) {
                        ok = 0;
                        break;
                    }
                }
                if (ok) {
                    return UCT_IMAGE_JPEG;
                }
            }

            fprintf(stderr, "validate_uploaded_file: Invalid jpg signature.\n");
            return UCT_NONE;
        } break;

        default: {
            fprintf(stderr, "validate_uploaded_file: Invalid content_type: %u.\n", content_type);
            return UCT_NONE;
        } break;
    }
}

static int
parse_params(char *params, parameter_t *p, int np)
{
    if (!params) {
        return 1;
    }
    for (int i = 0; i < np; i++) {
        char *next_params = NULL;
        char *ampersand = strchr(params, '&');
        if (ampersand) {
            *ampersand = '\0';
            if (*(ampersand + 1))
                next_params = ampersand + 1;
        }
        char *equals = strchr(params, '=');
        if (equals && *(equals + 1)) {
            *equals = '\0';
            char *key = params;
            char *val = equals + 1;
            for (int j = 0; j < np; j++) {
                parameter_t *cp = &p[j];
                if (cp->ok)
                    continue;
                if (strcmp(cp->key, key) != 0)
                    continue;
                switch (cp->type) {
                    case PVT_STRING: {
                        cp->val_s = val;
                        cp->ok = 1;
                    } break;
                    case PVT_INTEGER: {
                        errno = 0;
                        int succ = 1;
                        char *endptr;
                        long long res = strtoll(val, &endptr, 10);
                        if (errno == ERANGE || endptr == val || *endptr) {
                            succ = 0;
                        }
                        if (succ) {
                            cp->ok = 1;
                            cp->val_i = res;
                        }
                    } break;
                }
            }
        }
        if (!next_params)
            break;
        params = next_params;
    }
    for (int i = 0; i < np; i++) {
        parameter_t *cp = &p[i];
        if (!cp->optional && !cp->ok)
            return 1;
    }
    return 0;
}

static char *
find_in_string(char *haystack, const int hn, const char *needle, const int nn)
{
    for (int i = 0; i <= hn - nn; i++) {
        int j;
        for (j = 0; j < nn; j++) {
            if (haystack[i + j] != needle[j])
                break;
        }
        if (j == nn) {
            return (haystack + i);
        }
    }
    return NULL;
}

static int
parse_mutlipart_form_data(char *buf, long bufs, char *boundary, form_field_t *ff, int nff)
{
    if (bufs < 50) {
        return 1;
    }
    if (buf[bufs - 4] != '-' || buf[bufs - 3] != '-' || buf[bufs - 2] != '\r' || buf[bufs - 1] != '\n') {
        return 1;
    }
    char *lastbyte = &buf[bufs - 4];

    int blen = strlen(boundary);

    const char content_disposition[] = "Content-Disposition: form-data;";
    const int cdlen = sizeof(content_disposition) - 1;
    const char name_key[] = "name=\"";
    const int nklen = sizeof(name_key) - 1;
    const char content_type[] = "Content-Type: ";
    const int ctlen = sizeof(content_type) - 1;

    char *t = buf;
    char *t2;
    char *t3;

    t = find_in_string(t, lastbyte - t, boundary, blen);
    if (!t) {
        return 1;
    }
    t += blen;
    /* t after end of first boundary */

    t = find_in_string(t, lastbyte - t, "\r\n", 2);
    if (!t) {
        return 1;
    }
    t += 2;
    /* t on beginning of chunk's first line */

    while (1) {
        form_field_t *f = NULL;

        t = find_in_string(t, lastbyte - t, content_disposition, cdlen);
        if (!t) {
            return 1;
        }
        t += cdlen;

        while (*t == ' ')
            t++;

        t = find_in_string(t, lastbyte - t, name_key, nklen);
        if (!t) {
            return 1;
        }
        t += nklen;
        /* t on start of field name */

        t2 = find_in_string(t, lastbyte - t, "\r\n", 2);
        if (!t2) {
            return 1;
        }

        char *name = t;
        int namelen = 0;
        while (t < t2) {
            t++;
            namelen++;
            if (!isalnum(*t))
                break;
        }
        for (int i = 0; i < nff; i++) {
            if (strncmp(name, ff[i].key, namelen) == 0) {
                f = &ff[i];
            }
        }
        if (!f) {
#if 0
            fprintf(stderr, "parse_multipart_form_data: Invalid name: ");
            for (int i = 0; i < namelen; i++)
                fprintf(stderr, "%c", name[i]);
            fprintf(stderr, ".\n");
#endif
            return 1;
        }

        t = t2 + 2;
        /* t on beginning of chunk's second line */

        t2 = find_in_string(t, lastbyte - t, boundary, blen);
        if (!t2) {
            return 1;
        }
        char *value_end = t2 - 2;
        t3 = find_in_string(t, t2 - t, content_type, ctlen);
        if (t3) {
            if (t3 != t) {
                return 1;
            }
            t3 += ctlen;

            char *mime_type = t3;
            int mime_type_len = 0;
            while (t3 < t2) {
                t3++;
                mime_type_len++;
                if (!isalnum(*t3) && *t3 != '/' && *t3 != '-')
                    break;
            }
            int ok = 0;
            if (strncmp(mime_type, "image/png", mime_type_len) == 0) {
                if (f->accepted_content_types & UCT_IMAGE_PNG) {
                    f->content_type = UCT_IMAGE_PNG;
                    ok = 1;
                }
            } else if (strncmp(mime_type, "image/jpeg", mime_type_len) == 0) {
                if (f->accepted_content_types & UCT_IMAGE_JPEG) {
                    f->content_type = UCT_IMAGE_JPEG;
                    ok = 1;
                }
            }
            if (!ok) {
#if 0
                fprintf(stderr, "parse_multipart_form_data: Unhandled mime_type: ");
                for (int i = 0; i < mime_type_len; i++)
                    fprintf(stderr, "%c", mime_type[i]);
                fprintf(stderr, ".\n");
#endif
            }

            t = find_in_string(t3, lastbyte - t3, "\r\n", 2);
            if (!t) {
                return 1;
            }
            t += 2;
            /* t on beginning of chunk's third line */
        }

        t = find_in_string(t, lastbyte - t, "\r\n", 2);
        if (!t) {
            return 1;
        }
        t += 2;
        char *value_start = t;

        if (value_start < value_end && (f->accepted_content_types == UCT_NONE || f->content_type != UCT_NONE)) {
            f->value = value_start;
            f->value_len = value_end - value_start;
            f->ok = 1;
        }

        if (value_end + 2 + blen >= lastbyte) {
            break;
        }
    }

    for (int i = 0; i < nff; i++) {
        if (!ff[i].ok && !ff[i].optional) {
            return 1;
        }
    }

    return 0;
}

static int
sanitize(const char *in, const long insize, char *out, const long outsize, const int max_newlines)
{
#define SANITIZE_CASE_REPLACE(a, b)         \
    case (a): {                             \
        int len = sizeof((b)) - 1;          \
        if (outpos + len + 1 >= outsize) {  \
            return 1;                       \
        }                                   \
        memcpy(&out[outpos], (b), len);     \
        inpos++;                            \
        outpos += len;                      \
    } break

    const char lt[] = "&lt;";
    const char gt[] = "&gt;";
    const char amp[] = "&amp;";
    const char quot[] = "&quot;";
    const char apos[] = "&apos;";
    const char br[] = "<br>";

    long inpos = 0;
    long outpos = 0;
    int prev_newlines = 0;

    while (1) {
        char c = in[inpos];

        switch (c) {
            case '\0': {
                return 1;
            } break;

            SANITIZE_CASE_REPLACE('<', lt);
            SANITIZE_CASE_REPLACE('>', gt);
            SANITIZE_CASE_REPLACE('&', amp);
            SANITIZE_CASE_REPLACE('\"', quot);
            SANITIZE_CASE_REPLACE('\'', apos);

            case '\n': {
                if (prev_newlines < max_newlines) {
                    prev_newlines++;
                    int len = sizeof(br) - 1;
                    if (outpos + len + 1 >= outsize) {
                        return 1;
                    }
                    memcpy(&out[outpos], br, len);
                    inpos++;
                    outpos += len;
                } else {
                    inpos++;
                }
            } break;

            default: {
                if ((c >= 32 && c <= 126) || c & (1 << 7)) {
                    out[outpos] = c;
                    inpos++;
                    outpos++;
                } else {
                    inpos++;
                }
            } break;
        }

        if (c != '\n' && c != '\r') {
            prev_newlines = 0;
        }

        if (inpos == insize) {
            break;
        }

        if (outpos + 1 == outsize) {
            return 1;
        }
    }

    out[outpos] = '\0';
    return 0;

#undef SANITIZE_CASE_REPLACE
}

static void
route_catalog(handler_t *h, routeargs_t *args)
{
    template_catalog(h, args->headers_only);
}

static void
route_thread(handler_t *h, routeargs_t *args)
{
    char *s = args->path_rem;
    while (*s) {
        if (!isdigit(*s)) {
            serve_error_404(h);
            return;
        }
        s++;
    }
    long l;
    long *lp = &l;
    parse_long(&lp, args->path_rem);
    if (!lp) {
        serve_error_404(h);
        return;
    }
    template_thread(h, l, args->headers_only);
}

static void
route_report(handler_t *h, routeargs_t *args)
{
    /* Reporting a thread or post deletes it. */

    long post_id = -1;
    for (int i = 0; i < args->np; i++) {
        parameter_t *p = &args->p[i];
        if (strcmp(p->key, "post_id") == 0) {
            post_id = p->val_i;
            break;
        }
    }
    if (post_id == -1) {
        serve_error_500(h);
        return;
    }

    delete_post_or_thread(post_id);

    serve_redirect_303(h, "/");
}

static void
route_post(handler_t *h, routeargs_t *args)
{
    post_t post = {0};
    long thread_id = -1;
    char subject[THREAD_SUBJECT_MAXLEN] = {0};

    char *file = NULL;
    long filesize;

    for (int i = 0; i < args->nff; i++) {
        form_field_t *f = &args->ff[i];

        if (!f->ok)
            continue;
        
        if (strcmp(f->key, "thread_id") == 0) {

            if (f->value_len + 1 > 20) {
                fprintf(stderr, "route_post: thread_id string too large.\n");
                goto invalid;
            }
            char str[20];
            memcpy(str, f->value, f->value_len);
            str[f->value_len] = '\0';
            long *lp = &thread_id;
            parse_long(&lp, str);
            if (!lp) {
                fprintf(stderr, "route_post: Failed to parse thread_id.\n");
                goto invalid;
            }

        } else if (strcmp(f->key, "subject") == 0) {

            if (f->value_len + 1 > THREAD_SUBJECT_MAXLEN) {
                fprintf(stderr, "route_post: Thread subject too large.\n");
                goto invalid;
            }
            int ret = sanitize(f->value, f->value_len, subject, THREAD_SUBJECT_MAXLEN, 0);
            if (ret != 0) {
                fprintf(stderr, "route_post: Failed to sanitize thread subject.\n");
                goto invalid;
            }

        } else if (strcmp(f->key, "name") == 0) {

            if (f->value_len + 1 > POST_NAME_MAXLEN) {
                fprintf(stderr, "route_post: Post name too large.\n");
                goto invalid;
            }
            int ret = sanitize(f->value, f->value_len, post.name, POST_NAME_MAXLEN, 0);
            if (ret != 0) {
                fprintf(stderr, "route_post: Failed to sanitize post name.\n");
                goto invalid;
            }

        } else if (strcmp(f->key, "comment") == 0) {

            if (f->value_len + 1 > POST_COMMENT_MAXLEN) {
                fprintf(stderr, "route_post: Post comment too large.\n");
                goto invalid;
            }
            post.comment = malloc(POST_COMMENT_MAXLEN);
            if (!post.comment) {
                fprintf(stderr, "route_post: malloc() failed.\n");
                exit(1);
            }
            int ret = sanitize(f->value, f->value_len, post.comment, POST_COMMENT_MAXLEN, 2);
            if (ret != 0) {
                fprintf(stderr, "route_post: Failed to sanitize post comment.\n");
                goto invalid;
            }

        } else if (strcmp(f->key, "file") == 0) {

            enum upload_content_type uct = validate_uploaded_file(f->value, f->value_len, f->content_type, POST_FILE_MAXSIZE);
            if (uct == UCT_NONE) {
                fprintf(stderr, "route_post: Failed to validate uploaded file.\n");
                goto invalid;
            }

            char *ext;
            int extlen;
            switch (uct) {
                case UCT_IMAGE_PNG: {
                    ext = ".png";
                    extlen = 4;
                } break;
                case UCT_IMAGE_JPEG: {
                    ext = ".jpg";
                    extlen = 4;
                } break;
                default: {
                    fprintf(stderr, "route_post: Invalid uploaded file type.\n");
                    exit(1);
                } break;
            }

            gen_filename(post.filename, POST_FILENAME_MAXLEN, ext, extlen);
            file = f->value;
            filesize = f->value_len;
        }
    }

    if (thread_id == -1) {
        int ret = thread_create(&post, subject);
        if (ret != 0) {
            fprintf(stderr, "route_post: Failed to create thread.\n");
            goto invalid;
        }
    } else {
        int ret = post_create(thread_id, &post);
        if (ret != 0) {
            fprintf(stderr, "route_post: Failed to create post.\n");
            goto invalid;
        }
    }

    if (file) {
        save_file(file, filesize, post.filename, "uploads");
    }

    char redir[128];
    int nwritten;
    if (thread_id == -1) {
        nwritten = snprintf(redir, 128, SERVER_URL "/thread/%ld", post.post_id);
    } else {
        nwritten = snprintf(redir, 128, SERVER_URL "/thread/%ld#%ld", thread_id, post.post_id);
    }

    if (nwritten + 1 > 128) {
        fprintf(stderr, "route_post: Redirect location buffer too small.\n");
        exit(1);
    }
    serve_redirect_303(h, redir);
    
    return;

invalid:
    if (post.comment) {
        free(post.comment);
    }
    serve_error_400(h);
}

static void
route_uploads(handler_t *h, routeargs_t *args)
{
    char *filename = args->path_rem;
    int len = strlen(filename);
    
    if (len < 5)
        goto err404;

    if (len > 30)
        goto err404;
    
    for (int i = 0; i < len; i++) {
        char c = filename[i];
        if (!isalnum(c) && c != '.') {
            goto err404;
        }
    }
    
    char *mime_type;
    if (strcmp(&filename[len - 4], ".png") == 0) {
        mime_type = "image/png";
    } else if (strcmp(&filename[len - 4], ".jpg") == 0) {
        mime_type = "image/jpeg";
    } else {
        goto err404;
    }

    static const char dir[] = "uploads/";
    char path[128];
    memcpy(path, dir, sizeof(dir) - 1);
    memcpy(&path[sizeof(dir) - 1], filename, len + 1);

    serve_file_from_disk(h, path, mime_type, args->headers_only);

    return;

err404:
    serve_error_404(h);
}

static const parameter_t params_report[] = {
    {
        .key = "post_id",
        .type = PVT_INTEGER,
    }
};

static const form_field_t form_fields_post[] = {
    {
        .key = "thread_id",
        .optional = 1,
    }, {
        .key = "name",
        .optional = 1,
    }, {
        .key = "subject",
        .optional = 1,
    }, {
        .key = "comment",
    }, {
        .key = "file",
        .accepted_content_types = UCT_IMAGE_PNG | UCT_IMAGE_JPEG,
        .optional = 1,
    }
};

#define ROUTE_PARAMS(params)                   \
    .p = params,                               \
    .np = sizeof(params) / sizeof(parameter_t)

#define ROUTE_FORM_FIELDS(form_fields)                \
    .ff = form_fields,                                \
    .nff = sizeof(form_fields) / sizeof(form_field_t)

static const route_t routes[] = {
    {
        .meth = RM_GET,
        .path = "/catalog",
        .fun = route_catalog,
    }, {
        .meth = RM_GET,
        .path = "/thread/",
        .path_wildcard = 1,
        .fun = route_thread,
    }, {
        .meth = RM_GET,
        .path = "/report",
        ROUTE_PARAMS(params_report),
        .fun = route_report,
    }, {
        .meth = RM_POST,
        .path = "/post",
        .max_body_size = 1024 * 1024 * 5,
        ROUTE_FORM_FIELDS(form_fields_post),
        .fun = route_post,
    }, {
        .meth = RM_GET,
        .path = "/uploads/",
        .path_wildcard = 1,
        .fun = route_uploads,
    }, {
        .meth = RM_GET,
        .path = "/",
        .fun = route_catalog,
    }
};

#undef ROUTE_PARAMS
#undef ROUTE_FORM_FIELDS

enum validate_post_request_result
validate_post_request(request_t *req)
{
    if (req->meth != RM_POST)
        return VALIDATE_POST_REQUEST_400;

    int nroutes = sizeof(routes) / sizeof(route_t);

    for (int i = 0; i < nroutes; i++) {
        const route_t *route = &routes[i];

        if (req->meth != route->meth)
            continue;
        
        if (route->path_wildcard) {

            int len = strlen(route->path);
            if (strncmp(req->path, route->path, len) != 0)
                continue;

        } else if (strcmp(req->path, route->path) != 0) {
            continue;
        }

        if (req->content_length > route->max_body_size) {
            fprintf(stderr, "validate_post_request: Request body too big.\n");
            return VALIDATE_POST_REQUEST_400;
        }

        return VALIDATE_POST_REQUEST_OK;
    }
    return VALIDATE_POST_REQUEST_400;
}

void
do_routing(handler_t *h, request_t *req)
{
    routeargs_t args = {0};
    int nroutes = sizeof(routes) / sizeof(route_t);

    int i;
    for (i = 0; i < nroutes; i++) {
        const route_t *route = &routes[i];

        if (req->meth != route->meth) {
            if (req->meth == RM_HEAD && route->meth == RM_GET) {
                args.headers_only = 1;
            } else {
                continue;
            }
        }

        if (route->path_wildcard) {
            int len = strlen(route->path);
            if (strncmp(req->path, route->path, len) != 0)
                continue;
            args.path_rem = req->path + len;
        } else if (strcmp(req->path, route->path) != 0) {
            continue;
        }

        switch (req->meth) {
            case RM_GET: case RM_HEAD: {
                if (route->np > 0) {
                    parameter_t p[route->np];
                    memcpy(p, route->p, sizeof(p));

                    int ret = parse_params(req->params, p, route->np);
                    if (ret != 0) {
                        fprintf(stderr, "do_routing: Invalid params.\n");
                        goto err400;
                    }

                    args.p = p;
                    args.np = route->np;
                    route->fun(h, &args);
                } else {
                    route->fun(h, &args);
                }
            } break;

            case RM_POST: {
                if (route->nff <= 0) {
                    goto err500;
                }
                if (req->body_bufpos != req->content_length) {
                    goto err500;
                }
                if (!*req->boundary) {
                    goto err500;
                }

                form_field_t ff[route->nff];
                memcpy(ff, route->ff, sizeof(ff));

                int ret = parse_mutlipart_form_data(req->body_buf, req->content_length, req->boundary, ff, route->nff);
                if (ret != 0) {
                    fprintf(stderr, "do_routing: Invalid form fields.\n");
                    goto err400;
                }

                args.ff = ff;
                args.nff = route->nff;
                route->fun(h, &args);
            } break;

            default: {
                fprintf(stderr, "do_routing: Invalid request method.\n");
                exit(1);
            } break;
        }
        break;
    }

    if (i == nroutes) {
        serve_error_404(h);
        return;
    }

    if (!h->handler || !h->handler_after) {
        fprintf(stderr, "do_routing: Invalid handler.\n");
        goto err500;
    }

    return;

err500:
    serve_error_500(h);
    return;

err400:
    serve_error_400(h);
    return;
}

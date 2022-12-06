typedef struct {
    char *headers_buf;
    long headers_bufpos;
    long headers_bufs;
    char *body_buf;
    long body_bufpos;
    long body_bufs;
    int free_body;
} handler_args_send_buffer_t;

typedef union {
    handler_args_send_buffer_t send_buffer;
} handler_args_t;

typedef struct {
    int (*handler)(int, handler_args_t *); /* Returns: 1 == continue, 0 == done, -1 == error */
    void (*handler_after)(handler_args_t *);
    handler_args_t args;
    char *resp_headers_buf;
} handler_t;

void serve_file_from_disk(handler_t *h, const char *filename, const char *mime_type, const int headers_only);
void serve_html_file_from_disk(handler_t *h, const char *filename, const int headers_only);
void serve_file_from_buffer(handler_t *h, char *buf, const long bufs, const char *mime_type);
void serve_html_file_from_buffer(handler_t *h, char *buf, const long bufs);

void serve_redirect_303(handler_t *h, char *location);
void serve_error_400(handler_t *h);
void serve_error_404(handler_t *h);
void serve_error_500(handler_t *h);

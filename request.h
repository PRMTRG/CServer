enum request_method {
    RM_GET,
    RM_HEAD,
    RM_POST,
};

enum request_content_type {
    RCT_NONE,
    RCT_MULTIPART_FORMDATA,
};

typedef struct {
    enum request_method meth;
    char *path;
    char *params;
    enum request_content_type ct;
    long content_length;
    char boundary[73];
    char *body_buf;
    long body_bufpos;
} request_t;

enum read_headers_result {
    READ_HEADERS_DONE,
    READ_HEADERS_CONTINUE,
    READ_HEADERS_FAILED_CLOSE_CONNECTION,
    READ_HEADERS_FAILED_SEND_400,
};

enum read_headers_result read_headers(int sock, int *headers_end_state, char *buf, long *bufpos, long bufs, long *headers_len, long *rem_len);
int parse_headers(request_t *req, char *buf, long bufs);

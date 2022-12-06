enum parameter_value_type {
    PVT_STRING,
    PVT_INTEGER,
};

enum validate_post_request_result {
    VALIDATE_POST_REQUEST_OK,
    VALIDATE_POST_REQUEST_400,
};

enum validate_post_request_result validate_post_request(request_t *req);
void do_routing(handler_t *h, request_t *req);

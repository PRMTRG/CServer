#define _POSIX_C_SOURCE 200112L
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>

#include "utils.h"
#include "response.h"
#include "templating.h"
#include "forum.h"
#include "request.h"
#include "config.h"
#include "routing.h"

#define CONNECTION_SLOTS_COUNT 100
#define REQUEST_BUFFER_SIZE 1024 * 8
#define RESPONSE_HEADERS_BUFFER_SIZE 1024 * 8 /* When writing response headers no buffer size checks are performed so don't set this too low. */

enum connection_state {
    CON_CLOSED,
    CON_RECEIVING_HEADERS,
    CON_RECEIVING_BODY,
    CON_SENDING_RESPONSE,
};

typedef struct {
    int sock;
    enum connection_state state;
    int headers_end_state;
    char *buf;
    long bufpos;
    request_t req;
    handler_t h;
} connection_t;

static void
close_connection(connection_t *con, struct pollfd *poll_data, int poll_index, int *active_connections, int *active_slot_with_highest_index)
{
    struct pollfd *poll_slot = &poll_data[poll_index];
    poll_slot->fd = -1;
    poll_slot->events = 0;
    (*active_connections)--;
    if (poll_index == *active_slot_with_highest_index) {
        for (int i = poll_index; i >= 0; i--) {
            if (poll_data[i].fd > 0) {
                *active_slot_with_highest_index = i;
                break;
            }
        }
    }

    close(con->sock);

    memset(con->buf, 0, REQUEST_BUFFER_SIZE);
    char *tmp_conbuf = con->buf;

    memset(con->h.resp_headers_buf, 0, RESPONSE_HEADERS_BUFFER_SIZE);
    char *tmp_respbuf = con->h.resp_headers_buf;

    memset(con, 0, sizeof(connection_t));
    con->buf = tmp_conbuf;
    con->h.resp_headers_buf = tmp_respbuf;
    con->state = CON_CLOSED;
}

static void
free_body_buffer(const char *req_buf, char *body_buf)
{
    if (body_buf < req_buf || body_buf > &req_buf[REQUEST_BUFFER_SIZE - 1]) {
        free(body_buf);
    }
}

static int
accept_connection(int listening_socket)
{
    int sock = accept(listening_socket, NULL, NULL);
    if (sock <= 0) {
        perror("accept");
        exit(1);
    }

    if (fcntl(sock, F_SETFL, O_NONBLOCK | fcntl(sock, F_GETFL, 0)) != 0) {
        perror("fcntl");
        exit(1);
    }

    return sock;
}

static void
handle_connections(int listening_socket)
{
    connection_t cons[CONNECTION_SLOTS_COUNT] = {0};
    struct pollfd poll_data[CONNECTION_SLOTS_COUNT + 1] = {0};
    int active_connections = 0;

    for (int i = 0; i < CONNECTION_SLOTS_COUNT; i++) {
        cons[i].state = CON_CLOSED;
        cons[i].buf = calloc(REQUEST_BUFFER_SIZE, 1);
        if (!cons[i].buf) {
            fprintf(stderr, "handle_connections: calloc() failed.\n");
            exit(1);
        }
        cons[i].h.resp_headers_buf = calloc(RESPONSE_HEADERS_BUFFER_SIZE, 1);
        if (!cons[i].h.resp_headers_buf) {
            fprintf(stderr, "handle_connections: calloc() failed.\n");
            exit(1);
        }
    }

    for (int i = 1; i < CONNECTION_SLOTS_COUNT + 1; i++) {
        poll_data[i].fd = -1;
    }
    poll_data[0].fd = listening_socket;
    poll_data[0].events = POLLIN;
    int active_slot_with_highest_index = 0;

    int nevents;
    while ((nevents = poll(poll_data, active_slot_with_highest_index + 1, -1)) >= 0) {
        if (nevents == 0) {
            continue;
        }

        /* Event on listening socket. */
        if (poll_data[0].revents > 0) {
            nevents--;

            if (poll_data[0].revents != POLLIN) {
                fprintf(stderr, "Unexpected poll event on listening socket: %d\n", poll_data[0].revents);
                exit(1);
            }

            int poll_index = -1;
            if (active_connections < CONNECTION_SLOTS_COUNT) {
                for (int i = 1; i < CONNECTION_SLOTS_COUNT + 1; i++) {
                    if (poll_data[i].fd < 0) {
                        poll_index = i;
                        break;
                    }
                }
            }

            if (poll_index == -1) {
                fprintf(stderr, "Ran out of connection slots.\n");
            } else {
                int sock = accept_connection(listening_socket);

                struct pollfd *poll_slot = &poll_data[poll_index];
                poll_slot->fd = sock;
                poll_slot->events = POLLIN;
                active_connections++;
                if (poll_index > active_slot_with_highest_index) {
                    active_slot_with_highest_index = poll_index;
                }

                connection_t *con = &cons[poll_index - 1];
                con->sock = sock;
                con->state = CON_RECEIVING_HEADERS;
            }
        }

        for (int i = 1; i < CONNECTION_SLOTS_COUNT + 1 && nevents > 0; i++) {
            struct pollfd *poll_slot = &poll_data[i];
            if (poll_slot->fd < 0 || poll_slot->revents == 0) {
                continue;
            }

            nevents--;
            connection_t *con = &cons[i - 1];

            if (poll_slot->revents & (POLLERR | POLLHUP | POLLNVAL)) {
                fprintf(stderr, "Unexpected event on slot %d: %d\n", i, poll_slot->revents);
                close_connection(con, poll_data, i, &active_connections, &active_slot_with_highest_index);
                continue;
            }

            if (con->state == CON_CLOSED) {
                continue;
            }

            if (con->state == CON_RECEIVING_HEADERS) {
                long headers_len;
                long rem_len;

                {
                    enum read_headers_result ret = read_headers(con->sock, &con->headers_end_state, con->buf, &con->bufpos, REQUEST_BUFFER_SIZE, &headers_len, &rem_len);
                    if (ret != READ_HEADERS_DONE) {
                        if (ret == READ_HEADERS_CONTINUE) {
                            continue;
                        } else if (ret == READ_HEADERS_FAILED_CLOSE_CONNECTION) {
                            close_connection(con, poll_data, i, &active_connections, &active_slot_with_highest_index);
                            continue;
                        } else if (ret == READ_HEADERS_FAILED_SEND_400) {
                            serve_error_400(&con->h);
                            con->state = CON_SENDING_RESPONSE;
                            poll_slot->events = POLLOUT;
                            goto sending_response;
                        }
                    }
                }

                int ret = parse_headers(&con->req, con->buf, headers_len);
                if (ret != 0) {
                    serve_error_400(&con->h);
                    con->state = CON_SENDING_RESPONSE;
                    poll_slot->events = POLLOUT;
                    goto sending_response;
                }

                //char *meth = (con->req.meth == RM_GET) ? "GET" : (con->req.meth == RM_POST) ? "POST" : "HEAD";
                //printf("%s %s %s\n", meth, con->req.path, (con->req.params) ? con->req.params : "");

                if (con->req.meth == RM_POST) {

                    int ret = validate_post_request(&con->req);
                    if (ret != VALIDATE_POST_REQUEST_OK) {
                        if (ret == VALIDATE_POST_REQUEST_400) {
                            serve_error_400(&con->h);
                            con->state = CON_SENDING_RESPONSE;
                            poll_slot->events = POLLOUT;
                            goto sending_response;
                        } else {
                            exit(1);
                        }
                    }

                    if (con->req.content_length == rem_len) {
                        con->req.body_buf = &con->buf[headers_len];
                        con->req.body_bufpos = rem_len;

                        do_routing(&con->h, &con->req);
                        con->state = CON_SENDING_RESPONSE;
                        poll_slot->events = POLLOUT;
                    } else {
                        con->req.body_buf = malloc(con->req.content_length);
                        if (!con->req.body_buf) {
                            fprintf(stderr, "handle_connections: malloc() failed.\n");
                        }
                        con->req.body_bufpos = rem_len;
                        memcpy(con->req.body_buf, &con->buf[headers_len], rem_len);

                        con->state = CON_RECEIVING_BODY;
                        poll_slot->events = POLLOUT;
                    }

                } else {
                    do_routing(&con->h, &con->req);
                    con->state = CON_SENDING_RESPONSE;
                    poll_slot->events = POLLOUT;
                }

            }

            if (con->state == CON_RECEIVING_BODY) {
                long nread = read(con->sock, &con->req.body_buf[con->req.body_bufpos], con->req.content_length - con->req.body_bufpos);
                if (nread == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        goto cont;
                    }

                    perror("handle_connections: read()");
                    free_body_buffer(con->buf, con->req.body_buf);
                    close_connection(con, poll_data, i, &active_connections, &active_slot_with_highest_index);
                    goto cont;
                }
                con->req.body_bufpos += nread;
                int done_receiving = con->req.body_bufpos == con->req.content_length;
                if (nread == 0 && !done_receiving) {
                    fprintf(stderr, "handle_connections: Received less bytes than expected.\n");
                    free_body_buffer(con->buf, con->req.body_buf);
                    close_connection(con, poll_data, i, &active_connections, &active_slot_with_highest_index);
                    goto cont;
                }
                if (done_receiving) {
                    do_routing(&con->h, &con->req);
                    free_body_buffer(con->buf, con->req.body_buf);
                    con->state = CON_SENDING_RESPONSE;
                }
            }

            if (con->state == CON_SENDING_RESPONSE) {
sending_response:;
                handler_t *h = &con->h;
                if (!h->handler || !h->handler_after) {
                    fprintf(stderr, "handle_connections: Invalid handler.\n");
                    exit(1);
                }
                int ret = h->handler(con->sock, &h->args);
                if (ret == 0 || ret == -1) {
                    h->handler_after(&h->args);
                    close_connection(con, poll_data, i, &active_connections, &active_slot_with_highest_index);
                }
            }
cont:;
        }
    }
}

static void
run_server(void)
{
    signal(SIGPIPE, SIG_IGN);

    int sock;
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int))) {
        perror("setsockopt");
        exit(1);
    }
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(SERVER_PORT);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        exit(1);
    }
    if (listen(sock, 10) < 0) {
        perror("listen");
        exit(1);
    }

    handle_connections(sock);
}

int
main(int argc, char **argv)
{
    (void)(argc); (void)(argv);

    srand(time(NULL));
    forum_init();
    run_server();
}

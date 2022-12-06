#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

static const char req[] = 
"GET /404 HTTP/1.0\r\n"
"\r\n"
;

static int
connect_to_server(void)
{
    int sock = socket(PF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        exit(1);
    }

    struct sockaddr_in servername = {0};
    servername.sin_family = AF_INET;
    servername.sin_addr.s_addr = inet_addr("127.0.0.1");
    servername.sin_port = htons(5000);

    if (connect(sock, (struct sockaddr *)&servername, sizeof(servername)) < 0) {
        perror("connect");
        exit(1);
    }

    return sock;
}

static void
send_request(int sock)
{
    if (write(sock, req, sizeof(req) - 1) < 0) {
        perror("write");
        exit(1);
    }
}

static void
send_request_with_delay(int sock)
{
    int pos = 0;

    while (pos < (int)sizeof(req) - 1) {
        int nwritten = write(sock, &req[pos], 1);
        if (nwritten < 0) {
            perror("write");
            exit(1);
        }
        pos += nwritten;
        sleep(1);
    }

    printf("send_request done\n");
}

static void
read_response(int sock)
{
    char buf[1024];
    long nread;
    while ((nread = read(sock, buf, sizeof(buf))) > 0);
}

static void *
routine1(void *arg)
{
    (void) arg;

    for (int i = 0; i < 100000; i++) {
        int sock = connect_to_server();

        send_request(sock);
        read_response(sock);

        close(sock);
    }

    return NULL;
}

static void
test1(void)
{
    pthread_t threads[10];
    int nthreads = sizeof(threads) / sizeof(threads[0]);

    for (int i = 0; i < nthreads; i++) {
        pthread_create(&threads[i], NULL, routine1, NULL);
    }

    for (int i = 0; i < nthreads; i++) {
        pthread_join(threads[i], NULL);
    }
}

int
main(int argc, char **argv)
{
    (void) argc; (void) argv;

    test1();
}

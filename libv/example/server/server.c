#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_HOST       "127.0.0.1"
#define DEFAULT_PORT       "4567"
#define DEFAULT_QUEUE_SIZE 128
#define DEFAULT_THREADS    1

typedef struct
{
    volatile sig_atomic_t* stop_requested;
} EchoServerCtx;

static volatile sig_atomic_t g_stop_requested = 0;

static void handle_signal(int signo)
{
    (void)signo;
    g_stop_requested = 1;
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0)
    {
    }
}

static void echo_opened(net_conn* conn, void* udata)
{
    (void)conn;
    (void)udata;
}

static void echo_closed(net_conn* conn, void* udata)
{
    (void)conn;
    (void)udata;
}

static void echo_process(net_conn* conn, const void* data, size_t len, void* udata)
{
    (void)udata;

    char* out = (char*)malloc(len);
    if (!out)
    {
        conn->closed = true;
        return;
    }

    memcpy(out, data, len);
    free(conn->out);
    conn->out = out;
    conn->outlen = len;
    conn->outcap = len;
    conn->outoff = 0;
}

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : DEFAULT_HOST;
    const char* port = argc > 2 ? argv[2] : DEFAULT_PORT;
    int nthreads = argc > 3 ? atoi(argv[3]) : DEFAULT_THREADS;
    if (nthreads <= 0) nthreads = DEFAULT_THREADS;

    install_signal_handlers();

    EchoServerCtx ctx = {
        .stop_requested = &g_stop_requested,
    };

    NetListenOptions opt = {
        .host = host,
        .port = port,
        .nthreads = nthreads,
        .queuesize = DEFAULT_QUEUE_SIZE,
        .udata = &ctx,
        .process = echo_process,
        .opened = echo_opened,
        .closed = echo_closed,
    };

    NetServer* server = NULL;
    if (net_server_start(&opt, &server) != 0)
    {
        fprintf(stderr, "failed to start echo server on %s:%s\n", host, port);
        return EXIT_FAILURE;
    }

    printf("echo server listening on %s:%d with %d worker(s)\n", host, net_server_port(server),
           nthreads);
    fflush(stdout);

    while (!g_stop_requested) sleep_ms(100);

    net_server_stop(server);
    net_server_destroy(server);
    return EXIT_SUCCESS;
}

#define _POSIX_C_SOURCE 200809L

#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int pass_count = 0;
static int fail_count = 0;

#define CHECK(cond, msg)                 \
    do                                   \
    {                                    \
        if (cond)                        \
        {                                \
            pass_count++;                \
            printf("[PASS] %s\n", msg); \
        }                                \
        else                             \
        {                                \
            fail_count++;                \
            printf("[FAIL] %s\n", msg); \
        }                                \
    } while (0)

typedef struct
{
    atomic_int opened;
    atomic_int closed;
    atomic_int processed;
    size_t response_size;
    char response_byte;
} NetTestCtx;

static void sleep_ms(long ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
    {
    }
}

static void on_opened(net_conn* conn, void* udata)
{
    (void)conn;
    NetTestCtx* ctx = (NetTestCtx*)udata;
    atomic_fetch_add(&ctx->opened, 1);
}

static void on_closed(net_conn* conn, void* udata)
{
    (void)conn;
    NetTestCtx* ctx = (NetTestCtx*)udata;
    atomic_fetch_add(&ctx->closed, 1);
}

static void on_process(net_conn* conn, const void* data, size_t len, void* udata)
{
    (void)data;
    (void)len;
    NetTestCtx* ctx = (NetTestCtx*)udata;
    atomic_fetch_add(&ctx->processed, 1);

    size_t n = ctx->response_size ? ctx->response_size : 2;
    char fill = ctx->response_size ? ctx->response_byte : 'o';
    char* out = (char*)malloc(n);
    if (!out)
    {
        conn->closed = true;
        return;
    }

    memset(out, fill, n);
    if (!ctx->response_size && n == 2)
    {
        out[0] = 'o';
        out[1] = 'k';
    }

    conn->out = out;
    conn->outlen = n;
    conn->outcap = n;
}

static int set_io_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0)
        return -1;
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int connect_loop(int port)
{
    for (int i = 0; i < 100; i++)
    {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
            return -1;

        (void)set_io_timeout(fd, 2);

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((uint16_t)port);
        if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1)
        {
            close(fd);
            return -1;
        }

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0)
            return fd;

        close(fd);
        sleep_ms(10);
    }
    return -1;
}

static int write_all(int fd, const void* data, size_t len)
{
    const char* p = (const char*)data;
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = write(fd, p + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int read_exact(int fd, void* data, size_t len)
{
    char* p = (char*)data;
    size_t off = 0;
    while (off < len)
    {
        ssize_t n = read(fd, p + off, len - off);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int start_test_server(NetTestCtx* ctx, int queuesize, NetServer** out)
{
    NetListenOptions opt = {
        .host = "127.0.0.1",
        .port = "0",
        .nthreads = 1,
        .queuesize = queuesize,
        .udata = ctx,
        .process = on_process,
        .opened = on_opened,
        .closed = on_closed,
    };
    return net_server_start(&opt, out);
}

static void test_echo_roundtrip(void)
{
    NetTestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    NetServer* server = NULL;
    CHECK(start_test_server(&ctx, 16, &server) == 0 && server != NULL,
          "net_server_start supports ephemeral port");
    if (!server)
        return;

    int port = net_server_port(server);
    CHECK(port > 0, "net_server_port returns bound port");

    int fd = connect_loop(port);
    CHECK(fd >= 0, "client connects to libv net server");
    if (fd >= 0)
    {
        char buf[2] = {0};
        CHECK(write_all(fd, "ping", 4) == 0, "client writes request");
        CHECK(read_exact(fd, buf, sizeof(buf)) == 0 && memcmp(buf, "ok", 2) == 0,
              "server process callback writes response");
        close(fd);
    }

    net_server_stop(server);
    net_server_destroy(server);
    CHECK(atomic_load(&ctx.opened) >= 1, "opened callback runs");
    CHECK(atomic_load(&ctx.processed) >= 1, "process callback runs");
}

static void test_queue_boundary_burst(void)
{
    enum { NCLIENTS = 16 };
    NetTestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));

    NetServer* server = NULL;
    CHECK(start_test_server(&ctx, 2, &server) == 0 && server != NULL,
          "server starts with tiny queue size");
    if (!server)
        return;

    int port = net_server_port(server);
    int fds[NCLIENTS];
    for (int i = 0; i < NCLIENTS; i++)
        fds[i] = -1;

    int connected = 0;
    for (int i = 0; i < NCLIENTS; i++)
    {
        fds[i] = connect_loop(port);
        if (fds[i] >= 0)
            connected++;
    }
    CHECK(connected == NCLIENTS, "burst clients connect beyond queuesize");

    int echoed = 0;
    for (int i = 0; i < NCLIENTS; i++)
    {
        if (fds[i] < 0)
            continue;
        char buf[2] = {0};
        if (write_all(fds[i], "q", 1) == 0 &&
            read_exact(fds[i], buf, sizeof(buf)) == 0 &&
            memcmp(buf, "ok", 2) == 0)
        {
            echoed++;
        }
        close(fds[i]);
    }

    net_server_stop(server);
    net_server_destroy(server);
    CHECK(echoed == NCLIENTS, "tiny queues handle burst without lost responses");
}

static int child_backpressure_main(void)
{
    NetTestCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.response_size = 64u * 1024u * 1024u;
    ctx.response_byte = 'x';

    NetServer* server = NULL;
    if (start_test_server(&ctx, 16, &server) != 0 || !server)
        return 10;

    int fd = connect_loop(net_server_port(server));
    if (fd < 0)
    {
        net_server_destroy(server);
        return 11;
    }

    if (write_all(fd, "big", 3) != 0)
    {
        close(fd);
        net_server_destroy(server);
        return 12;
    }

    sleep_ms(100);
    net_server_stop(server);
    net_server_destroy(server);
    close(fd);
    return 0;
}

static int wait_child_with_timeout(pid_t pid, long timeout_ms)
{
    long waited = 0;
    while (waited < timeout_ms)
    {
        int status = 0;
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid)
        {
            if (WIFEXITED(status))
                return WEXITSTATUS(status);
            return 128;
        }
        if (got < 0)
            return 129;
        sleep_ms(10);
        waited += 10;
    }

    kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    return 124;
}

static void test_stop_under_write_backpressure(void)
{
    pid_t pid = fork();
    if (pid < 0)
    {
        CHECK(false, "fork succeeds for backpressure isolation");
        return;
    }

    if (pid == 0)
    {
        int rc = child_backpressure_main();
        _exit(rc);
    }

    int rc = wait_child_with_timeout(pid, 3000);
    CHECK(rc == 0, "server stop/destroy returns while client is not reading large response");
}

int main(void)
{
    test_echo_roundtrip();
    test_queue_boundary_burst();
    test_stop_under_write_backpressure();

    printf("\ntest_net: %d passed, %d failed\n", pass_count, fail_count);
    return fail_count > 0 ? 1 : 0;
}

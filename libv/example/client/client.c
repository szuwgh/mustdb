#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "4567"
#define RX_BUF       4096

static int dial(const char* host, const char* port)
{
    struct addrinfo hints = {0};
    struct addrinfo* res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0)
    {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == -1)
        {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd == -1)
    {
        perror("connect");
    }
    return fd;
}

int main(int argc, char** argv)
{
    const char* host = argc > 1 ? argv[1] : DEFAULT_HOST;
    const char* port = argc > 2 ? argv[2] : DEFAULT_PORT;

    int sock = dial(host, port);
    if (sock == -1)
    {
        return EXIT_FAILURE;
    }

    printf("Connected to %s:%s\nType text and press Enter to send. Ctrl+D to quit.\n", host, port);

    char rxbuf[RX_BUF];
    char* line = NULL;
    size_t linecap = 0;

    while (true)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(sock, &rfds);
        int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;
        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("select");
            break;
        }

        if (FD_ISSET(STDIN_FILENO, &rfds))
        {
            ssize_t n = getline(&line, &linecap, stdin);
            if (n == -1)
            {
                printf("EOF on stdin, exiting.\n");
                break;
            }
            ssize_t off = 0;
            while (off < n)
            {
                ssize_t w = send(sock, line + off, n - off, 0);
                if (w < 0)
                {
                    if (errno == EINTR) continue;
                    perror("send");
                    goto done;
                }
                off += w;
            }
        }

        if (FD_ISSET(sock, &rfds))
        {
            ssize_t r = recv(sock, rxbuf, sizeof(rxbuf) - 1, 0);
            if (r <= 0)
            {
                if (r == 0)
                {
                    printf("Server closed the connection.\n");
                }
                else
                {
                    perror("recv");
                }
                break;
            }
            rxbuf[r] = '\0';
            printf("< %s", rxbuf);
            if (rxbuf[r - 1] != '\n')
            {
                printf("\n");
            }
        }
    }

done:
    free(line);
    close(sock);
    return EXIT_SUCCESS;
}
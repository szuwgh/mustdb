#ifndef CONN_H
#define CONN_H
#include <stdio.h>
#include "net.h"

struct buf
{
    char* data;
    size_t len;
    size_t cap;
};

struct Conn
{
    int fd;
    struct buf packet;
    struct net_conn* conn5;
};

void ev_process(struct net_conn* conn, const void* p, size_t len, void* udata);

void ev_opened(struct net_conn* conn, void* udata);

void ev_closed(struct net_conn* conn5, void* udata);

#endif
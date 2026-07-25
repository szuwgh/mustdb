# Echo Server

This example uses `libv`'s `NetServer` API to start a TCP echo server.

## Build

```sh
cd libv/example/server
make
```

## Run

```sh
./server [host] [port] [nthreads]
```

Defaults:

- `host`: `127.0.0.1`
- `port`: `4567`
- `nthreads`: `1`

Stop the server with `Ctrl+C`.

## Test With Client

In another terminal:

```sh
cd libv/example/client
make
./client 127.0.0.1 4567
```

Type text and press Enter. The server echoes the same bytes back.

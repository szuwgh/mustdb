# Simple CLI Client

This tiny interactive client connects to the TCP server implemented in `src/net.c`.

## Build

```sh
cd example/client
make
```

The binary `client` will be created in the same directory. Override compiler options with `make CC=clang CFLAGS='-O2 -g'` if needed.

## Run

```sh
./client [host] [port]
```

- `host` defaults to `127.0.0.1`
- `port` defaults to `4567`

Type a line and press Enter to send it to the server; any data received from the server is printed immediately. Press `Ctrl+D` (EOF) to exit. Ensure your server is already listening before starting the client.
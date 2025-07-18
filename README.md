# Unicaster

UDP Unicasting Server Node Using a Custom Unicaster Module with Redis

The module retrieves the list of Kubernetes pods to get their IP addresses for sending unicast messages.
It is intended to run inside a pod that has access to the Kubernetes service account credentials located at
`/var/run/secrets/kubernetes.io/serviceaccount`. The service account must also be bound to an RBAC role with the
necessary permissions to list pods.

## Build

To build a module use:

```bash
make
```

to clean:

```bash
make clean
```

See all available targets in Makefile (install/uninstall/reload/run etc.).

## Usage

Environment variables available:

- REDIS_BROADCAST_NAME (default is "imq-broker")
- REDIS_BROADCAST_PORT (default is 63000)
- REDIS_BROADCAST_INTERVAL (in seconds, default is 1 second)
- SELECTED_INTERFACES="10,192.168,172.20" (comma-separated IP patters to match when binding to interfaces)

Either configure through redis.conf or by launching the server with `--loadmodule` option, like:

```bash
redis-server --loadmodule /path/to/unicaster.so
```

If you need to log messages and errors from module, enable by:

```bash
redis-server --loadmodule /path/to/unicaster.so --loglevel verbose
```

Message format on redis running is:

```aiignore
[REDIS_BROADCAST_NAME]  [REDIS_GUID]    [STATUS]    [REDIS_INTERFACE_HOST]:[REDIS_PORT]   <REDIS_BROADCAST_INTERVAL>
```

Where STATUS could be one of "up" or "down", and REDIS_BROADCAST_INTERVAL only present if STATUS is "up", e.g:

```aiignore
imq-broker      2cc7c345-3569-44bb-b57a-b72d729d7012    up      127.0.0.1:6380  1
imq-broker      2cc7c345-3569-44bb-b57a-b72d729d7012    up      127.0.0.1:6380  1
imq-broker      2cc7c345-3569-44bb-b57a-b72d729d7012    down    127.0.0.1:6380
```
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
- REDIS_BROADCAST_PORT (the UDP port announcements are sent to, default is 63000)
- REDIS_BROADCAST_INTERVAL (in seconds, default is 1 second)
- REDIS_BROADCAST_TLS (unset by default, see "TLS" below)
- REDIS_BROADCAST_PODS_REFRESH (seconds between two pod list requests, default is 5, see "Pod list" below)
- REDIS_BROADCAST_PODS_TIMEOUT (seconds one pod list request may take in total, default is 5)
- REDIS_BROADCAST_PODS_MAX_AGE (seconds a pod list stays usable when requests fail, unset by default: forever)
- DEPLOYMENT_ENV (the Kubernetes namespace to list pods in, default is the namespace the pod runs in)
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
[REDIS_BROADCAST_NAME]  [REDIS_GUID]    [STATUS]    [REDIS_INTERFACE_HOST]:[REDIS_PORT]   <REDIS_BROADCAST_INTERVAL>  <TRANSPORT>
```

Where STATUS could be one of "up" or "down", and REDIS_BROADCAST_INTERVAL and TRANSPORT only present if STATUS is
"up". TRANSPORT is "tls" when the announced port is the TLS listener and "plain" when it is not — see "TLS" below, e.g:

```aiignore
imq-broker      2cc7c345-3569-44bb-b57a-b72d729d7012    up      127.0.0.1:6380  1   plain
imq-broker      2cc7c345-3569-44bb-b57a-b72d729d7012    up      127.0.0.1:6380  1   plain
imq-broker      2cc7c345-3569-44bb-b57a-b72d729d7012    down    127.0.0.1:6380
```

The fields are positional and only ever appended to, so a reader that splits on tabs and takes the first five sees
exactly what it saw before this field existed.

## Pod list

Announcing and asking Kubernetes who to announce to are two separate jobs. One
background thread asks the API for the pods of the namespace every
`REDIS_BROADCAST_PODS_REFRESH` seconds and keeps the answer; the announcements
go out every `REDIS_BROADCAST_INTERVAL` seconds to whatever list it holds.

Only a successful request replaces the list: an HTTP 200 whose body is a pod
list with at least one pod IP in it. When the API hangs, refuses the request,
throttles it, answers with an error or with something that is not a pod list,
the previous list stays and the broker keeps announcing to it. A service drops
a broker it has not heard from for a few seconds, so an API hiccup used to
disconnect the whole fleet; now it only delays noticing new pods.

- A request is aborted after `REDIS_BROADCAST_PODS_TIMEOUT` seconds, and failed
  requests are retried ever more slowly, up to 30 seconds apart (or
  `REDIS_BROADCAST_PODS_REFRESH`, when that is longer).
- Until the first request succeeds there is no list, and nothing is announced.
- Pods that start while requests are failing are not announced to until a
  request succeeds again.
- Pods that have finished (`Succeeded`, `Failed`) are not announced to: their
  IP may already belong to another pod.
- `REDIS_BROADCAST_PODS_MAX_AGE` stops announcing once the list is that many
  seconds old. It is off by default, because stopping is exactly what makes the
  fleet disconnect; set it when announcing to IPs that may have been reused
  elsewhere is the greater risk.
- Failures are logged at `warning` when they start and once a minute while they
  last, together with the size and age of the list still in use.

Lowering `REDIS_BROADCAST_PODS_REFRESH` makes new pods find the broker sooner,
at the price of one more list request per broker each time.

## TLS

The announced port is the one the server is actually **listening** on. Redis
serves TLS by setting `port 0` and `tls-port <n>`, so a TLS broker used to
announce `<ip>:0` — an address nothing can connect to, and one @imqueue's UDP
listener discards as malformed. Such a fleet discovered no broker at all, with
nothing in any log to say why: the announcement went out, it was just useless.
The module now announces `tls-port` in that case, and marks the datagram `tls`.

When **both** listeners are up, the plaintext port is announced. That is what
an existing fleet is already connecting to, and upgrading this module must not
move it onto a transport its clients are not configured for. Set
`REDIS_BROADCAST_TLS=1` to announce the TLS port instead; `0` pins plaintext.

If the requested listener is not up — `REDIS_BROADCAST_TLS=1` with no
`tls-port`, or a server listening nowhere — **nothing is announced**, and the
reason is logged at `warning`. Announcing a port that refuses connections, or
quietly downgrading a fleet that asked for TLS, are both worse than being
missing.

The marker describes the announced port and nothing else. It carries no
certificate and configures no client: services still supply their own TLS
options (`IMQ_REDIS_TLS*` in `@imqueue/core`).

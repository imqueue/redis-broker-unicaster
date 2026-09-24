/*!
 * Extends native redis module to be promise-like
 *
 * Copyright (c) 2018, imqueue.com <support@imqueue.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ifaddrs.h>
#include <limits.h>
#include <uuid/uuid.h>
#include <curl/curl.h>
#include "redismodule.h"
#include "settings.h"
#include "closing.h"
#include "pod_cache.h"

#define MAX_REDIS_BINDS 16
#define MAX_IP_PATTERNS 16

static int enable_logging = 0;
static int global_redis_port = 6379;
static int global_redis_tls = 0;

static char redis_guid[37];

static char redis_bind_ips[MAX_REDIS_BINDS][INET_ADDRSTRLEN];
static int redis_bind_count = 0;
static int allow_all_interfaces = 0;

static pthread_t *thread_ids = NULL;
static int thread_count = 0;

static char *ip_patterns[MAX_IP_PATTERNS];
static int ip_pattern_count = 0;

void init_ip_patterns() {
    const char *patterns_env = getenv("SELECTED_INTERFACES");

    if (!patterns_env) {
        RedisModule_Log(
            NULL,
            "warning",
            "DEBUG: SELECTED_INTERFACES is not defined"
        );

        return;
    }

    RedisModule_Log(
        NULL,
        "debug",
        "DEBUG: SELECTED_INTERFACES: %s",
        patterns_env
    );

    char *patterns_str = strdup(patterns_env);
    char *pattern = strtok(patterns_str, ",");

    while (pattern && ip_pattern_count < MAX_IP_PATTERNS) {
        // Trim whitespace
        while (*pattern == ' ') {
            pattern++;
        }

        char *end = pattern + strlen(pattern) - 1;

        while (end > pattern && *end == ' ') {
            end--;
        }

        *(end + 1) = '\0';

        RedisModule_Log(
            NULL,
            "debug",
            "DEBUG: pattern found: %s",
            pattern
        );

        if (strlen(pattern) > 0) {
            ip_patterns[ip_pattern_count] = strdup(pattern);
            ip_pattern_count++;
        }

        pattern = strtok(NULL, ",");
    }

    free(patterns_str);
}

void cleanup_ip_patterns() {
    for (int i = 0; i < ip_pattern_count; i++) {
        free(ip_patterns[i]);
    }

    ip_pattern_count = 0;
}

int ip_matches_pattern(const char *ip) {
    // If no patterns defined, accept all IPs
    if (ip_pattern_count == 0) {
        return 1;
    }

    for (int i = 0; i < ip_pattern_count; i++) {
        const int res = strncmp(ip, ip_patterns[i], strlen(ip_patterns[i]));

        RedisModule_Log(
            NULL,
            "debug",
            "DEBUG: ip compare result [ip, pattern, matched]: [%s, %s, %d]",
            ip, ip_patterns[i], res
        );

        if (res == 0) {
            return 1;
        }
    }

    return 0;
}


void generate_redis_guid() {
    uuid_t binuuid;
    uuid_generate(binuuid);
    uuid_unparse_lower(binuuid, redis_guid);
}

void load_redis_bind_ips(RedisModuleCtx *ctx) {
    redis_bind_count = 0;
    allow_all_interfaces = 0;

    RedisModuleCallReply *reply = RedisModule_Call(ctx, "CONFIG", "cc", "GET", "bind");
    if (!reply
        || RedisModule_CallReplyType(reply) != REDISMODULE_REPLY_ARRAY
        || RedisModule_CallReplyLength(reply) != 2
    ) {
        if (reply) {
            RedisModule_FreeCallReply(reply);
        }

        allow_all_interfaces = 1;

        return;
    }

    RedisModuleCallReply *val_reply = RedisModule_CallReplyArrayElement(reply, 1);

    if (val_reply && RedisModule_CallReplyType(val_reply) == REDISMODULE_REPLY_STRING) {
        RedisModuleString *bind_str = RedisModule_CreateStringFromCallReply(val_reply);
        size_t len;
        const char *bind_cstr = RedisModule_StringPtrLen(bind_str, &len);
        char buf[256];

        strncpy(buf, bind_cstr, sizeof(buf));
        buf[sizeof(buf) - 1] = '\0';

        const char *token = strtok(buf, " ");

        while (token && redis_bind_count < MAX_REDIS_BINDS) {
            if (strcmp(token, "0.0.0.0") == 0) {
                allow_all_interfaces = 1;

                break;
            }

            strncpy(redis_bind_ips[redis_bind_count++], token, INET_ADDRSTRLEN);
            token = strtok(NULL, " ");
        }

        RedisModule_FreeString(ctx, bind_str);
    }

    RedisModule_FreeCallReply(reply);
}

/*
 * Reads one integer directive out of the running configuration.
 *
 * Returns `fallback` when the directive is not there at all: `tls-port` does
 * not exist on a Redis built without TLS, and CONFIG GET answers that with an
 * empty array rather than with an error.
 */
int get_config_int(RedisModuleCtx *ctx, const char *name, const int fallback) {
    int value = fallback;
    RedisModuleCallReply *reply = RedisModule_Call(ctx, "CONFIG", "cc", "GET", name);

    if (reply
        && RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ARRAY
        && RedisModule_CallReplyLength(reply) == 2
    ) {
        RedisModuleCallReply *value_reply = RedisModule_CallReplyArrayElement(reply, 1);

        if (value_reply
            && RedisModule_CallReplyType(value_reply) == REDISMODULE_REPLY_STRING
        ) {
            RedisModuleString *value_str = RedisModule_CreateStringFromCallReply(value_reply);
            size_t len;
            const char *value_cstr = RedisModule_StringPtrLen(value_str, &len);

            value = parse_int(value_cstr);

            RedisModule_FreeString(ctx, value_str);
        }
    }

    if (reply) {
        RedisModule_FreeCallReply(reply);
    }

    return value;
}

/*
 * REDIS_BROADCAST_TLS: 1 announces the TLS listener, 0 announces the plaintext
 * one, -1 (unset, or anything unrecognised) announces whichever is up.
 */
int get_tls_preference() {
    const char *env = getenv("REDIS_BROADCAST_TLS");

    if (!env || !*env) {
        return -1;
    }

    if (!strcasecmp(env, "1") || !strcasecmp(env, "yes")
        || !strcasecmp(env, "true") || !strcasecmp(env, "on")
    ) {
        return 1;
    }

    if (!strcasecmp(env, "0") || !strcasecmp(env, "no")
        || !strcasecmp(env, "false") || !strcasecmp(env, "off")
    ) {
        return 0;
    }

    RedisModule_Log(
        NULL,
        "warning",
        "%s: REDIS_BROADCAST_TLS='%s' is not one of 1/0, yes/no, true/false,"
        " on/off - ignored, the listening port decides",
        get_service_name(),
        env
    );

    return -1;
}

/*
 * The port a client can actually reach this broker on, and whether reaching it
 * means TLS.
 *
 * `port 0` does not mean "no port": it is how Redis is told to stop listening
 * in plaintext, and a TLS-only broker is configured in exactly that way. This
 * announcement has always carried `port` verbatim, so such a broker announced
 * `<ip>:0` - an address nothing can connect to, and one that @imqueue's UDP
 * listener drops as malformed. The fleet then saw no broker at all, and no
 * error anywhere said why: the announcement did go out, it was just useless.
 *
 * So the announced port is the one that is listening. When BOTH are listening
 * plaintext wins, because that is what every existing deployment already
 * announces, and an image upgrade must not move a fleet onto a transport its
 * clients are not configured for. REDIS_BROADCAST_TLS=1 is how that move is
 * made deliberately.
 *
 * @param ctx - module context, used to read the configuration
 * @param is_tls - set to 1 when the returned port is the TLS listener
 * @returns the port to announce, or 0 when there is nothing worth announcing
 */
int resolve_announced_port(RedisModuleCtx *ctx, int *is_tls) {
    const int port = get_config_int(ctx, "port", 0);
    const int tls_port = get_config_int(ctx, "tls-port", 0);
    const int prefer_tls = get_tls_preference();

    *is_tls = 0;

    if (prefer_tls == 1) {
        if (tls_port > 0) {
            *is_tls = 1;

            return tls_port;
        }

        // announcing the plaintext port instead would silently undo an explicit
        // request for TLS, which is the one outcome worse than not being found
        RedisModule_Log(
            NULL,
            "warning",
            "%s: REDIS_BROADCAST_TLS asks for the TLS listener, but `tls-port`"
            " is 0 or unsupported by this build",
            get_service_name()
        );

        return 0;
    }

    if (prefer_tls == 0) {
        if (port > 0) {
            return port;
        }

        RedisModule_Log(
            NULL,
            "warning",
            "%s: REDIS_BROADCAST_TLS asks for the plaintext listener, but"
            " `port` is 0",
            get_service_name()
        );

        return 0;
    }

    if (port > 0) {
        return port;
    }

    if (tls_port > 0) {
        *is_tls = 1;

        return tls_port;
    }

    return 0;
}

typedef struct {
    char source_ip[INET_ADDRSTRLEN];
    int redis_port;
    int redis_tls;
} BroadcastTask;

/*
 * @returns 0 when the datagram was handed to the kernel, -1 with errno set
 * when it was not
 */
int send_unicast_message(const char *ip, const int port, const char *message) {
    if (!ip || !message) {
        errno = EINVAL;

        return -1;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        errno = EINVAL;

        return -1;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        return -1;
    }

    const ssize_t sent = sendto(
        sock,
        message,
        strlen(message),
        0,
        (struct sockaddr *)&dest,
        sizeof(dest)
    );
    const int send_errno = errno;

    close(sock);

    if (sent < 0) {
        errno = send_errno;

        return -1;
    }

    return 0;
}

void* unicast_thread(void* arg) {
    BroadcastTask* task = arg;
    const char* broadcast_name = get_service_name();
    const int broadcast_interval = get_interval();
    char up_message[256];
    char down_message[256];

    snprintf(
        down_message,
        sizeof(down_message),
        "%s\t%s\tdown\t%s:%d",
        broadcast_name,
        redis_guid,
        task->source_ip,
        task->redis_port
    );
    // the transport is the SIXTH field, after the interval, and only on `up`.
    // A reader that splits on tabs and takes the first five gets exactly what it
    // got before, and `down` keeps the four fields it has always had - where a
    // fifth would land in the interval's slot and be read as a timeout of NaN,
    // which drops the whole datagram.
    snprintf(
        up_message,
        sizeof(up_message),
        "%s\t%s\tup\t%s:%d\t%d\t%s",
        broadcast_name,
        redis_guid,
        task->source_ip,
        task->redis_port,
        broadcast_interval,
        task->redis_tls ? "tls" : "plain"
    );

    const int destination_port = get_port();
    int was_failing = 0;

    // the pod list comes from pod_cache, which keeps the last one the API
    // returned; a slow or failing API delays its refresh, never this loop
    while (1) {
        const int closing = is_closing();
        const char *message = closing ? down_message : up_message;
        const PodSnapshot *snapshot = pod_cache_acquire();
        int failed = 0;
        int last_errno = 0;
        const char *last_failed_ip = NULL;

        for (int i = 0; snapshot && i < snapshot->pods->count; i++) {
            const char *ip = snapshot->pods->ips[i];

            if (send_unicast_message(ip, destination_port, message) < 0) {
                failed++;
                last_errno = errno;
                last_failed_ip = ip;
            } else if (enable_logging) {
                RedisModule_Log(
                    NULL,
                    "notice",
                    "%s: UDP unicast from %s to %s:%d: %s",
                    broadcast_name,
                    task->source_ip,
                    ip,
                    destination_port,
                    message
                );
            }
        }

        // said once when sends start failing and once when they stop, not
        // once per pod per interval
        if (failed && !was_failing) {
            RedisModule_Log(
                NULL,
                "warning",
                "%s: %d of %d announcements from %s failed, e.g. to %s:%d: %s",
                broadcast_name,
                failed,
                snapshot->pods->count,
                task->source_ip,
                last_failed_ip,
                destination_port,
                strerror(last_errno)
            );
        } else if (!failed && was_failing && snapshot) {
            RedisModule_Log(
                NULL,
                "notice",
                "%s: announcements from %s are delivered again",
                broadcast_name,
                task->source_ip
            );
        }

        if (snapshot) {
            was_failing = failed > 0;
        }

        pod_cache_release(snapshot);

        if (closing) {
            break;
        }

        closing_sleep(broadcast_interval);
    }

    free(task);

    return NULL;
}

void send_udp_message(const int redis_port, const int redis_tls) {
    struct ifaddrs *ifaddr;
    int max_threads = 0;

    thread_count = 0;

    if (getifaddrs(&ifaddr) == -1) {
        RedisModule_Log(NULL, "warning", "%s: getifaddrs failed: %s", get_service_name(), strerror(errno));

        return;
    }

    // counted in the same list the loop below walks: a second getifaddrs()
    // could see an interface the first did not, and overrun the array
    for (const struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET) {
            max_threads++;
        }
    }

    if (!max_threads) {
        RedisModule_Log(NULL, "notice", "%s: no network interfaces found", get_service_name());
        freeifaddrs(ifaddr);

        return;
    }

    thread_ids = malloc(sizeof(pthread_t) * max_threads);

    if (!thread_ids) {
        RedisModule_Log(NULL, "warning", "%s: failed to allocate thread array", get_service_name());
        freeifaddrs(ifaddr);

        return;
    }

    // Initialize IP patterns
    init_ip_patterns();

    for (const struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        const struct sockaddr_in *addr_in = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];

        inet_ntop(AF_INET, &addr_in->sin_addr, ip, sizeof(ip));

        // Skip if IP doesn't match any of our patterns
        if (!ip_matches_pattern(ip)) {
            RedisModule_Log(
                NULL,
                "debug",
                "DEBUG: send_udp_message: %s: skipping interface with IP %s (doesn't match patterns)",
                get_service_name(), ip
            );

            continue;
        } else {
            RedisModule_Log(
                NULL,
                "debug",
                "DEBUG: send_udp_message: %s: using interface with IP %s (as it matches patterns)",
                get_service_name(), ip
            );
        }

        BroadcastTask *task = malloc(sizeof(BroadcastTask));

        if (!task) {
            continue;
        }

        snprintf(task->source_ip, sizeof(task->source_ip), "%s", ip);
        task->redis_port = redis_port;
        task->redis_tls = redis_tls;

        pthread_t tid;

        if (pthread_create(&tid, NULL, unicast_thread, task) == 0) {
            thread_ids[thread_count++] = tid;
        } else {
            free(task);
        }
    }

    if (thread_count == 0 && ip_pattern_count > 0) {
        RedisModule_Log(
            NULL,
            "warning",
            "DEBUG: %s: no interfaces matched the specified IP patterns",
            get_service_name()
        );
    }

    freeifaddrs(ifaddr);
}

/*
 * Stops every thread and waits for it. The senders wake at once, announce
 * `down` to the pods they already know and exit; a pod list request still in
 * flight is aborted rather than waited out.
 */
void cleanup_threads() {
    closing_begin();

    for (int i = 0; i < thread_count; i++) {
        pthread_join(thread_ids[i], NULL);
    }

    free(thread_ids);
    thread_ids = NULL;
    thread_count = 0;

    pod_cache_stop();
    cleanup_ip_patterns();
}

// set by the first cron tick after load, reset in RedisModule_OnLoad() so a
// reloaded module announces again
static int announced = 0;

/*
 * Starts the broadcaster threads.
 *
 * Called from the startup cron hook rather than from RedisModule_OnLoad(),
 * because modules are loaded before the listeners exist: announcing there
 * advertised an address that still refused connections, and a client that
 * dialled the address it learned that way was refused.
 */
void start_broadcasting(RedisModuleCtx *ctx) {
    RedisModule_Log(
        ctx,
        "notice",
        "%s: announcing port %d (%s)",
        get_service_name(),
        global_redis_port,
        global_redis_tls ? "tls" : "plain"
    );

    if (!pod_cache_start()) {
        RedisModule_Log(
            ctx,
            "warning",
            "%s: could not start the pod list refresh, this broker stays invisible",
            get_service_name()
        );

        return;
    }

    send_udp_message(global_redis_port, global_redis_tls);
}

/*
 * Fires on every server cron; announces on the first one after load.
 *
 * Redis enters its event loop only after initListeners() and loadDataFromDisk(),
 * so the first announcement cannot precede the listener.
 *
 * The hook stays subscribed for the life of the module. Unsubscribing from
 * inside the callback frees the listener that moduleFireServerEvent() still
 * dereferences after the callback returns (el->module->in_hook--), a
 * use-after-free on every redis from 7.2 to unstable; redis drops the
 * subscription itself on unload. A flag check per cron tick is the cost.
 */
void cron_broadcast_once(
    RedisModuleCtx *ctx,
    RedisModuleEvent e,
    uint64_t subevent,
    void *data
) {
    (void)e;
    (void)subevent;
    (void)data;

    // a shutdown that wins the race must not start broadcaster threads
    if (announced || is_closing()) {
        return;
    }

    announced = 1;
    start_broadcasting(ctx);
}

void shutdown_callback(
    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    RedisModuleCtx *ctx,
    // ReSharper disable once CppParameterMayBeConst
    RedisModuleEvent e,
    // ReSharper disable once CppParameterMayBeConst
    uint64_t subevent,
    // ReSharper disable once CppParameterMayBeConstPtrOrRef
    void *data
) {
    (void)ctx;
    (void)e;
    (void)subevent;
    (void)data;

    // waits for the `down` announcements instead of hoping a fixed sleep is
    // long enough for them; the senders are woken rather than left to finish
    // their interval, which could outlast the whole shutdown
    cleanup_threads();
}

// Redis Module initialization
int RedisModule_OnLoad(RedisModuleCtx *ctx) {
    generate_redis_guid();

    if (RedisModule_Init(ctx, "unicaster", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        cleanup_threads();

        return REDISMODULE_ERR;
    }

    closing_reset();
    // a reloaded module must announce again; on musl the DSO is never unloaded,
    // so file-scope state survives MODULE UNLOAD / LOAD unless reset here
    announced = 0;
    load_redis_bind_ips(ctx);

    // done here, while the module has no threads: left to the first
    // curl_easy_init(), it runs in whichever thread gets there first, and
    // libcurl's global setup is not thread-safe
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        RedisModule_Log(ctx, "warning", "%s: curl_global_init failed", get_service_name());

        return REDISMODULE_ERR;
    }

    RedisModule_SubscribeToServerEvent(ctx, RedisModuleEvent_Shutdown, shutdown_callback);

    RedisModuleCallReply *loglevel_reply = RedisModule_Call(ctx, "CONFIG", "cc", "GET", "loglevel");

    if (loglevel_reply &&
        RedisModule_CallReplyType(loglevel_reply) == REDISMODULE_REPLY_ARRAY &&
        RedisModule_CallReplyLength(loglevel_reply) == 2
    ) {
        RedisModuleCallReply *level = RedisModule_CallReplyArrayElement(loglevel_reply, 1);

        if (level &&
            RedisModule_CallReplyType(level) == REDISMODULE_REPLY_STRING
        ) {
            RedisModuleString *level_str = RedisModule_CreateStringFromCallReply(level);
            size_t len;
            const char *level_cstr = RedisModule_StringPtrLen(level_str, &len);

            if (strcmp(level_cstr, "verbose") == 0 || strcmp(level_cstr, "debug") == 0) {
                enable_logging = 1;
            }

            RedisModule_FreeString(ctx, level_str);
        }
    }

    if (loglevel_reply) {
        RedisModule_FreeCallReply(loglevel_reply);
    }

    global_redis_port = resolve_announced_port(ctx, &global_redis_tls);

    if (global_redis_port <= 0) {
        // a broker that announces nothing is invisible to the whole fleet, so
        // this is a warning and it is the last word on the subject - the reason
        // was logged by resolve_announced_port
        RedisModule_Log(
            ctx,
            "warning",
            "%s: no reachable listener to announce, this broker stays invisible",
            get_service_name()
        );

        return REDISMODULE_OK;
    }

    if (RedisModule_SubscribeToServerEvent(
            ctx, RedisModuleEvent_CronLoop, cron_broadcast_once) != REDISMODULE_OK) {
        // without the hook nothing ever announces, which is the one failure
        // this module must never keep quiet about
        RedisModule_Log(
            ctx,
            "warning",
            "%s: could not register the startup hook, this broker stays invisible",
            get_service_name()
        );
    }

    return REDISMODULE_OK;
}

/*
 * Redis calls this as int (*)(RedisModuleCtx *) and refuses the unload when it
 * returns REDISMODULE_ERR, so the signature has to match or the answer is
 * whatever happens to be in the return register.
 */
int RedisModule_OnUnload(RedisModuleCtx *ctx) {
    (void)ctx;

    cleanup_threads();
    curl_global_cleanup();

    return REDISMODULE_OK;
}

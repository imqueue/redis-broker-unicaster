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
#include <json-c/json.h>
#include "redismodule.h"

#define DEFAULT_NAME "imq-broker"
#define DEFAULT_PORT 63000
#define DEFAULT_INTERVAL 1
#define MAX_REDIS_BINDS 16
#define MAX_PODS 10000
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
static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

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

char *get_service_name() {
    char *service_name = getenv("REDIS_BROADCAST_NAME");

    if (!service_name) {
        service_name = DEFAULT_NAME;
    }

    return service_name;
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

int parse_int(const char *buff) {
    char *end;

    errno = 0;

    const long sl = strtol(buff, &end, 10);

    if (end == buff
        || '\0' != *end
        || ((LONG_MIN == sl || LONG_MAX == sl) && ERANGE == errno)
        || sl > INT_MAX
        || sl < INT_MIN
    ) {
        return 0;
    }

    return (int)sl;
}

int get_port() {
    const char *env = getenv("REDIS_BROADCAST_PORT");

    if (env) {
        const int port = parse_int(env);

        if (port > 0 && port <= 65535) {
            return port;
        }
    }

    return DEFAULT_PORT;
}

int get_interval() {
    const char *env = getenv("REDIS_BROADCAST_INTERVAL");

    if (env) {
        const int interval = parse_int(env);

        if (interval > 0) {
            return interval;
        }
    }

    return DEFAULT_INTERVAL;
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

int is_closing = 0;

typedef struct {
    char source_ip[INET_ADDRSTRLEN];
    int redis_port;
    int redis_tls;
} BroadcastTask;

struct MemoryStruct {
    char *memory;
    size_t size;
};

static size_t WriteMemoryCallback(const void *contents, const size_t size, const size_t nmemb, void *userp) {
    const size_t realsize = size * nmemb;
    struct MemoryStruct *mem = userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);

    if (!ptr) {
        return 0;
    }

    mem->memory = ptr;
    memcpy(&mem->memory[mem->size], contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

char **fetch_pod_ips(int *pod_count) {
    struct MemoryStruct chunk;
    char **pod_ips = calloc(MAX_PODS, sizeof(char*));  // Use calloc to initialize to NULL
    *pod_count = 0;

    if (!pod_ips) {
        return NULL;
    }

    chunk.memory = malloc(1);
    chunk.size = 0;

    if (!chunk.memory) {
        free(pod_ips);

        return NULL;
    }

    CURL* curl = curl_easy_init();

    if (!curl) {
        free(chunk.memory);
        free(pod_ips);

        return NULL;
    }

    FILE *token_file = fopen("/var/run/secrets/kubernetes.io/serviceaccount/token", "r");

    if (!token_file) {
        curl_easy_cleanup(curl);
        free(chunk.memory);
        free(pod_ips);

        return NULL;
    }

    char token[4096];

    if (fgets(token, sizeof(token), token_file) == NULL) {
        fclose(token_file);
        curl_easy_cleanup(curl);
        free(chunk.memory);

        return NULL;
    }

    fclose(token_file);

    char auth_header[4128];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, auth_header);

    const char *deployment_env = getenv("DEPLOYMENT_ENV");
    char url[256];
    snprintf(url, sizeof(url), "https://kubernetes.default.svc/api/v1/namespaces/%s/pods",
             deployment_env ? deployment_env : "");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO, "/var/run/secrets/kubernetes.io/serviceaccount/ca.crt");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteMemoryCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

    const CURLcode res = curl_easy_perform(curl);

    if (res == CURLE_OK) {
        json_object *parsed_json = json_tokener_parse(chunk.memory);
        json_object *items;

        if (parsed_json) {
            if (json_object_object_get_ex(parsed_json, "items", &items)) {
                const int n_items = (int)json_object_array_length(items);

                for (int i = 0; i < n_items && *pod_count < MAX_PODS; i++) {
                    const json_object *item = json_object_array_get_idx(items, i);
                    json_object *status;
                    json_object *pod_ip;

                    if (json_object_object_get_ex(item, "status", &status) &&
                        json_object_object_get_ex(status, "podIP", &pod_ip)) {
                        const char *ip = json_object_get_string(pod_ip);

                        pod_ips[*pod_count] = strdup(ip);
                        (*pod_count)++;
                    }
                }
            }

            json_object_put(parsed_json);
        }
    }

    if (res != CURLE_OK) {
        for (int i = 0; i < *pod_count; i++) {
            free(pod_ips[i]);
        }

        free(pod_ips);
        pod_ips = NULL;
        *pod_count = 0;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(chunk.memory);

    return pod_ips;
}

int send_unicast_message(const char *ip, const int port, const char *message) {
    if (!ip || !message) {
        return 0;
    }

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);

    if (sock < 0) {
        return 0;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;

    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        close(sock);

        return 0;
    }

    dest.sin_port = htons(port);
    sendto(sock, message, strlen(message), 0, (struct sockaddr *)&dest, sizeof(dest));
    close(sock);

    return 1;
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

    while (1) {
        int pod_count;
        char** pod_ips = fetch_pod_ips(&pod_count);

        if (pod_ips) {
            for (int i = 0; i < pod_count; i++) {
                const char* message = is_closing ? down_message : up_message;

                if (send_unicast_message(pod_ips[i], DEFAULT_PORT, message) < 0) {
                    RedisModule_Log(
                        NULL,
                        "warning",
                        "%s: broadcast to %s failed: %s",
                        broadcast_name,
                        task->source_ip,
                        strerror(errno)
                    );
                } else if (enable_logging) {
                    RedisModule_Log(
                        NULL,
                        "notice",
                        "%s: UDP Broadcast from %s: %s",
                        broadcast_name,
                        task->source_ip,
                        message
                    );
                }

                free(pod_ips[i]);
            }
        }

        if (is_closing) {
            break;
        }

        free(pod_ips);
        sleep(broadcast_interval);
    }

    free(task);

    return NULL;
}

int count_usable_interfaces() {
    struct ifaddrs *ifaddr;
    int count = 0;

    if (getifaddrs(&ifaddr) == -1) {
        return 0;
    }

    for (const struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        ++count;
    }

    freeifaddrs(ifaddr);

    return count;
}

void send_udp_message(const int redis_port, const int redis_tls) {
    struct ifaddrs *ifaddr;
    const int max_threads = ip_pattern_count > 0 && ip_pattern_count < count_usable_interfaces()
        ? ip_pattern_count
        : count_usable_interfaces()
    ;

    if (!max_threads) {
        RedisModule_Log(NULL, "notice", "%s: no network interfaces found", get_service_name());

        return;
    }

    thread_ids = malloc(sizeof(pthread_t) * max_threads);

    if (!thread_ids) {
        RedisModule_Log(NULL, "error", "%s: failed to allocate thread array", get_service_name());

        return;
    }

    thread_count = 0;

    if (getifaddrs(&ifaddr) == -1) {
        free(thread_ids);
        thread_ids = NULL;
        RedisModule_Log(NULL, "error", "%s: getifaddrs failed: %s", get_service_name(), strerror(errno));

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
        strncpy(task->source_ip, ip, sizeof(task->source_ip));
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

void cleanup_threads() {
    if (!thread_ids) return;

    pthread_mutex_lock(&thread_mutex);
    is_closing = 1;
    pthread_mutex_unlock(&thread_mutex);

    for (int i = 0; i < thread_count; i++) {
        pthread_join(thread_ids[i], NULL);
    }

    free(thread_ids);
    thread_ids = NULL;
    thread_count = 0;

    cleanup_ip_patterns();
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

    is_closing = 1;
    sleep(1);
}

// Redis Module initialization
int RedisModule_OnLoad(RedisModuleCtx *ctx) {
    generate_redis_guid();

    if (RedisModule_Init(ctx, "unicaster", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        cleanup_threads();

        return REDISMODULE_ERR;
    }

    is_closing = 0;
    load_redis_bind_ips(ctx);
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

    RedisModule_Log(
        ctx,
        "notice",
        "%s: announcing port %d (%s)",
        get_service_name(),
        global_redis_port,
        global_redis_tls ? "tls" : "plain"
    );

    send_udp_message(global_redis_port, global_redis_tls);

    return REDISMODULE_OK;
}

void RedisModule_OnUnload() {
    cleanup_threads();
}

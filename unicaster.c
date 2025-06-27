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
#define MAX_REDIS_BINDS 8
#define MAX_PODS 10000

static int enable_logging = 0;
static int global_redis_port = 6379;

static char redis_guid[37];

static char redis_bind_ips[MAX_REDIS_BINDS][INET_ADDRSTRLEN];
static int redis_bind_count = 0;
static int allow_all_interfaces = 0;

static pthread_t *thread_ids = NULL;
static int thread_count = 0;
static pthread_mutex_t thread_mutex = PTHREAD_MUTEX_INITIALIZER;

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

int is_closing = 0;

typedef struct {
    char source_ip[INET_ADDRSTRLEN];
    int redis_port;
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

    curl_easy_setopt(curl, CURLOPT_URL, "https://kubernetes.default.svc/api/v1/namespaces/development/pods");
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
    snprintf(
        up_message,
        sizeof(up_message),
        "%s\t%s\tup\t%s:%d\t%d",
        broadcast_name,
        redis_guid,
        task->source_ip,
        task->redis_port,
        broadcast_interval
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

void send_udp_message(const int redis_port) {
    struct ifaddrs *ifaddr;
    const int max_threads = count_usable_interfaces();

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

    for (const struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }

        const struct sockaddr_in *addr_in = (struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr_in->sin_addr, ip, sizeof(ip));

        BroadcastTask *task = malloc(sizeof(BroadcastTask));
        strncpy(task->source_ip, ip, sizeof(task->source_ip));
        task->redis_port = redis_port;

        pthread_t tid;

        if (pthread_create(&tid, NULL, unicast_thread, task) == 0) {
            thread_ids[thread_count++] = tid;
        } else {
            free(task);
        }
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

    RedisModuleCallReply *reply = RedisModule_Call(ctx, "CONFIG", "cc", "GET", "port");
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

    if (reply &&
        RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ARRAY &&
        RedisModule_CallReplyLength(reply) == 2
    ) {
        RedisModuleCallReply *port_reply = RedisModule_CallReplyArrayElement(reply, 1);

        if (port_reply &&
            RedisModule_CallReplyType(port_reply) == REDISMODULE_REPLY_STRING
        ) {
            RedisModuleString *port_str = RedisModule_CreateStringFromCallReply(port_reply);
            size_t len;
            const char *port_cstr = RedisModule_StringPtrLen(port_str, &len);

            global_redis_port = parse_int(port_cstr);

            RedisModule_FreeString(ctx, port_str);
        }
    }

    if (reply) {
        RedisModule_FreeCallReply(reply);
    }

    send_udp_message(global_redis_port);

    return REDISMODULE_OK;
}

void RedisModule_OnUnload() {
    cleanup_threads();
}

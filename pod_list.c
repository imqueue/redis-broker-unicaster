/*!
 * One request for the IPs of the pods in this broker's namespace
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
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <json-c/json.h>
#include "closing.h"
#include "pod_list.h"

#define SERVICE_ACCOUNT_DIR "/var/run/secrets/kubernetes.io/serviceaccount"
#define MAX_TOKEN_SIZE 8192
#define MAX_NAMESPACE_SIZE 64

/*
 * resourceVersion=0 lets the API server answer from its watch cache instead of
 * reading etcd, which is what keeps a request every few seconds per broker
 * cheap. Pods that have finished hold on to an IP the cluster may already have
 * handed to somebody else, so they are not asked for at all.
 */
#define PODS_QUERY "?resourceVersion=0" \
    "&fieldSelector=status.phase%21%3DSucceeded%2Cstatus.phase%21%3DFailed"

struct PodListClient {
    CURL *curl;
};

typedef struct {
    char *data;
    size_t size;
    size_t capacity;
} ResponseBody;

static size_t write_body(
    const void *contents,
    const size_t size,
    const size_t nmemb,
    void *userp
) {
    const size_t chunk = size * nmemb;
    ResponseBody *body = userp;

    if (body->size + chunk + 1 > body->capacity) {
        size_t capacity = body->capacity ? body->capacity : 16384;

        while (body->size + chunk + 1 > capacity) {
            capacity *= 2;
        }

        char *data = realloc(body->data, capacity);

        if (!data) {
            return 0;
        }

        body->data = data;
        body->capacity = capacity;
    }

    memcpy(body->data + body->size, contents, chunk);
    body->size += chunk;
    body->data[body->size] = '\0';

    return chunk;
}

// makes curl abort the transfer as soon as the module starts closing
static int abort_when_closing(
    void *clientp,
    const curl_off_t dltotal,
    const curl_off_t dlnow,
    const curl_off_t ultotal,
    const curl_off_t ulnow
) {
    (void)clientp;
    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    return is_closing();
}

/*
 * Reads a whole small file with surrounding whitespace removed: a token with
 * its trailing newline still attached turns into a broken HTTP header.
 */
static int read_trimmed(const char *path, char *buf, const size_t size) {
    FILE *file = fopen(path, "r");

    if (!file) {
        return 0;
    }

    size_t len = fread(buf, 1, size, file);

    fclose(file);

    // a full buffer means the file did not fit, and a cut token is useless
    if (len == size) {
        return 0;
    }

    buf[len] = '\0';

    while (len && strchr(" \t\r\n", buf[len - 1])) {
        buf[--len] = '\0';
    }

    const size_t lead = strspn(buf, " \t\r\n");

    memmove(buf, buf + lead, len - lead + 1);

    return buf[0] != '\0';
}

// a namespace is an RFC 1123 label, which also makes it safe inside the URL
static int is_valid_namespace(const char *name) {
    const size_t len = strlen(name);

    if (len == 0 || len >= MAX_NAMESPACE_SIZE) {
        return 0;
    }

    return strspn(name, "abcdefghijklmnopqrstuvwxyz0123456789-") == len
        && name[0] != '-'
        && name[len - 1] != '-';
}

/*
 * DEPLOYMENT_ENV when set, otherwise the namespace the pod itself runs in.
 * Without that fallback an unset DEPLOYMENT_ENV requested `/namespaces//pods`,
 * which can never succeed.
 */
static int resolve_namespace(char *buf, const size_t size, char *error, const size_t error_size) {
    const char *env = getenv("DEPLOYMENT_ENV");

    if (env && *env) {
        snprintf(buf, size, "%s", env);
    } else if (!read_trimmed(SERVICE_ACCOUNT_DIR "/namespace", buf, size)) {
        snprintf(
            error,
            error_size,
            "DEPLOYMENT_ENV is not set and %s/namespace is not readable",
            SERVICE_ACCOUNT_DIR
        );

        return 0;
    }

    if (!is_valid_namespace(buf)) {
        snprintf(error, error_size, "'%s' is not a valid namespace name", buf);

        return 0;
    }

    return 1;
}

static int compare_ips(const void *a, const void *b) {
    return strcmp(a, b);
}

// is this pod finished? the field selector already says no, this re-checks it
static int is_finished(json_object *status) {
    json_object *phase;

    if (!json_object_object_get_ex(status, "phase", &phase)) {
        return 0;
    }

    const char *value = json_object_get_string(phase);

    return value && (!strcmp(value, "Succeeded") || !strcmp(value, "Failed"));
}

static PodList *parse_pod_list(const char *body, char *error, const size_t error_size) {
    json_object *root = json_tokener_parse(body);
    json_object *items;

    if (!root) {
        snprintf(error, error_size, "the response is not valid JSON");

        return NULL;
    }

    if (!json_object_object_get_ex(root, "items", &items)
        || !json_object_is_type(items, json_type_array)
    ) {
        snprintf(error, error_size, "the response has no `items` array");
        json_object_put(root);

        return NULL;
    }

    size_t n_items = json_object_array_length(items);

    if (n_items > MAX_PODS) {
        n_items = MAX_PODS;
    }

    PodList *list = malloc(sizeof(PodList) + n_items * INET_ADDRSTRLEN);

    if (!list) {
        snprintf(error, error_size, "out of memory");
        json_object_put(root);

        return NULL;
    }

    list->count = 0;

    for (size_t i = 0; i < n_items; i++) {
        json_object *item = json_object_array_get_idx(items, i);
        json_object *status;
        json_object *pod_ip;
        struct in_addr addr;

        if (!json_object_object_get_ex(item, "status", &status)
            || is_finished(status)
            || !json_object_object_get_ex(status, "podIP", &pod_ip)
            || !json_object_is_type(pod_ip, json_type_string)
            || inet_pton(AF_INET, json_object_get_string(pod_ip), &addr) != 1
        ) {
            continue;
        }

        inet_ntop(AF_INET, &addr, list->ips[list->count], INET_ADDRSTRLEN);
        list->count++;
    }

    json_object_put(root);

    // host-network pods share their node's IP, and one datagram per node is
    // enough
    qsort(list->ips, list->count, INET_ADDRSTRLEN, compare_ips);

    int unique = 0;

    for (int i = 0; i < list->count; i++) {
        if (unique == 0 || strcmp(list->ips[unique - 1], list->ips[i])) {
            memmove(list->ips[unique++], list->ips[i], INET_ADDRSTRLEN);
        }
    }

    list->count = unique;

    return list;
}

// the API explains a refusal in a Status object; "HTTP 403" alone does not
static void describe_http_error(
    const long code,
    const char *body,
    char *error,
    const size_t error_size
) {
    json_object *root = body ? json_tokener_parse(body) : NULL;
    json_object *message;

    if (root
        && json_object_object_get_ex(root, "message", &message)
        && json_object_is_type(message, json_type_string)
    ) {
        snprintf(error, error_size, "HTTP %ld: %s", code, json_object_get_string(message));
    } else {
        snprintf(error, error_size, "HTTP %ld", code);
    }

    if (root) {
        json_object_put(root);
    }
}

PodListClient *pod_list_client_create(void) {
    PodListClient *client = malloc(sizeof(PodListClient));

    if (!client) {
        return NULL;
    }

    client->curl = curl_easy_init();

    if (!client->curl) {
        free(client);

        return NULL;
    }

    return client;
}

void pod_list_client_destroy(PodListClient *client) {
    if (!client) {
        return;
    }

    curl_easy_cleanup(client->curl);
    free(client);
}

PodList *pod_list_fetch(
    PodListClient *client,
    const int timeout,
    char *error,
    const size_t error_size
) {
    char namespace[MAX_NAMESPACE_SIZE];
    char token[MAX_TOKEN_SIZE];

    if (!resolve_namespace(namespace, sizeof(namespace), error, error_size)) {
        return NULL;
    }

    // read on every request: the kubelet rotates projected tokens in place
    if (!read_trimmed(SERVICE_ACCOUNT_DIR "/token", token, sizeof(token))) {
        snprintf(error, error_size, "cannot read %s/token", SERVICE_ACCOUNT_DIR);

        return NULL;
    }

    char auth_header[MAX_TOKEN_SIZE + 32];
    char url[256];
    char curl_error[CURL_ERROR_SIZE] = "";
    ResponseBody body = {0};
    struct curl_slist *headers = NULL;
    CURL *curl = client->curl;

    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", token);
    snprintf(
        url,
        sizeof(url),
        "https://kubernetes.default.svc/api/v1/namespaces/%s/pods%s",
        namespace,
        PODS_QUERY
    );

    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO, SERVICE_ACCOUNT_DIR "/ca.crt");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    // without it libcurl times name resolution out with SIGALRM, and a signal
    // raised by one thread of the redis process is nobody's to receive
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // bounds connecting, TLS and the transfer together: an unbounded request
    // that never answers stops the list from ever being refreshed again
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_when_closing);

    const CURLcode res = curl_easy_perform(curl);
    long code = 0;
    PodList *list = NULL;

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

    if (res != CURLE_OK) {
        snprintf(
            error,
            error_size,
            "%s",
            curl_error[0] ? curl_error : curl_easy_strerror(res)
        );
    } else if (code != 200) {
        describe_http_error(code, body.data, error, error_size);
    } else if (!body.data) {
        snprintf(error, error_size, "the response is empty");
    } else {
        list = parse_pod_list(body.data, error, error_size);
    }

    // the handle outlives this call, and must not keep pointers into its stack
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, NULL);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    curl_slist_free_all(headers);
    free(body.data);

    return list;
}

void pod_list_free(PodList *list) {
    free(list);
}

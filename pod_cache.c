/*!
 * The last pod list the Kubernetes API returned, kept fresh in the background
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
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "redismodule.h"
#include "closing.h"
#include "settings.h"
#include "pod_cache.h"

// failed requests are retried ever more slowly, up to this many seconds apart
#define MAX_RETRY_DELAY 30
// how often a refresh that keeps failing says so again
#define FAILURE_REMINDER 60

static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static PodSnapshot *current = NULL;
static pthread_t refresher;
static int refresher_running = 0;
static PodListClient *client = NULL;
static int max_age = 0;

static time_t now_seconds(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return now.tv_sec;
}

static void free_snapshot(PodSnapshot *snapshot) {
    pod_list_free(snapshot->pods);
    free(snapshot);
}

void pod_cache_release(const PodSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }

    PodSnapshot *owned = (PodSnapshot *)snapshot;

    pthread_mutex_lock(&cache_mutex);
    const int refs = --owned->refs;
    pthread_mutex_unlock(&cache_mutex);

    if (refs == 0) {
        free_snapshot(owned);
    }
}

const PodSnapshot *pod_cache_acquire(void) {
    pthread_mutex_lock(&cache_mutex);

    PodSnapshot *snapshot = current;

    if (snapshot && max_age > 0 && now_seconds() - snapshot->received_at > max_age) {
        snapshot = NULL;
    }

    if (snapshot) {
        snapshot->refs++;
    }

    pthread_mutex_unlock(&cache_mutex);

    return snapshot;
}

// makes `pods` the current list; readers still holding the old one keep it
static int publish(PodList *pods) {
    PodSnapshot *snapshot = malloc(sizeof(PodSnapshot));

    if (!snapshot) {
        pod_list_free(pods);

        return 0;
    }

    snapshot->refs = 1;
    snapshot->received_at = now_seconds();
    snapshot->pods = pods;

    pthread_mutex_lock(&cache_mutex);
    PodSnapshot *previous = current;
    current = snapshot;
    pthread_mutex_unlock(&cache_mutex);

    pod_cache_release(previous);

    return 1;
}

// how many pods the stored list has, and how old it is; 0 pods when none
static int describe_current(time_t *age) {
    int count = 0;

    pthread_mutex_lock(&cache_mutex);

    if (current) {
        count = current->pods->count;
        *age = now_seconds() - current->received_at;
    }

    pthread_mutex_unlock(&cache_mutex);

    return count;
}

// has the stored list outlived REDIS_BROADCAST_PODS_MAX_AGE?
static int is_expired(void) {
    time_t age = 0;

    return max_age > 0 && describe_current(&age) && age > max_age;
}

// the refresh interval, doubled for each failure in a row, up to the cap
static int next_delay(const int refresh, const int failures) {
    const int cap = refresh > MAX_RETRY_DELAY ? refresh : MAX_RETRY_DELAY;
    int delay = refresh;

    for (int i = 1; i < failures && delay < cap; i++) {
        delay *= 2;
    }

    return delay < cap ? delay : cap;
}

/*
 * Says what a failed request means for the announcements: once when requests
 * start failing, then every FAILURE_REMINDER seconds while they keep failing.
 */
static void log_failure(const char *reason, const time_t failing_for) {
    char what[640];
    time_t age = 0;
    const int count = describe_current(&age);
    const char *name = get_service_name();

    if (failing_for) {
        snprintf(
            what,
            sizeof(what),
            "pod list requests failing for %lds (%s)",
            (long)failing_for,
            reason
        );
    } else {
        snprintf(what, sizeof(what), "pod list request failed (%s)", reason);
    }

    if (!count) {
        RedisModule_Log(
            NULL,
            "warning",
            "%s: %s; there is no pod list yet, nothing is announced until a"
            " request succeeds",
            name,
            what
        );
    } else if (max_age > 0 && age > max_age) {
        RedisModule_Log(
            NULL,
            "warning",
            "%s: %s; the last pod list is %lds old, over"
            " REDIS_BROADCAST_PODS_MAX_AGE=%d, nothing is announced",
            name,
            what,
            (long)age,
            max_age
        );
    } else {
        RedisModule_Log(
            NULL,
            "warning",
            "%s: %s; still announcing to the last pod list, %d pods, %lds old",
            name,
            what,
            count,
            (long)age
        );
    }
}

static void *refresh_pods(void *arg) {
    (void)arg;

    const int refresh = get_pods_refresh();
    const int timeout = get_pods_timeout();
    const char *name = get_service_name();
    int failures = 0;
    int last_count = 0;
    int expiry_reported = 0;
    time_t failing_since = 0;
    time_t last_reminder = 0;

    while (!is_closing()) {
        char error[512] = "";
        PodList *pods = pod_list_fetch(client, timeout, error, sizeof(error));

        if (is_closing()) {
            if (pods) {
                pod_list_free(pods);
            }

            break;
        }

        // the broker's own pod is always in its namespace, so an empty list is
        // something wrong, and replacing a good list with it silences the broker
        if (pods && pods->count == 0) {
            pod_list_free(pods);
            pods = NULL;
            snprintf(error, sizeof(error), "the API listed no pod with an IPv4 address");
        }

        const int count = pods ? pods->count : 0;

        if (pods && !publish(pods)) {
            pods = NULL;
            snprintf(error, sizeof(error), "out of memory");
        }

        if (pods) {
            if (failures) {
                RedisModule_Log(
                    NULL,
                    "notice",
                    "%s: pod list request succeeded again after %lds, announcing"
                    " to %d pods",
                    name,
                    (long)(now_seconds() - failing_since),
                    count
                );
            } else if (!last_count) {
                RedisModule_Log(NULL, "notice", "%s: announcing to %d pods", name, count);
            } else if (count != last_count) {
                RedisModule_Log(NULL, "verbose", "%s: pod list now has %d pods", name, count);
            }

            failures = 0;
            expiry_reported = 0;
            last_count = count;

            closing_sleep(refresh);

            continue;
        }

        const time_t now = now_seconds();
        const int expired = is_expired();

        if (!failures) {
            failing_since = now;
            last_reminder = now;
            log_failure(error, 0);
        } else if (now - last_reminder >= FAILURE_REMINDER
            || (expired && !expiry_reported)
        ) {
            // the moment announcing stops is worth saying at once, not at
            // the next reminder
            last_reminder = now;
            log_failure(error, now - failing_since);
        }

        expiry_reported = expired;
        failures++;

        closing_sleep(next_delay(refresh, failures));
    }

    return NULL;
}

int pod_cache_start(void) {
    if (refresher_running) {
        return 1;
    }

    max_age = get_pods_max_age();
    client = pod_list_client_create();

    if (!client) {
        return 0;
    }

    if (pthread_create(&refresher, NULL, refresh_pods, NULL) != 0) {
        pod_list_client_destroy(client);
        client = NULL;

        return 0;
    }

    refresher_running = 1;

    return 1;
}

void pod_cache_stop(void) {
    if (refresher_running) {
        pthread_join(refresher, NULL);
        refresher_running = 0;
    }

    pod_list_client_destroy(client);
    client = NULL;

    pthread_mutex_lock(&cache_mutex);
    PodSnapshot *previous = current;
    current = NULL;
    pthread_mutex_unlock(&cache_mutex);

    pod_cache_release(previous);
}

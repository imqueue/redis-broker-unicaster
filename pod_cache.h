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
#ifndef UNICASTER_POD_CACHE_H
#define UNICASTER_POD_CACHE_H

#include <time.h>
#include "pod_list.h"

// one received pod list; shared by every reader, freed after the last release
typedef struct {
    int refs;
    time_t received_at;
    PodList *pods;
} PodSnapshot;

/*
 * Starts the thread that requests the pod list every REDIS_BROADCAST_PODS_REFRESH
 * seconds. Only a successful request replaces the stored list: when the API
 * hangs, refuses or throttles, the previous list stays and keeps being used.
 *
 * @returns non-zero when the thread is running
 */
int pod_cache_start(void);

/*
 * Waits for the refresh thread and drops the stored list. The caller raises
 * the closing signal first; that is what ends the thread and aborts a request
 * that is still running.
 */
void pod_cache_stop(void);

/*
 * The current pod list, or NULL when there is none yet, or when it has been
 * older than REDIS_BROADCAST_PODS_MAX_AGE. Every non-NULL result is handed
 * back with pod_cache_release().
 */
const PodSnapshot *pod_cache_acquire(void);

void pod_cache_release(const PodSnapshot *snapshot);

#endif

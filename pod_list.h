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
#ifndef UNICASTER_POD_LIST_H
#define UNICASTER_POD_LIST_H

#include <stddef.h>
#include <netinet/in.h>

#define MAX_PODS 10000

// distinct, valid IPv4 addresses; never modified once returned
typedef struct {
    int count;
    char ips[][INET_ADDRSTRLEN];
} PodList;

// owns the HTTP connection, which is kept open between requests
typedef struct PodListClient PodListClient;

PodListClient *pod_list_client_create(void);

void pod_list_client_destroy(PodListClient *client);

/*
 * Asks the Kubernetes API for the pods of this namespace.
 *
 * Succeeds only on an HTTP 200 whose body is a pod list: a transport error, a
 * refused or throttled request and an unparseable body all fail, so a caller
 * that keeps its previous list on failure never replaces it with a list made
 * out of an error response. A request running longer than `timeout` seconds,
 * or still running when the module starts closing, is aborted.
 *
 * @param client - the connection to use
 * @param timeout - seconds the whole request may take, connecting included
 * @param error - receives the reason on failure
 * @param error_size - size of `error`
 * @returns the pod list, or NULL on failure
 */
PodList *pod_list_fetch(
    PodListClient *client,
    int timeout,
    char *error,
    size_t error_size
);

void pod_list_free(PodList *list);

#endif

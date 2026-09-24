/*!
 * Module settings read from the environment
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
#ifndef UNICASTER_SETTINGS_H
#define UNICASTER_SETTINGS_H

#define DEFAULT_NAME "imq-broker"
#define DEFAULT_PORT 63000
#define DEFAULT_INTERVAL 1
#define DEFAULT_PODS_REFRESH 5
#define DEFAULT_PODS_TIMEOUT 5

int parse_int(const char *buff);

// REDIS_BROADCAST_NAME
const char *get_service_name(void);

// REDIS_BROADCAST_PORT: the UDP port the announcement is sent to
int get_port(void);

// REDIS_BROADCAST_INTERVAL: seconds between two announcements
int get_interval(void);

// REDIS_BROADCAST_PODS_REFRESH: seconds between two pod list requests
int get_pods_refresh(void);

// REDIS_BROADCAST_PODS_TIMEOUT: seconds one pod list request may take in total
int get_pods_timeout(void);

// REDIS_BROADCAST_PODS_MAX_AGE: seconds a pod list stays usable, 0 = forever
int get_pods_max_age(void);

#endif

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
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include "settings.h"

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

/*
 * A whole number of seconds from the environment, or `fallback` when the
 * variable is unset, not a number, or below `min`.
 */
static int get_env_seconds(const char *name, const int min, const int fallback) {
    const char *env = getenv(name);

    if (env) {
        const int value = parse_int(env);

        if (value >= min) {
            return value;
        }
    }

    return fallback;
}

const char *get_service_name(void) {
    const char *service_name = getenv("REDIS_BROADCAST_NAME");

    if (!service_name) {
        service_name = DEFAULT_NAME;
    }

    return service_name;
}

int get_port(void) {
    const char *env = getenv("REDIS_BROADCAST_PORT");

    if (env) {
        const int port = parse_int(env);

        if (port > 0 && port <= 65535) {
            return port;
        }
    }

    return DEFAULT_PORT;
}

int get_interval(void) {
    return get_env_seconds("REDIS_BROADCAST_INTERVAL", 1, DEFAULT_INTERVAL);
}

int get_pods_refresh(void) {
    return get_env_seconds("REDIS_BROADCAST_PODS_REFRESH", 1, DEFAULT_PODS_REFRESH);
}

int get_pods_timeout(void) {
    return get_env_seconds("REDIS_BROADCAST_PODS_TIMEOUT", 1, DEFAULT_PODS_TIMEOUT);
}

int get_pods_max_age(void) {
    return get_env_seconds("REDIS_BROADCAST_PODS_MAX_AGE", 0, 0);
}

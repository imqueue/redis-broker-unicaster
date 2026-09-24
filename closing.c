/*!
 * The module-wide "closing" signal every background thread stops on
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
#include <stdatomic.h>
#include <time.h>
#include "closing.h"

static atomic_int closing = 0;
static pthread_mutex_t closing_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t closing_cond;
static pthread_once_t closing_once = PTHREAD_ONCE_INIT;

/*
 * The wait runs on the monotonic clock: on the default realtime one, a clock
 * stepped backwards stretches the wait by the size of the step, and a sender
 * that sleeps through the fleet's liveness window gets its broker dropped.
 */
static void init_closing_cond(void) {
    pthread_condattr_t attr;

    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&closing_cond, &attr);
    pthread_condattr_destroy(&attr);
}

void closing_reset(void) {
    pthread_once(&closing_once, init_closing_cond);
    atomic_store(&closing, 0);
}

void closing_begin(void) {
    pthread_once(&closing_once, init_closing_cond);

    // set under the mutex, or a thread that has just checked the flag and not
    // yet started waiting misses the broadcast and sleeps its full time
    pthread_mutex_lock(&closing_mutex);
    atomic_store(&closing, 1);
    pthread_cond_broadcast(&closing_cond);
    pthread_mutex_unlock(&closing_mutex);
}

int is_closing(void) {
    return atomic_load(&closing);
}

int closing_sleep(const int seconds) {
    struct timespec deadline;

    pthread_once(&closing_once, init_closing_cond);
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += seconds;

    pthread_mutex_lock(&closing_mutex);

    while (!atomic_load(&closing)) {
        if (pthread_cond_timedwait(&closing_cond, &closing_mutex, &deadline)) {
            break;
        }
    }

    pthread_mutex_unlock(&closing_mutex);

    return atomic_load(&closing);
}

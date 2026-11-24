/*!
 * Whether redis's main thread is still serving, as the announcer sees it
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
#include <stdatomic.h>
#include <time.h>
#include "redismodule.h"
#include "liveness.h"

/*
 * Why a timestamp and not the module GIL.
 *
 * The announcer runs in its own thread, so it went on announcing `up` while
 * the main thread was deadlocked or stuck in a command or a script that never
 * returned, and services went on routing calls to a broker that answered none
 * of them. Services read the announcements as the broker's heartbeat, so the
 * heartbeat has to come from the thread that does the answering.
 *
 * The main thread runs serverCron `hz` times a second (10 by default, never
 * less than 1) whenever it gets back to its event loop, and the CronLoop hook
 * stamps the time here. A command, a script or a deadlock that holds the main
 * thread stops the stamps. So does a script past busy-reply-threshold: redis
 * then answers clients BUSY from inside it, but runs no cron until it ends.
 * The announcer reads the stamp without taking any lock, so it costs the main
 * thread nothing and can never wait on it.
 *
 * Probing with RedisModule_ThreadSafeContextTryLock() was the alternative, and
 * it answers a different question. The main thread holds the GIL whenever it
 * is not inside epoll_wait(), so on a broker saturated with ordinary traffic a
 * single try fails almost every time: a healthy broker under load - the one
 * the fleet can least afford to lose - would stop announcing. A blocking lock
 * is worse: the announcers are joined from the main thread, with the GIL held,
 * when the module unloads (and, in the unicaster, on shutdown), and an
 * announcer waiting for the GIL there deadlocks both threads.
 *
 * Why 2 x interval + 2 seconds.
 *
 * A service drops a broker whose announcements stop for longer than its lapse
 * window W = interval + aliveTimeoutCorrection (5 s in @imqueue/core by
 * default), once a PING then goes unanswered too. With a limit L, a stall of S
 * seconds leaves a silence of at most
 *
 *     S - L + 2 x interval + 1/hz
 *
 * on the wire: the last `up` can go out up to an interval (plus the age of the
 * last cron, at most 1/hz) before the limit is reached, and the first one after
 * the stall up to an interval after it ends. With L = 2 x interval + 2 that is
 * at most S - 1 second even at hz 1, the slowest cron redis allows: the
 * silence a service sees is at least a second shorter than the stall that
 * caused it. A stall shorter than a service's lapse window therefore never
 * lapses it, whatever its aliveTimeoutCorrection; a broker that is merely slow
 * keeps its place. A stall shorter than L - 1/hz withholds nothing at all.
 *
 * At the defaults (interval 1 s, hz 10) the limit is 4 s: announcements stop
 * 4 to 5 s into a stall, and a service drops the broker 9 to 12 s into it,
 * after its 6 s window and a PING that fails at once (BUSY) or after 2 s.
 */

#define LIVENESS_SLACK_SECONDS 2

// monotonic milliseconds of the last server cron, 0 for none since load
static atomic_llong last_beat_ms = 0;

// non-zero while `up` is being withheld, so each change is logged only once
static atomic_int withholding = 0;

static long long monotonic_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void liveness_reset(void) {
    atomic_store(&last_beat_ms, 0);
    atomic_store(&withholding, 0);
}

void liveness_beat(void) {
    atomic_store(&last_beat_ms, monotonic_ms());
}

long long liveness_limit_ms(const int interval) {
    return (2LL * interval + LIVENESS_SLACK_SECONDS) * 1000;
}

int liveness_serving(const char *name, const int interval) {
    const long long beat = atomic_load(&last_beat_ms);

    // no cron since load. The announcers are started from the first one, so
    // they should never see this; if one does, nothing has vouched for redis
    if (!beat) {
        return 0;
    }

    const long long limit = liveness_limit_ms(interval);
    const long long silent = monotonic_ms() - beat;
    const int serving = silent <= limit;

    if (!serving && !atomic_exchange(&withholding, 1)) {
        RedisModule_Log(
            NULL,
            "warning",
            "%s: redis has run no server cron for %lld ms (limit %lld ms) -"
            " withholding `up` until it does, so services stop routing to a"
            " broker that does not answer",
            name,
            silent,
            limit
        );
    } else if (serving && atomic_exchange(&withholding, 0)) {
        RedisModule_Log(
            NULL,
            "notice",
            "%s: redis is serving again, announcing `up` again",
            name
        );
    }

    return serving;
}

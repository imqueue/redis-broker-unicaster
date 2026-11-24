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
#ifndef BROKER_LIVENESS_H
#define BROKER_LIVENESS_H

/*
 * Forgets every beat; called on module load, before any announcer exists.
 * Until the next liveness_beat() the main thread counts as not serving.
 */
void liveness_reset(void);

/*
 * Records that the main thread has just completed a server cron. Called from
 * the CronLoop hook, so on the main thread, `hz` times a second while it is
 * free to run its event loop - and never while it is stuck in a command, a
 * script or a deadlock.
 */
void liveness_beat(void);

/*
 * How long the main thread may go without a server cron before `up` is
 * withheld: 2 x interval + 2 seconds, in milliseconds. See liveness.c for why.
 */
long long liveness_limit_ms(int interval);

/*
 * Called by an announcer before each `up`. Logs once when the main thread
 * stops serving and once when it serves again, however many announcers ask.
 *
 * @param name - the broker name, for the log line
 * @param interval - REDIS_BROADCAST_INTERVAL, in seconds
 * @returns non-zero while the main thread has run a server cron within
 *          liveness_limit_ms(interval)
 */
int liveness_serving(const char *name, int interval);

#endif

/*
 * In-process byte channel between libuade and the embedded uadecore thread.
 * See uade/uadechannel.h.
 */

#include <uade/uadechannel.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
#endif

#define UADE_CHANNEL_MAX 8

/*
 * The protocol is a strict token ping-pong, so only one side is ever sending
 * and at most one message (~4 KiB) is in flight. The buffer only has to be
 * big enough to keep the handoff from thrashing.
 */
#define UADE_CHANNEL_CAPACITY 16384

/*
 * A read or write that makes no progress for this long means the peer is
 * wedged. Failing loudly beats blocking the host forever. Tests override it.
 */
#ifndef UADE_CHANNEL_TIMEOUT_MS
#define UADE_CHANNEL_TIMEOUT_MS 30000
#endif

/*
 * Endpoint id layout: [generation:25][slot:5][end:1]. The generation makes a
 * stale id from a released channel fail lookup instead of aliasing a reused
 * slot -- both sides close the same id twice, once as in_fd and once as
 * out_fd. Always non-negative, so uade_set_peer()'s assertions still hold.
 */
#define UADE_CHANNEL_SLOT_SHIFT 1
#define UADE_CHANNEL_SLOT_MASK 0x1fu
#define UADE_CHANNEL_GEN_SHIFT 6
#define UADE_CHANNEL_GEN_MASK 0x1ffffffu

struct uade_channel_end {
	uint8_t *buf; /* bytes waiting for the endpoint that owns this end */
	size_t head;  /* read offset into buf */
	size_t level; /* bytes readable */
	int closed;
};

struct uade_channel {
	int in_use;
	unsigned int generation;
	struct uade_channel_end end[2];
};

static struct uade_channel channels[UADE_CHANNEL_MAX];

/*
 * One lock and one condition variable for every channel: with two threads and
 * a strictly alternating protocol there is nothing to contend over, and a
 * broadcast on every state change keeps the wakeup rules trivial.
 */
#ifdef _WIN32

static SRWLOCK channel_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE channel_cond = CONDITION_VARIABLE_INIT;

static void lock_channels(void)
{
	AcquireSRWLockExclusive(&channel_lock);
}

static void unlock_channels(void)
{
	ReleaseSRWLockExclusive(&channel_lock);
}

static void wake_channels(void)
{
	WakeAllConditionVariable(&channel_cond);
}

/* Returns 0 when woken, -1 on timeout. */
static int wait_channels(unsigned int ms)
{
	if (SleepConditionVariableSRW(&channel_cond, &channel_lock, ms, 0))
		return 0;
	return -1;
}

static uint64_t now_ms(void)
{
	return GetTickCount64();
}

#else

static pthread_mutex_t channel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t channel_cond = PTHREAD_COND_INITIALIZER;

static void lock_channels(void)
{
	pthread_mutex_lock(&channel_lock);
}

static void unlock_channels(void)
{
	pthread_mutex_unlock(&channel_lock);
}

static void wake_channels(void)
{
	pthread_cond_broadcast(&channel_cond);
}

/*
 * The wait is relative and re-derived from the monotonic deadline each pass, so
 * a wall-clock step can stretch or shorten one wait but cannot defeat the
 * no-progress deadline itself.
 */
static int wait_channels(unsigned int ms)
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	ts.tv_sec += ms / 1000;
	ts.tv_nsec += (long) (ms % 1000) * 1000000L;
	if (ts.tv_nsec >= 1000000000L) {
		ts.tv_nsec -= 1000000000L;
		ts.tv_sec++;
	}
	return pthread_cond_timedwait(&channel_cond, &channel_lock, &ts) ==
	       ETIMEDOUT ? -1 : 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000 + (uint64_t) (ts.tv_nsec / 1000000);
}

#endif

/* Call with the lock held. */
static struct uade_channel *lookup(int id, int *end)
{
	unsigned int u = (unsigned int) id;
	unsigned int slot = (u >> UADE_CHANNEL_SLOT_SHIFT) &
			    UADE_CHANNEL_SLOT_MASK;
	struct uade_channel *ch;

	if (id < 0 || slot >= UADE_CHANNEL_MAX)
		return NULL;
	ch = &channels[slot];
	if (!ch->in_use || ch->generation != (u >> UADE_CHANNEL_GEN_SHIFT))
		return NULL;
	*end = (int) (u & 1);
	return ch;
}

/* Call with the lock held. */
static void release(struct uade_channel *ch)
{
	free(ch->end[0].buf);
	free(ch->end[1].buf);
	ch->end[0] = (struct uade_channel_end) {0};
	ch->end[1] = (struct uade_channel_end) {0};
	ch->in_use = 0;
}

/*
 * Blocks until the channel state changes, giving up once the caller's
 * no-progress deadline passes. Returns 0 to retry, -1 to fail the transfer.
 */
static int wait_for_progress(uint64_t deadline)
{
	uint64_t now = now_ms();

	if (now >= deadline)
		return -1;
	return wait_channels((unsigned int) (deadline - now));
}

int uade_ipc_channel_create(int ids[2])
{
	unsigned int slot;
	struct uade_channel *ch;
	uint8_t *bufs[2];

	bufs[0] = malloc(UADE_CHANNEL_CAPACITY);
	bufs[1] = malloc(UADE_CHANNEL_CAPACITY);
	if (bufs[0] == NULL || bufs[1] == NULL) {
		free(bufs[0]);
		free(bufs[1]);
		fprintf(stderr, "uade channel: Out of memory\n");
		return -1;
	}

	lock_channels();

	for (slot = 0; slot < UADE_CHANNEL_MAX; slot++) {
		if (!channels[slot].in_use)
			break;
	}
	if (slot == UADE_CHANNEL_MAX) {
		unlock_channels();
		free(bufs[0]);
		free(bufs[1]);
		fprintf(stderr, "uade channel: No free channels\n");
		return -1;
	}

	ch = &channels[slot];
	ch->in_use = 1;
	ch->generation = (ch->generation + 1) & UADE_CHANNEL_GEN_MASK;
	ch->end[0] = (struct uade_channel_end) {.buf = bufs[0]};
	ch->end[1] = (struct uade_channel_end) {.buf = bufs[1]};

	ids[0] = (int) ((ch->generation << UADE_CHANNEL_GEN_SHIFT) |
			(slot << UADE_CHANNEL_SLOT_SHIFT));
	ids[1] = ids[0] | 1;

	unlock_channels();
	return 0;
}

ssize_t uade_ipc_read(int id, void *buf, size_t count)
{
	uint64_t deadline = now_ms() + UADE_CHANNEL_TIMEOUT_MS;
	uint8_t *dst = buf;
	size_t done = 0;

	lock_channels();

	while (done < count) {
		struct uade_channel_end *self;
		struct uade_channel *ch;
		size_t n;
		int end;

		/* Re-resolved every pass: the lock is dropped while waiting, and
		   the channel may be closed and released in the meantime. */
		ch = lookup(id, &end);
		if (ch == NULL || ch->end[end].closed) {
			unlock_channels();
			return -1;
		}
		self = &ch->end[end];

		if (self->level == 0) {
			if (ch->end[end ^ 1].closed)
				break; /* drained and the peer is gone: EOF */
			if (wait_for_progress(deadline)) {
				unlock_channels();
				fprintf(stderr, "uade channel: Read timed out after %d ms\n",
					UADE_CHANNEL_TIMEOUT_MS);
				return -1;
			}
			continue;
		}

		n = count - done;
		if (n > self->level)
			n = self->level;
		if (n > UADE_CHANNEL_CAPACITY - self->head)
			n = UADE_CHANNEL_CAPACITY - self->head;
		memcpy(&dst[done], &self->buf[self->head], n);
		self->head = (self->head + n) % UADE_CHANNEL_CAPACITY;
		self->level -= n;
		done += n;
		deadline = now_ms() + UADE_CHANNEL_TIMEOUT_MS;
		wake_channels();
	}

	unlock_channels();
	return done == count ? (ssize_t) count : 0;
}

ssize_t uade_ipc_write(int id, const void *buf, size_t count)
{
	uint64_t deadline = now_ms() + UADE_CHANNEL_TIMEOUT_MS;
	const uint8_t *src = buf;
	size_t done = 0;

	lock_channels();

	while (done < count) {
		struct uade_channel_end *peer;
		struct uade_channel *ch;
		size_t space;
		size_t tail;
		size_t n;
		int end;

		/* See uade_ipc_read(): the channel can vanish while we wait. */
		ch = lookup(id, &end);
		if (ch == NULL || ch->end[end].closed ||
		    ch->end[end ^ 1].closed) {
			unlock_channels();
			return -1;
		}
		peer = &ch->end[end ^ 1];

		space = UADE_CHANNEL_CAPACITY - peer->level;
		if (space == 0) {
			if (wait_for_progress(deadline)) {
				unlock_channels();
				fprintf(stderr, "uade channel: Write timed out after %d ms\n",
					UADE_CHANNEL_TIMEOUT_MS);
				return -1;
			}
			continue;
		}

		n = count - done;
		if (n > space)
			n = space;
		tail = (peer->head + peer->level) % UADE_CHANNEL_CAPACITY;
		if (n > UADE_CHANNEL_CAPACITY - tail)
			n = UADE_CHANNEL_CAPACITY - tail;
		memcpy(&peer->buf[tail], &src[done], n);
		peer->level += n;
		done += n;
		deadline = now_ms() + UADE_CHANNEL_TIMEOUT_MS;
		wake_channels();
	}

	unlock_channels();
	return (ssize_t) count;
}

int uade_ipc_close(int id)
{
	struct uade_channel *ch;
	int end;

	lock_channels();

	ch = lookup(id, &end);
	if (ch == NULL) {
		unlock_channels();
		return 0;
	}
	if (!ch->end[end].closed) {
		ch->end[end].closed = 1;
		wake_channels();
	}
	if (ch->end[0].closed && ch->end[1].closed)
		release(ch);

	unlock_channels();
	return 0;
}

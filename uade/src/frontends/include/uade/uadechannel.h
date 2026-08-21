#ifndef _UADE_UADECHANNEL_H_
#define _UADE_UADECHANNEL_H_

#include <uade/unixatomic.h> /* ssize_t */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * In-process byte channel between libuade and the embedded uadecore thread.
 * It replaces the socketpair that the fork/exec build needed; both halves
 * live in one process here, so no kernel object is involved.
 *
 * Endpoints are identified by small non-negative ids rather than file
 * descriptors, so they still travel through uadecore's -i/-o argv unchanged.
 * An endpoint is bidirectional: both halves pass the same id as in and out.
 */

/* Creates a connected endpoint pair. Returns 0 on success, -1 on failure. */
int uade_ipc_channel_create(int ids[2]);

/*
 * Read exactly count bytes: returns count on success, 0 once the peer has
 * closed and the channel is drained, -1 on error or timeout. Mirrors the
 * uade_atomic_read() contract the socket path had.
 */
ssize_t uade_ipc_read(int id, void *buf, size_t count);

/* Writes exactly count bytes. Returns count on success, -1 on error. */
ssize_t uade_ipc_write(int id, const void *buf, size_t count);

/* Idempotent. Wakes the peer's blocked reads and writes. Always returns 0. */
int uade_ipc_close(int id);

#ifdef __cplusplus
}
#endif

#endif

/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef VALKEY_REMOTE_VALUE_H
#define VALKEY_REMOTE_VALUE_H

#include <stddef.h>
#include <stdint.h>

#define REMOTE_VALUE_MAX_SIZE (1024 * 1024)
#define REMOTE_VALUE_MAX_SLOTS 65536
#define REMOTE_VALUE_MAX_TTL 300000

typedef struct remoteValuePool remoteValuePool;
typedef struct {
    uint64_t owner;
    uint64_t object;
} remoteValueIdentity;

/* Offsets/identifiers, never a CPU pointer or a URMA access credential. The
 * caller must choose a fresh, nonzero pool epoch after every pool restart.
 * owner/object identify an object incarnation, not a possibly colliding hash. */
typedef struct {
    uint64_t epoch;
    uint64_t generation;
    uint32_t slot;
    uint32_t length;
    uint64_t checksum;
} remoteValueRef;

typedef struct {
    remoteValuePool *pool;
    remoteValueRef ref;
    const unsigned char *data;
} remoteValueView;

typedef struct {
    size_t capacity_bytes, metadata_bytes, live_bytes;
    uint32_t slots, live_slots, retired_slots;
    uint64_t pins, puts, hits, misses, rejected, expired, corrupt;
} remoteValueStats;

typedef enum {
    REMOTE_VALUE_OK,
    REMOTE_VALUE_MISS,
    REMOTE_VALUE_FULL,
    REMOTE_VALUE_INVALID
} remoteValueResult;

/* Single-owner CPU-addressable pool, independent of object/SDS allocation.
 * The caller owns arena, keeps it writable/alive until Free succeeds, and must
 * not mutate stored bytes. This does NOT map remote memory or register DMA.
 * Initialize Valkey's crc64 table once at process startup before using it.
 * Budget the caller's arena plus MetadataSize; no per-Value allocation occurs. */
size_t remoteValueMetadataSize(size_t arena_size, size_t slot_size);
remoteValuePool *remoteValuePoolCreate(void *arena, size_t arena_size, size_t slot_size, uint64_t epoch);
int remoteValuePoolFree(remoteValuePool *pool);
void remoteValuePoolStats(const remoteValuePool *pool, remoteValueStats *stats);

/* Synchronous copy of an immutable cache replica. The authoritative local
 * Value must remain owned by the caller. ttl/now are server-local monotonic
 * milliseconds, not a remotely supplied absolute clock. Ref changes only on OK.
 * Pool exhaustion, invalid checksum and stale refs never delete authority. */
remoteValueResult remoteValuePut(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const void *data, size_t length, uint64_t checksum, uint64_t now, uint64_t ttl, remoteValueRef *ref);
/* Acquire verifies identity, version, lifetime, generation, length and CRC64.
 * Zero-initialize view before its first use. A successful view pins the slot.
 * Views are linear: do not copy them; Release
 * exactly once before discarding. Failed calls leave view unchanged. A view
 * borrows immutable bytes; release after the last CPU consumer has copied them.
 * It is not a DMA lifetime/fencing mechanism. */
remoteValueResult remoteValueAcquire(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const remoteValueRef *ref, uint64_t now, remoteValueView *view);
int remoteValueRelease(remoteValueView *view);
remoteValueResult remoteValueDrop(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const remoteValueRef *ref);
/* Bounded full sweep; at most REMOTE_VALUE_MAX_SLOTS metadata entries. */
void remoteValueExpire(remoteValuePool *pool, uint64_t now);

#endif

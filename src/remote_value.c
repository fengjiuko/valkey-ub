/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "remote_value.h"
#include "crc64.h"
#include "zmalloc.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
    remoteValueIdentity identity;
    uint64_t version, generation, expires, checksum, pins;
    uint32_t length;
    bool live;
} remoteValueSlot;

struct remoteValuePool {
    unsigned char *arena;
    size_t slot_size;
    uint64_t epoch;
    uint32_t cursor;
    remoteValueStats stats;
    remoteValueSlot slot[];
};

size_t remoteValueMetadataSize(size_t arena_size, size_t slot_size) {
    if (!slot_size || slot_size > REMOTE_VALUE_MAX_SIZE) return 0;
    size_t slots = arena_size / slot_size;
    if (!slots || slots > REMOTE_VALUE_MAX_SLOTS || slots > (SIZE_MAX - sizeof(remoteValuePool)) / sizeof(remoteValueSlot))
        return 0;
    return sizeof(remoteValuePool) + slots * sizeof(remoteValueSlot);
}

remoteValuePool *remoteValuePoolCreate(void *arena, size_t arena_size, size_t slot_size, uint64_t epoch) {
    size_t size = remoteValueMetadataSize(arena_size, slot_size);
    if (!arena || !epoch || !size) return NULL;
    remoteValuePool *pool = ztrycalloc(size);
    if (!pool) return NULL;
    pool->arena = arena;
    pool->slot_size = slot_size;
    pool->epoch = epoch;
    pool->stats.slots = arena_size / slot_size;
    pool->stats.capacity_bytes = pool->stats.slots * slot_size;
    pool->stats.metadata_bytes = size;
    return pool;
}

int remoteValuePoolFree(remoteValuePool *pool) {
    if (pool && pool->stats.pins) return 0;
    zfree(pool);
    return 1;
}

void remoteValuePoolStats(const remoteValuePool *pool, remoteValueStats *stats) {
    *stats = pool->stats;
}

static void retireSlot(remoteValuePool *pool, uint32_t index) {
    remoteValueSlot *slot = &pool->slot[index];
    if (!slot->live) return;
    slot->live = false;
    pool->stats.live_slots--;
    pool->stats.live_bytes -= slot->length;
    if (slot->pins) pool->stats.retired_slots++;
}

void remoteValueExpire(remoteValuePool *pool, uint64_t now) {
    for (uint32_t i = 0; i < pool->stats.slots; i++) {
        if (pool->slot[i].live && now >= pool->slot[i].expires) {
            retireSlot(pool, i);
            pool->stats.expired++;
        }
    }
}

static remoteValueRef slotRef(const remoteValuePool *pool, uint32_t index) {
    const remoteValueSlot *slot = &pool->slot[index];
    remoteValueRef ref = {pool->epoch, slot->generation, index, slot->length, slot->checksum};
    return ref;
}

static remoteValueSlot *findSlot(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const remoteValueRef *ref) {
    if (!ref || !identity.owner || !identity.object || !version || ref->epoch != pool->epoch ||
        ref->slot >= pool->stats.slots)
        return NULL;
    remoteValueSlot *slot = &pool->slot[ref->slot];
    if (!slot->live || !ref->generation || slot->generation != ref->generation ||
        slot->identity.owner != identity.owner || slot->identity.object != identity.object || slot->version != version ||
        slot->length != ref->length || slot->checksum != ref->checksum)
        return NULL;
    return slot;
}

remoteValueResult remoteValuePut(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const void *data, size_t length, uint64_t checksum, uint64_t now, uint64_t ttl, remoteValueRef *ref) {
    if (!ref || !identity.owner || !identity.object || !version || (!data && length) || length > pool->slot_size ||
        !ttl || ttl > REMOTE_VALUE_MAX_TTL || now > UINT64_MAX - ttl ||
        crc64(0, data, length) != checksum) {
        pool->stats.rejected++;
        return REMOTE_VALUE_INVALID;
    }
    remoteValueExpire(pool, now);
    for (uint32_t i = 0; i < pool->stats.slots; i++) {
        uint32_t index = (pool->cursor + i) % pool->stats.slots;
        remoteValueSlot *slot = &pool->slot[index];
        /* Never wrap a generation: an old ref must never name a new Value. */
        if (slot->live || slot->pins || slot->generation == UINT64_MAX) continue;
        if (length) memmove(pool->arena + index * pool->slot_size, data, length);
        slot->identity = identity;
        slot->version = version;
        slot->generation++;
        slot->expires = now + ttl;
        slot->checksum = checksum;
        slot->length = length;
        slot->live = true;
        pool->cursor = (index + 1) % pool->stats.slots;
        pool->stats.puts++;
        pool->stats.live_slots++;
        pool->stats.live_bytes += length;
        *ref = slotRef(pool, index);
        return REMOTE_VALUE_OK;
    }
    pool->stats.rejected++;
    return REMOTE_VALUE_FULL;
}

remoteValueResult remoteValueAcquire(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const remoteValueRef *ref, uint64_t now, remoteValueView *view) {
    if (!view || view->pool) return REMOTE_VALUE_INVALID;
    remoteValueSlot *slot = findSlot(pool, identity, version, ref);
    if (slot && now >= slot->expires) {
        retireSlot(pool, ref->slot);
        pool->stats.expired++;
        slot = NULL;
    }
    if (slot && crc64(0, pool->arena + ref->slot * pool->slot_size, slot->length) != slot->checksum) {
        retireSlot(pool, ref->slot);
        pool->stats.corrupt++;
        slot = NULL;
    }
    if (!slot) {
        pool->stats.misses++;
        return REMOTE_VALUE_MISS;
    }
    if (slot->pins == UINT64_MAX || pool->stats.pins == UINT64_MAX) return REMOTE_VALUE_FULL;
    slot->pins++;
    pool->stats.pins++;
    pool->stats.hits++;
    view->pool = pool;
    view->ref = *ref;
    view->data = pool->arena + ref->slot * pool->slot_size;
    return REMOTE_VALUE_OK;
}

int remoteValueRelease(remoteValueView *view) {
    if (!view || !view->pool) return 0;
    remoteValuePool *pool = view->pool;
    if (view->ref.epoch != pool->epoch || view->ref.slot >= pool->stats.slots) return 0;
    remoteValueSlot *slot = &pool->slot[view->ref.slot];
    if (slot->generation != view->ref.generation || !slot->pins) return 0;
    slot->pins--;
    pool->stats.pins--;
    if (!slot->live && !slot->pins) pool->stats.retired_slots--;
    memset(view, 0, sizeof(*view));
    return 1;
}

remoteValueResult remoteValueDrop(remoteValuePool *pool, remoteValueIdentity identity, uint64_t version, const remoteValueRef *ref) {
    if (!findSlot(pool, identity, version, ref)) return REMOTE_VALUE_MISS;
    retireSlot(pool, ref->slot);
    return REMOTE_VALUE_OK;
}

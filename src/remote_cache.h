/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef VALKEY_REMOTE_CACHE_H
#define VALKEY_REMOTE_CACHE_H

#include "remote_value.h"

#define REMOTE_CACHE_HEADER_SIZE 88
typedef enum {
    REMOTE_CACHE_PUT = 1,
    REMOTE_CACHE_GET,
    REMOTE_CACHE_DROP
} remoteCacheOpcode;
typedef enum {
    REMOTE_CACHE_INVALID = -1,
    REMOTE_CACHE_OK,
    REMOTE_CACHE_INCOMPLETE
} remoteCacheDecodeResult;

/* Experimental cache protocol, separate from RESP and the URMA byte stream.
 * response is 0/1; requests have status OK. A decoded payload borrows its wire.
 * ttl is relative to the cache owner's monotonic clock; only PUT requests use
 * it. Ref/CRC do not authenticate a peer and do not provide DMA fencing. */
typedef struct {
    uint8_t opcode, response;
    remoteValueResult status;
    uint64_t request_id;
    remoteValueIdentity identity;
    uint64_t version;
    remoteValueRef ref;
    uint64_t ttl;
    uint32_t payload_length;
    const unsigned char *payload;
} remoteCacheMessage;

/* Header validation bounds size before allocating/reading the body. Output is
 * unchanged on failure. Encode supports overlapping payload; decode accepts
 * exactly one message, never trailing bytes. */
remoteCacheDecodeResult remoteCacheFrameSize(const unsigned char *wire, size_t length, size_t *size);
remoteCacheDecodeResult remoteCacheEncode(unsigned char *wire, size_t capacity, const remoteCacheMessage *message, size_t *written);
remoteCacheDecodeResult remoteCacheDecode(const unsigned char *wire, size_t length, remoteCacheMessage *message);
/* Synchronous cache-owner dispatch. GET may return a pinned view; the caller
 * must copy the reply into owned transmit storage and Release before reusing
 * that storage. Neither this engine nor the pool performs any transport I/O. */
remoteValueResult remoteCacheApply(remoteValuePool *pool, const remoteCacheMessage *request, uint64_t now, remoteCacheMessage *reply, remoteValueView *view);

/* Publication gates at the local-authority owner. Re-read the CURRENT object
 * identity/version (and check its local expiry) before calling. Timeout/failed
 * decode is passed as reply=NULL. On miss, dst/ref/written remain unchanged.
 * No command may retain a bare robj pointer across an asynchronous operation.
 * This is a reusable cache gate, not yet connected to database commands. */
int remoteCacheAcceptPut(const remoteCacheMessage *request, const remoteCacheMessage *reply, remoteValueIdentity current, uint64_t version, remoteValueRef *ref);
int remoteCacheAcceptGet(const remoteCacheMessage *request, const remoteCacheMessage *reply, remoteValueIdentity current, uint64_t version, void *dst, size_t capacity, size_t *written);

#endif

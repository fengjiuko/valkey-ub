/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "remote_cache.h"
#include "crc64.h"
#include <string.h>

static uint64_t load(const unsigned char *bytes, unsigned int size) {
    uint64_t result = 0;
    while (size--) result = (result << 8) | *bytes++;
    return result;
}

static void store(unsigned char *bytes, uint64_t value, unsigned int size) {
    while (size) {
        bytes[--size] = value & 255;
        value >>= 8;
    }
}

static int sameRef(const remoteValueRef *a, const remoteValueRef *b) {
    return a->epoch == b->epoch && a->generation == b->generation && a->slot == b->slot &&
           a->length == b->length && a->checksum == b->checksum;
}

static int validMessage(const remoteCacheMessage *message) {
    if (!message || message->opcode < REMOTE_CACHE_PUT || message->opcode > REMOTE_CACHE_DROP ||
        message->response > 1 || !message->request_id || !message->identity.owner || !message->identity.object ||
        !message->version || message->status < REMOTE_VALUE_OK || message->status > REMOTE_VALUE_INVALID ||
        message->ref.length > REMOTE_VALUE_MAX_SIZE || message->payload_length > REMOTE_VALUE_MAX_SIZE)
        return 0;
    if (!message->response && message->status != REMOTE_VALUE_OK) return 0;
    if (message->response && message->status != REMOTE_VALUE_OK) {
        if (message->ttl || message->payload_length) return 0;
        if (message->status == REMOTE_VALUE_MISS && message->opcode == REMOTE_CACHE_PUT) return 0;
        if (message->status == REMOTE_VALUE_FULL && message->opcode != REMOTE_CACHE_PUT) return 0;
    } else if (!message->response && message->opcode == REMOTE_CACHE_PUT) {
        return !message->ref.epoch && !message->ref.generation && !message->ref.slot &&
               message->payload_length == message->ref.length && message->ttl && message->ttl <= REMOTE_VALUE_MAX_TTL;
    } else {
        if (message->ttl) return 0;
        uint32_t expected = message->response && message->opcode == REMOTE_CACHE_GET ? message->ref.length : 0;
        if (message->payload_length != expected) return 0;
    }
    if (message->opcode == REMOTE_CACHE_PUT && message->response && message->status != REMOTE_VALUE_OK)
        return !message->ref.epoch && !message->ref.generation && !message->ref.slot;
    return message->ref.epoch && message->ref.generation && message->ref.slot < REMOTE_VALUE_MAX_SLOTS;
}

static remoteCacheDecodeResult decodeHeader(const unsigned char *wire, size_t length, remoteCacheMessage *message) {
    if (!wire) return REMOTE_CACHE_INVALID;
    if (length < REMOTE_CACHE_HEADER_SIZE) return REMOTE_CACHE_INCOMPLETE;
    if (memcmp(wire, "VKVC", 4) || wire[4] != 1 || load(wire + 12, 4)) return REMOTE_CACHE_INVALID;
    remoteCacheMessage candidate = {0};
    candidate.opcode = wire[5];
    candidate.response = wire[6];
    candidate.status = wire[7];
    candidate.payload_length = load(wire + 8, 4);
    candidate.request_id = load(wire + 16, 8);
    candidate.identity.owner = load(wire + 24, 8);
    candidate.identity.object = load(wire + 32, 8);
    candidate.version = load(wire + 40, 8);
    candidate.ref.epoch = load(wire + 48, 8);
    candidate.ref.generation = load(wire + 56, 8);
    candidate.ref.slot = load(wire + 64, 4);
    candidate.ref.length = load(wire + 68, 4);
    candidate.ref.checksum = load(wire + 72, 8);
    candidate.ttl = load(wire + 80, 8);
    if (!validMessage(&candidate)) return REMOTE_CACHE_INVALID;
    *message = candidate;
    return REMOTE_CACHE_OK;
}

remoteCacheDecodeResult remoteCacheFrameSize(const unsigned char *wire, size_t length, size_t *size) {
    remoteCacheMessage message;
    if (!size) return REMOTE_CACHE_INVALID;
    remoteCacheDecodeResult result = decodeHeader(wire, length, &message);
    if (result == REMOTE_CACHE_OK) *size = REMOTE_CACHE_HEADER_SIZE + (size_t)message.payload_length;
    return result;
}

remoteCacheDecodeResult remoteCacheEncode(unsigned char *wire, size_t capacity, const remoteCacheMessage *message, size_t *written) {
    if (!wire || !written || !validMessage(message) || (message->payload_length && !message->payload) ||
        capacity < REMOTE_CACHE_HEADER_SIZE + (size_t)message->payload_length)
        return REMOTE_CACHE_INVALID;
    if (message->payload_length) memmove(wire + REMOTE_CACHE_HEADER_SIZE, message->payload, message->payload_length);
    memset(wire, 0, REMOTE_CACHE_HEADER_SIZE);
    memcpy(wire, "VKVC", 4);
    wire[4] = 1;
    wire[5] = message->opcode;
    wire[6] = message->response;
    wire[7] = message->status;
    store(wire + 8, message->payload_length, 4);
    store(wire + 16, message->request_id, 8);
    store(wire + 24, message->identity.owner, 8);
    store(wire + 32, message->identity.object, 8);
    store(wire + 40, message->version, 8);
    store(wire + 48, message->ref.epoch, 8);
    store(wire + 56, message->ref.generation, 8);
    store(wire + 64, message->ref.slot, 4);
    store(wire + 68, message->ref.length, 4);
    store(wire + 72, message->ref.checksum, 8);
    store(wire + 80, message->ttl, 8);
    *written = REMOTE_CACHE_HEADER_SIZE + message->payload_length;
    return REMOTE_CACHE_OK;
}

remoteCacheDecodeResult remoteCacheDecode(const unsigned char *wire, size_t length, remoteCacheMessage *message) {
    remoteCacheMessage candidate;
    if (!message) return REMOTE_CACHE_INVALID;
    remoteCacheDecodeResult result = decodeHeader(wire, length, &candidate);
    if (result != REMOTE_CACHE_OK) return result;
    size_t expected = REMOTE_CACHE_HEADER_SIZE + (size_t)candidate.payload_length;
    if (length != expected) return length < expected ? REMOTE_CACHE_INCOMPLETE : REMOTE_CACHE_INVALID;
    candidate.payload = wire + REMOTE_CACHE_HEADER_SIZE;
    *message = candidate;
    return REMOTE_CACHE_OK;
}

remoteValueResult remoteCacheApply(remoteValuePool *pool, const remoteCacheMessage *request, uint64_t now, remoteCacheMessage *reply, remoteValueView *view) {
    if (!reply || !view || view->pool || !validMessage(request) || request->response ||
        (request->payload_length && !request->payload))
        return REMOTE_VALUE_INVALID;
    remoteCacheMessage response = *request;
    response.response = 1;
    response.payload = NULL;
    response.payload_length = 0;
    response.ttl = 0;
    if (request->opcode == REMOTE_CACHE_PUT) {
        response.status = remoteValuePut(pool, request->identity, request->version, request->payload,
                                         request->payload_length, request->ref.checksum, now, request->ttl, &response.ref);
    } else if (request->opcode == REMOTE_CACHE_GET) {
        response.status = remoteValueAcquire(pool, request->identity, request->version, &request->ref, now, view);
        /* Resource/validation failures are misses for a reconstructible cache. */
        if (response.status != REMOTE_VALUE_OK) response.status = REMOTE_VALUE_MISS;
        if (response.status == REMOTE_VALUE_OK) {
            response.payload = view->data;
            response.payload_length = view->ref.length;
        }
    } else {
        response.status = remoteValueDrop(pool, request->identity, request->version, &request->ref);
    }
    *reply = response;
    return response.status;
}

static int matches(const remoteCacheMessage *request, const remoteCacheMessage *reply, remoteValueIdentity current, uint64_t version) {
    return validMessage(request) && !request->response && validMessage(reply) && reply->response &&
           reply->status == REMOTE_VALUE_OK && reply->opcode == request->opcode &&
           reply->request_id == request->request_id && reply->identity.owner == request->identity.owner &&
           reply->identity.object == request->identity.object && reply->version == request->version &&
           current.owner == request->identity.owner && current.object == request->identity.object &&
           version == request->version;
}

int remoteCacheAcceptPut(const remoteCacheMessage *request, const remoteCacheMessage *reply, remoteValueIdentity current, uint64_t version, remoteValueRef *ref) {
    if (!ref || !matches(request, reply, current, version) || request->opcode != REMOTE_CACHE_PUT ||
        reply->ref.length != request->ref.length || reply->ref.checksum != request->ref.checksum)
        return 0;
    *ref = reply->ref;
    return 1;
}

int remoteCacheAcceptGet(const remoteCacheMessage *request, const remoteCacheMessage *reply, remoteValueIdentity current, uint64_t version, void *dst, size_t capacity, size_t *written) {
    if (!written || !matches(request, reply, current, version) || request->opcode != REMOTE_CACHE_GET ||
        !sameRef(&request->ref, &reply->ref) || capacity < reply->ref.length ||
        (reply->ref.length && (!reply->payload || !dst)) ||
        crc64(0, reply->payload, reply->ref.length) != request->ref.checksum)
        return 0;
    if (reply->ref.length) memmove(dst, reply->payload, reply->ref.length);
    *written = reply->ref.length;
    return 1;
}

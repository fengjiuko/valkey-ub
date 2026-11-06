/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "crc64.h"
#include "remote_cache.h"
#include "zmalloc.h"
}

static remoteCacheMessage putRequest(const unsigned char *value, uint32_t length) {
    crc64_init();
    remoteCacheMessage request = {};
    request.opcode = REMOTE_CACHE_PUT;
    request.request_id = 1;
    request.identity.owner = 2;
    request.identity.object = 3;
    request.version = 4;
    request.ref.length = length;
    request.ref.checksum = crc64(0, value, length);
    request.ttl = 100;
    request.payload_length = length;
    request.payload = value;
    return request;
}

static remoteCacheMessage getRequest(const remoteCacheMessage *put, const remoteValueRef *ref) {
    remoteCacheMessage get = *put;
    get.opcode = REMOTE_CACHE_GET;
    get.request_id++;
    get.ref = *ref;
    get.payload = NULL;
    get.payload_length = 0;
    get.ttl = 0;
    return get;
}

TEST(RemoteCacheTest, CanonicalHeaderRoundTripAndOverlappingPayload) {
    unsigned char wire[REMOTE_CACHE_HEADER_SIZE + 4];
    remoteCacheMessage request = putRequest((const unsigned char *)"a\0b", 3);
    size_t written = 0;
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &request, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(written, REMOTE_CACHE_HEADER_SIZE + 3u);
    EXPECT_EQ(memcmp(wire, "VKVC\1\1\0\0\0\0\0\3", 12), 0);
    EXPECT_EQ(wire[23], 1);
    EXPECT_EQ(wire[31], 2);
    EXPECT_EQ(wire[39], 3);
    EXPECT_EQ(wire[47], 4);
    EXPECT_EQ(wire[87], 100);
    remoteCacheMessage decoded;
    EXPECT_EQ(remoteCacheDecode(wire, written, &decoded), REMOTE_CACHE_OK);
    EXPECT_EQ(memcmp(decoded.payload, request.payload, 3), 0);
    request.payload = wire + REMOTE_CACHE_HEADER_SIZE;
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &request, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(memcmp(wire + REMOTE_CACHE_HEADER_SIZE, "a\0b", 3), 0);
}

TEST(RemoteCacheTest, TruncationTrailingReservedAndOversizeAreRejectedBeforeBodyAllocation) {
    unsigned char wire[REMOTE_CACHE_HEADER_SIZE + 5];
    remoteCacheMessage request = putRequest((const unsigned char *)"test", 4), decoded = {};
    decoded.request_id = 999;
    size_t written = 0, size = 999;
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &request, &written), REMOTE_CACHE_OK);
    for (size_t i = 0; i < written; i++) {
        EXPECT_EQ(remoteCacheDecode(wire, i, &decoded), REMOTE_CACHE_INCOMPLETE);
        EXPECT_EQ(decoded.request_id, 999u);
    }
    EXPECT_EQ(remoteCacheFrameSize(wire, REMOTE_CACHE_HEADER_SIZE, &size), REMOTE_CACHE_OK);
    EXPECT_EQ(size, written);
    EXPECT_EQ(remoteCacheDecode(wire, written + 1, &decoded), REMOTE_CACHE_INVALID);
    const unsigned int offsets[] = {0, 4, 5, 6, 7, 12};
    for (unsigned int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        unsigned char old = wire[offsets[i]];
        wire[offsets[i]] = 255;
        EXPECT_EQ(remoteCacheFrameSize(wire, REMOTE_CACHE_HEADER_SIZE, &size), REMOTE_CACHE_INVALID);
        wire[offsets[i]] = old;
    }
    memset(wire + 8, 255, 4);
    EXPECT_EQ(remoteCacheFrameSize(wire, REMOTE_CACHE_HEADER_SIZE, &size), REMOTE_CACHE_INVALID);
    EXPECT_EQ(decoded.request_id, 999u);
}

TEST(RemoteCacheTest, PutGetDropTraverseWireAndPreserveAuthority) {
    unsigned char arena[16], wire[REMOTE_CACHE_HEADER_SIZE + 16], output[16];
    const unsigned char authority[] = {5, 0, 255};
    remoteCacheMessage put = putRequest(authority, sizeof(authority)), decoded, reply, response;
    remoteValuePool *pool = remoteValuePoolCreate(arena, sizeof(arena), 16, 123);
    ASSERT_NE(pool, nullptr);
    size_t written = 0;
    remoteValueView view = {};
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &put, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheDecode(wire, written, &decoded), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheApply(pool, &decoded, 10, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &reply, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheDecode(wire, written, &response), REMOTE_CACHE_OK);
    remoteValueRef ref;
    EXPECT_EQ(remoteCacheAcceptPut(&put, &response, put.identity, put.version, &ref), 1);
    remoteCacheMessage get = getRequest(&put, &ref);
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &get, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheDecode(wire, written, &decoded), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheApply(pool, &decoded, 11, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &reply, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteCacheDecode(wire, written, &response), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheAcceptGet(&get, &response, get.identity, get.version, output, sizeof(output), &written), 1);
    EXPECT_EQ(written, sizeof(authority));
    EXPECT_EQ(memcmp(output, authority, written), 0);
    get.opcode = REMOTE_CACHE_DROP;
    EXPECT_EQ(remoteCacheApply(pool, &get, 12, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &reply, &written), REMOTE_CACHE_OK);
    get.opcode = REMOTE_CACHE_GET;
    EXPECT_EQ(remoteCacheApply(pool, &get, 12, &reply, &view), REMOTE_VALUE_MISS);
    EXPECT_EQ(memcmp(authority, "\5\0\377", sizeof(authority)), 0);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteCacheTest, LateRepliesCannotPublishAfterOverwriteDeleteOrRecreate) {
    unsigned char arena[16], output[16];
    memset(output, 0xa5, sizeof(output));
    remoteCacheMessage put = putRequest((const unsigned char *)"old", 3), reply;
    remoteValuePool *pool = remoteValuePoolCreate(arena, sizeof(arena), 16, 123);
    ASSERT_NE(pool, nullptr);
    remoteValueView view = {};
    EXPECT_EQ(remoteCacheApply(pool, &put, 10, &reply, &view), REMOTE_VALUE_OK);
    remoteValueRef ref = {}, sentinel = {999, 999, 0, 0, 0};
    EXPECT_EQ(remoteCacheAcceptPut(&put, &reply, put.identity, put.version + 1, &sentinel), 0);
    EXPECT_EQ(sentinel.epoch, 999u);
    EXPECT_EQ(remoteCacheAcceptPut(&put, &reply, put.identity, put.version, &ref), 1);
    remoteCacheMessage get = getRequest(&put, &ref);
    EXPECT_EQ(remoteCacheApply(pool, &get, 10, &reply, &view), REMOTE_VALUE_OK);
    size_t written = 999;
    remoteValueIdentity recreated = {put.identity.owner, put.identity.object + 1};
    EXPECT_EQ(remoteCacheAcceptGet(&get, &reply, get.identity, get.version + 1, output, sizeof(output), &written), 0);
    EXPECT_EQ(remoteCacheAcceptGet(&get, &reply, recreated, get.version, output, sizeof(output), &written), 0);
    recreated = get.identity;
    recreated.owner++;
    EXPECT_EQ(remoteCacheAcceptGet(&get, &reply, recreated, get.version, output, sizeof(output), &written), 0);
    EXPECT_EQ(remoteCacheAcceptGet(&get, &reply, get.identity, 0, output, sizeof(output), &written), 0);
    EXPECT_EQ(remoteCacheAcceptGet(&get, NULL, get.identity, get.version, output, sizeof(output), &written), 0);
    EXPECT_EQ(written, 999u);
    for (size_t i = 0; i < sizeof(output); i++) EXPECT_EQ(output[i], 0xa5);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteCacheTest, WrongRequestExtentChecksumAndCorruptReplyAreMisses) {
    unsigned char arena[16], output[16];
    remoteCacheMessage put = putRequest((const unsigned char *)"old", 3), reply;
    remoteValuePool *pool = remoteValuePoolCreate(arena, sizeof(arena), 16, 123);
    ASSERT_NE(pool, nullptr);
    remoteValueView view = {};
    EXPECT_EQ(remoteCacheApply(pool, &put, 10, &reply, &view), REMOTE_VALUE_OK);
    remoteCacheMessage get = getRequest(&put, &reply.ref);
    EXPECT_EQ(remoteCacheApply(pool, &get, 10, &reply, &view), REMOTE_VALUE_OK);
    for (int field = 0; field < 7; field++) {
        remoteCacheMessage bad = reply;
        if (field == 0) bad.request_id++;
        if (field == 1) bad.ref.epoch++;
        if (field == 2) bad.ref.generation++;
        if (field == 3) bad.ref.checksum++;
        if (field == 4) bad.payload = (const unsigned char *)"bad";
        if (field == 5) bad.payload = NULL;
        if (field == 6) bad.payload_length--;
        size_t written = 999;
        EXPECT_EQ(remoteCacheAcceptGet(&get, &bad, get.identity, get.version, output, sizeof(output), &written), 0);
        EXPECT_EQ(written, 999u);
    }
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteCacheTest, FullCorruptExpiredAndRestartedCachesRemainRebuildable) {
    unsigned char arena[16], wire[REMOTE_CACHE_HEADER_SIZE];
    remoteCacheMessage put = putRequest((const unsigned char *)"authority", 9), reply;
    remoteValuePool *pool = remoteValuePoolCreate(arena, sizeof(arena), 16, 123);
    ASSERT_NE(pool, nullptr);
    remoteValueView view = {};
    EXPECT_EQ(remoteCacheApply(pool, &put, 10, &reply, &view), REMOTE_VALUE_OK);
    remoteCacheMessage get = getRequest(&put, &reply.ref);
    size_t written = 0;
    EXPECT_EQ(remoteCacheApply(pool, &put, 10, &reply, &view), REMOTE_VALUE_FULL);
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &reply, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheApply(pool, &get, 110, &reply, &view), REMOTE_VALUE_MISS);
    EXPECT_EQ(remoteCacheApply(pool, &put, 111, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheApply(pool, &get, 111, &reply, &view), REMOTE_VALUE_MISS);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
    pool = remoteValuePoolCreate(arena, sizeof(arena), 16, 124);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(remoteCacheApply(pool, &put, 111, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheApply(pool, &get, 111, &reply, &view), REMOTE_VALUE_MISS);
    put.ref.checksum++;
    EXPECT_EQ(remoteCacheApply(pool, &put, 111, &reply, &view), REMOTE_VALUE_INVALID);
    EXPECT_EQ(remoteCacheEncode(wire, sizeof(wire), &reply, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteCacheTest, LargeBinaryValueSpansStreamFramesAndEmptyValueIsValid) {
    const uint32_t length = 65537;
    unsigned char *arena = (unsigned char *)zmalloc(length);
    unsigned char *value = (unsigned char *)zmalloc(length);
    unsigned char *wire = (unsigned char *)zmalloc(REMOTE_CACHE_HEADER_SIZE + length);
    unsigned char *output = (unsigned char *)zmalloc(length);
    for (uint32_t i = 0; i < length; i++) value[i] = (i * 37) & 255;
    remoteCacheMessage put = putRequest(value, length), reply, decoded;
    remoteValuePool *pool = remoteValuePoolCreate(arena, length, length, 123);
    ASSERT_NE(pool, nullptr);
    remoteValueView view = {};
    size_t written = 0;
    EXPECT_EQ(remoteCacheEncode(wire, REMOTE_CACHE_HEADER_SIZE + length, &put, &written), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheDecode(wire, written, &decoded), REMOTE_CACHE_OK);
    EXPECT_EQ(remoteCacheApply(pool, &decoded, 10, &reply, &view), REMOTE_VALUE_OK);
    remoteCacheMessage get = getRequest(&put, &reply.ref);
    EXPECT_EQ(remoteCacheApply(pool, &get, 10, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheAcceptGet(&get, &reply, get.identity, get.version, output, length, &written), 1);
    EXPECT_EQ(written, length);
    EXPECT_EQ(memcmp(output, value, length), 0);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
    pool = remoteValuePoolCreate(arena, length, length, 124);
    ASSERT_NE(pool, nullptr);
    put = putRequest(NULL, 0);
    EXPECT_EQ(remoteCacheApply(pool, &put, 10, &reply, &view), REMOTE_VALUE_OK);
    get = getRequest(&put, &reply.ref);
    EXPECT_EQ(remoteCacheApply(pool, &get, 10, &reply, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteCacheAcceptGet(&get, &reply, get.identity, get.version, NULL, 0, &written), 1);
    EXPECT_EQ(written, 0u);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
    zfree(arena);
    zfree(value);
    zfree(wire);
    zfree(output);
}

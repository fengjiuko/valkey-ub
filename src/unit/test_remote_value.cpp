/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>
#include <string.h>
extern "C" {
#include "crc64.h"
#include "remote_value.h"
}

static const remoteValueIdentity identity = {11, 22};

static remoteValuePool *createPool(void *arena, size_t size, size_t slot, uint64_t epoch) {
    crc64_init();
    return remoteValuePoolCreate(arena, size, slot, epoch);
}

static remoteValueResult put(remoteValuePool *pool, const void *data, size_t length, remoteValueRef *ref) {
    return remoteValuePut(pool, identity, 1, data, length, crc64(0, (const unsigned char *)data, length), 10, 100, ref);
}

TEST(RemoteValueTest, ValidatesPoolLimitsWithoutTouchingCallerArena) {
    unsigned char arena[16];
    memset(arena, 0xa5, sizeof(arena));
    EXPECT_EQ(remoteValueMetadataSize(0, 4), 0u);
    EXPECT_EQ(remoteValueMetadataSize(16, 0), 0u);
    EXPECT_EQ(remoteValueMetadataSize(SIZE_MAX, 1), 0u);
    EXPECT_EQ(remoteValueMetadataSize(16, REMOTE_VALUE_MAX_SIZE + 1), 0u);
    EXPECT_EQ(remoteValuePoolCreate(NULL, 16, 4, 1), nullptr);
    EXPECT_EQ(remoteValuePoolCreate(arena, 16, 4, 0), nullptr);
    remoteValuePool *pool = createPool(arena, sizeof(arena), 5, 1);
    ASSERT_NE(pool, nullptr);
    remoteValueStats stats;
    remoteValuePoolStats(pool, &stats);
    EXPECT_EQ(stats.slots, 3u);
    EXPECT_EQ(stats.capacity_bytes, 15u);
    EXPECT_EQ(stats.metadata_bytes, remoteValueMetadataSize(16, 5));
    for (size_t i = 0; i < sizeof(arena); i++) EXPECT_EQ(arena[i], 0xa5);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, CopiesBinaryValueAndPreservesLocalAuthority) {
    unsigned char arena[32], authority[] = {0, 255, 1, 0};
    remoteValuePool *pool = createPool(arena, sizeof(arena), 16, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef ref;
    EXPECT_EQ(put(pool, authority, sizeof(authority), &ref), REMOTE_VALUE_OK);
    const unsigned char expected[] = {0, 255, 1, 0};
    authority[0] = 55;
    remoteValueView view = {};
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &ref, 10, &view), REMOTE_VALUE_OK);
    ASSERT_NE(view.data, nullptr);
    EXPECT_EQ(memcmp(view.data, expected, sizeof(expected)), 0);
    EXPECT_EQ(authority[0], 55);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValueRelease(&view), 0);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, RetiredPinsPreventReuseAndPoolDestruction) {
    unsigned char arena[16];
    remoteValuePool *pool = createPool(arena, sizeof(arena), 16, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef first, second = {};
    EXPECT_EQ(put(pool, "old", 3, &first), REMOTE_VALUE_OK);
    remoteValueView a = {}, b = {};
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &first, 10, &a), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &first, 10, &b), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteValueDrop(pool, identity, 1, &first), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteValueDrop(pool, identity, 1, &first), REMOTE_VALUE_MISS);
    EXPECT_EQ(put(pool, "new", 3, &second), REMOTE_VALUE_FULL);
    EXPECT_EQ(memcmp(a.data, "old", 3), 0);
    EXPECT_EQ(remoteValuePoolFree(pool), 0);
    EXPECT_EQ(remoteValueRelease(&a), 1);
    EXPECT_EQ(put(pool, "new", 3, &second), REMOTE_VALUE_FULL);
    EXPECT_EQ(remoteValueRelease(&b), 1);
    EXPECT_EQ(put(pool, "new", 3, &second), REMOTE_VALUE_OK);
    EXPECT_NE(first.generation, second.generation);
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &first, 10, &a), REMOTE_VALUE_MISS);
    remoteValueStats stats;
    remoteValuePoolStats(pool, &stats);
    EXPECT_EQ(stats.pins, 0u);
    EXPECT_EQ(stats.retired_slots, 0u);
    EXPECT_EQ(stats.live_bytes, 3u);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, RejectsStaleObjectVersionPoolAndExtentReferences) {
    unsigned char arena[16];
    remoteValuePool *pool = createPool(arena, sizeof(arena), 16, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef ref;
    EXPECT_EQ(put(pool, "value", 5, &ref), REMOTE_VALUE_OK);
    remoteValueView view = {};
    remoteValueIdentity other = {identity.owner, identity.object + 1};
    EXPECT_EQ(remoteValueAcquire(pool, other, 1, &ref, 10, &view), REMOTE_VALUE_MISS);
    other = identity;
    other.owner++;
    EXPECT_EQ(remoteValueAcquire(pool, other, 1, &ref, 10, &view), REMOTE_VALUE_MISS);
    EXPECT_EQ(remoteValueAcquire(pool, identity, 2, &ref, 10, &view), REMOTE_VALUE_MISS);
    for (int field = 0; field < 5; field++) {
        remoteValueRef invalid = ref;
        if (field == 0) invalid.epoch++;
        if (field == 1) invalid.generation++;
        if (field == 2) invalid.slot++;
        if (field == 3) invalid.length++;
        if (field == 4) invalid.checksum++;
        EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &invalid, 10, &view), REMOTE_VALUE_MISS);
        EXPECT_EQ(remoteValueDrop(pool, identity, 1, &invalid), REMOTE_VALUE_MISS);
    }
    EXPECT_EQ(view.pool, nullptr);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, ExpiryDoesNotOverwritePinnedReplyAndRebuildsAfterRelease) {
    unsigned char arena[16];
    remoteValuePool *pool = createPool(arena, sizeof(arena), 16, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef ref;
    EXPECT_EQ(put(pool, "old", 3, &ref), REMOTE_VALUE_OK);
    remoteValueView view = {}, miss = {};
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &ref, 109, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &ref, 110, &miss), REMOTE_VALUE_MISS);
    remoteValueStats stats;
    remoteValuePoolStats(pool, &stats);
    EXPECT_EQ(stats.expired, 1u);
    EXPECT_EQ(stats.live_slots, 0u);
    EXPECT_EQ(stats.retired_slots, 1u);
    EXPECT_EQ(put(pool, "new", 3, &ref), REMOTE_VALUE_FULL);
    EXPECT_EQ(memcmp(view.data, "old", 3), 0);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(put(pool, "new", 3, &ref), REMOTE_VALUE_OK);
    remoteValueExpire(pool, 110);
    remoteValuePoolStats(pool, &stats);
    EXPECT_EQ(stats.live_bytes, 0u);
    EXPECT_EQ(stats.expired, 2u);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, CorruptionIsAMissAndCacheCanBeRebuilt) {
    unsigned char arena[16];
    const unsigned char authority[] = {9, 0, 8};
    remoteValuePool *pool = createPool(arena, sizeof(arena), 16, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef old, rebuilt;
    EXPECT_EQ(put(pool, authority, sizeof(authority), &old), REMOTE_VALUE_OK);
    arena[0] ^= 1; /* Fault injection into the caller-owned test arena. */
    remoteValueView view = {};
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &old, 10, &view), REMOTE_VALUE_MISS);
    EXPECT_EQ(put(pool, authority, sizeof(authority), &rebuilt), REMOTE_VALUE_OK);
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &old, 10, &view), REMOTE_VALUE_MISS);
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &rebuilt, 10, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(memcmp(view.data, authority, sizeof(authority)), 0);
    remoteValueStats stats;
    remoteValuePoolStats(pool, &stats);
    EXPECT_EQ(stats.corrupt, 1u);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, InvalidPutAndPoolFullLeaveOutputAndExistingValueUnchanged) {
    unsigned char arena[4];
    remoteValuePool *pool = createPool(arena, sizeof(arena), 4, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef ref = {}, sentinel = {99, 99, 99, 99, 99};
    EXPECT_EQ(put(pool, "abcd", 4, &ref), REMOTE_VALUE_OK);
    EXPECT_EQ(put(pool, "next", 4, &sentinel), REMOTE_VALUE_FULL);
    EXPECT_EQ(sentinel.epoch, 99u);
    EXPECT_EQ(remoteValuePut(pool, identity, 1, NULL, 1, 0, 0, 1, &sentinel), REMOTE_VALUE_INVALID);
    EXPECT_EQ(remoteValuePut(pool, identity, 1, "bad", 3, 0, 0, 1, &sentinel), REMOTE_VALUE_INVALID);
    EXPECT_EQ(remoteValuePut(pool, identity, 1, "", 0, 0, 0, 0, &sentinel), REMOTE_VALUE_INVALID);
    EXPECT_EQ(remoteValuePut(pool, identity, 1, "", 0, 0, UINT64_MAX, 1, &sentinel), REMOTE_VALUE_INVALID);
    EXPECT_EQ(remoteValuePut(pool, identity, 1, "", 0, 0, 0, REMOTE_VALUE_MAX_TTL + 1, &sentinel), REMOTE_VALUE_INVALID);
    EXPECT_EQ(put(pool, "large", 5, &sentinel), REMOTE_VALUE_INVALID);
    EXPECT_EQ(sentinel.generation, 99u);
    remoteValueView view = {};
    EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &ref, 10, &view), REMOTE_VALUE_OK);
    EXPECT_EQ(memcmp(view.data, "abcd", 4), 0);
    EXPECT_EQ(remoteValueRelease(&view), 1);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

TEST(RemoteValueTest, EmptyValueAndThousandsOfExtentReusesPreserveGeneration) {
    unsigned char arena[16];
    remoteValuePool *pool = createPool(arena, sizeof(arena), 16, 7);
    ASSERT_NE(pool, nullptr);
    remoteValueRef ref = {}, previous = {};
    for (unsigned int i = 0; i < 4096; i++) {
        unsigned char value[4] = {(unsigned char)i, 0, (unsigned char)(i >> 8), 255};
        size_t length = i % 5;
        EXPECT_EQ(put(pool, value, length, &ref), REMOTE_VALUE_OK);
        EXPECT_GT(ref.generation, previous.generation);
        remoteValueView view = {};
        EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &previous, 10, &view), REMOTE_VALUE_MISS);
        EXPECT_EQ(remoteValueAcquire(pool, identity, 1, &ref, 10, &view), REMOTE_VALUE_OK);
        EXPECT_EQ(memcmp(view.data, value, length), 0);
        EXPECT_EQ(remoteValueRelease(&view), 1);
        EXPECT_EQ(remoteValueDrop(pool, identity, 1, &ref), REMOTE_VALUE_OK);
        previous = ref;
    }
    remoteValueStats stats;
    remoteValuePoolStats(pool, &stats);
    EXPECT_EQ(stats.live_slots, 0u);
    EXPECT_EQ(stats.pins, 0u);
    EXPECT_EQ(stats.puts, 4096u);
    EXPECT_EQ(remoteValuePoolFree(pool), 1);
}

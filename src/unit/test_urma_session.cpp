/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include "urma_session.h"
}

static urmaSession *createSession(uint32_t payload, uint32_t slots, uint32_t peer_slots) {
    urmaHello local = {0, 0, payload, slots};
    urmaHello peer = {0, 0, payload, peer_slots};
    return urmaSessionCreate(42, &local, &peer);
}

static urmaProtocolResult deliverFrame(urmaSession *session, const urmaFrame *frame) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + 64];
    size_t written = 0;
    if (urmaEncodeFrame(wire, sizeof(wire), frame, 64, &written) != URMA_PROTOCOL_OK) return URMA_PROTOCOL_INVALID;
    return urmaSessionReceive(session, wire, written);
}

/* Helpers use EXPECT so callers always reach explicit cleanup. */
static uint64_t writeFrame(urmaSession *session, const char *data, size_t length, urmaFrame *frame, size_t expected) {
    uint64_t operation = 0;
    urmaIoResult result = urmaSessionWrite(session, data, length, frame, &operation);
    EXPECT_EQ(result.status, URMA_IO_OK);
    EXPECT_EQ(result.bytes, expected);
    EXPECT_NE(operation, 0u);
    return operation;
}

TEST(UrmaSessionTest, CopiesAcceptedPrefixAndKeepsBorrowUntilCompletion) {
    urmaSession *session = createSession(4, 1, 2);
    ASSERT_TRUE(session != NULL);
    char input[] = "abcdef";
    urmaFrame frame;
    uint64_t operation = writeFrame(session, input, 6, &frame, 4);
    memset(input, 'z', 6);
    EXPECT_EQ(memcmp(frame.payload, "abcd", 4), 0);
    EXPECT_EQ(frame.sequence, 0u);
    EXPECT_EQ(urmaSessionFree(session), 0);
    uint64_t untouched = 99;
    EXPECT_EQ(urmaSessionWrite(session, "x", 1, &frame, &untouched).status, URMA_IO_AGAIN);
    EXPECT_EQ(untouched, 99u);
    EXPECT_EQ(urmaSessionComplete(session, 41, operation, 1), 0);
    EXPECT_EQ(urmaSessionComplete(session, 42, operation, 1), 1);
    EXPECT_EQ(urmaSessionComplete(session, 42, operation, 1), 0);
    operation = writeFrame(session, "ef", 2, &frame, 2);
    EXPECT_EQ(frame.sequence, 4u);
    EXPECT_EQ(urmaSessionComplete(session, 42, operation, 1), 1);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, CompletionDoesNotRestorePeerCredit) {
    urmaSession *session = createSession(4, 2, 1);
    ASSERT_TRUE(session != NULL);
    urmaFrame frame;
    uint64_t operation = writeFrame(session, "a", 1, &frame, 1);
    EXPECT_EQ(urmaSessionComplete(session, 42, operation, 1), 1);
    EXPECT_EQ(urmaSessionWrite(session, "b", 1, &frame, &operation).status, URMA_IO_AGAIN);
    urmaFrame credit = {URMA_FRAME_CREDIT, 42, 0, 2, 0, NULL};
    EXPECT_EQ(deliverFrame(session, &credit), URMA_PROTOCOL_OK);
    EXPECT_EQ(deliverFrame(session, &credit), URMA_PROTOCOL_OK);
    operation = writeFrame(session, "b", 1, &frame, 1);
    EXPECT_EQ(urmaSessionComplete(session, 42, operation, 1), 1);
    EXPECT_EQ(urmaSessionWrite(session, "c", 1, &frame, &operation).status, URMA_IO_AGAIN);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, OutOfOrderCompletionDoesNotReuseLiveOperation) {
    urmaSession *session = createSession(4, 2, 3);
    ASSERT_TRUE(session != NULL);
    urmaFrame first, second, third;
    uint64_t op1 = writeFrame(session, "1111", 4, &first, 4);
    uint64_t op2 = writeFrame(session, "2222", 4, &second, 4);
    EXPECT_EQ(urmaSessionComplete(session, 42, op2, 1), 1);
    uint64_t op3 = writeFrame(session, "3333", 4, &third, 4);
    EXPECT_NE(op3, op2);
    EXPECT_EQ(urmaSessionComplete(session, 42, op2, 1), 0);
    EXPECT_EQ(memcmp(first.payload, "1111", 4), 0);
    EXPECT_EQ(urmaSessionComplete(session, 42, op1, 1), 1);
    EXPECT_EQ(urmaSessionComplete(session, 42, op3, 1), 1);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, ReadProgressDoesNotGrantCreditUntilReceiveReposted) {
    urmaSession *sender = createSession(4, 1, 1);
    urmaSession *receiver = createSession(4, 1, 1);
    ASSERT_TRUE(sender != NULL);
    ASSERT_TRUE(receiver != NULL);
    urmaFrame frame, credit;
    uint64_t operation = writeFrame(sender, "abcd", 4, &frame, 4);
    EXPECT_EQ(deliverFrame(receiver, &frame), URMA_PROTOCOL_OK);
    EXPECT_EQ(urmaSessionComplete(sender, 42, operation, 1), 1);
    char output[8] = {0};
    urmaIoResult read = urmaSessionRead(receiver, output, 2);
    EXPECT_EQ(read.status, URMA_IO_OK);
    EXPECT_EQ(read.bytes, 2u);
    EXPECT_EQ(urmaSessionRepostTicket(receiver), 0u);
    read = urmaSessionRead(receiver, output + 2, sizeof(output) - 2);
    EXPECT_EQ(read.bytes, 2u);
    EXPECT_EQ(memcmp(output, "abcd", 4), 0);
    EXPECT_EQ(urmaSessionWrite(sender, "x", 1, &frame, &operation).status, URMA_IO_AGAIN);
    EXPECT_EQ(urmaSessionRepostTicket(receiver), 1u);
    EXPECT_EQ(urmaSessionReposted(receiver, 2, &credit), 0);
    EXPECT_EQ(urmaSessionReposted(receiver, 1, &credit), 1);
    EXPECT_EQ(credit.value, 2u);
    EXPECT_EQ(urmaSessionReposted(receiver, 1, &credit), 0);
    EXPECT_EQ(deliverFrame(sender, &credit), URMA_PROTOCOL_OK);
    operation = writeFrame(sender, "x", 1, &frame, 1);
    EXPECT_EQ(deliverFrame(receiver, &frame), URMA_PROTOCOL_OK);
    EXPECT_EQ(urmaSessionComplete(sender, 42, operation, 1), 1);
    read = urmaSessionRead(receiver, output, sizeof(output));
    EXPECT_EQ(read.bytes, 1u);
    EXPECT_EQ(output[0], 'x');
    EXPECT_EQ(urmaSessionFree(sender), 1);
    EXPECT_EQ(urmaSessionFree(receiver), 1);
}

TEST(UrmaSessionTest, ReadAcrossFramesAndDrainBeforeEof) {
    urmaSession *session = createSession(4, 2, 2);
    ASSERT_TRUE(session != NULL);
    urmaFrame first = {URMA_FRAME_DATA, 42, 0, 0, 2, (const unsigned char *)"ab"};
    urmaFrame second = {URMA_FRAME_DATA, 42, 2, 0, 3, (const unsigned char *)"cde"};
    urmaFrame close = {URMA_FRAME_CLOSE, 42, 5, 0, 0, NULL};
    EXPECT_EQ(deliverFrame(session, &first), URMA_PROTOCOL_OK);
    EXPECT_EQ(deliverFrame(session, &second), URMA_PROTOCOL_OK);
    EXPECT_EQ(deliverFrame(session, &close), URMA_PROTOCOL_OK);
    char output[8] = {0};
    urmaIoResult result = urmaSessionRead(session, output, sizeof(output));
    EXPECT_EQ(result.status, URMA_IO_OK);
    EXPECT_EQ(result.bytes, 5u);
    EXPECT_EQ(memcmp(output, "abcde", 5), 0);
    EXPECT_EQ(urmaSessionRead(session, output, 1).status, URMA_IO_EOF);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, GracefulCloseWaitsForBorrowedFramesAndPeerEof) {
    urmaSession *session = createSession(4, 1, 1);
    ASSERT_TRUE(session != NULL);
    urmaFrame data, close;
    uint64_t operation = writeFrame(session, "ab", 2, &data, 2);
    urmaSessionBeginClose(session);
    urmaSessionBeginClose(session);
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_QUIESCING);
    EXPECT_EQ(urmaSessionMakeClose(session, &close), URMA_IO_AGAIN);
    EXPECT_EQ(urmaSessionWrite(session, "x", 1, &data, &operation).status, URMA_IO_ERROR);
    EXPECT_EQ(urmaSessionComplete(session, 42, operation, 1), 1);
    EXPECT_EQ(urmaSessionMakeClose(session, &close), URMA_IO_OK);
    EXPECT_EQ(close.sequence, 2u);
    EXPECT_EQ(urmaSessionMakeClose(session, &close), URMA_IO_AGAIN);
    urmaFrame peer_close = {URMA_FRAME_CLOSE, 42, 0, 0, 0, NULL};
    EXPECT_EQ(deliverFrame(session, &peer_close), URMA_PROTOCOL_OK);
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_CLOSED);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, FailedCompletionRetainsOtherBorrowedFrames) {
    urmaSession *session = createSession(4, 2, 2);
    ASSERT_TRUE(session != NULL);
    urmaFrame frame;
    uint64_t first = writeFrame(session, "a", 1, &frame, 1);
    uint64_t second = writeFrame(session, "b", 1, &frame, 1);
    EXPECT_EQ(urmaSessionComplete(session, 42, first, 0), 1);
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_FAILED);
    EXPECT_EQ(urmaSessionFree(session), 0);
    char output;
    EXPECT_EQ(urmaSessionRead(session, &output, 1).status, URMA_IO_ERROR);
    EXPECT_EQ(urmaSessionComplete(session, 42, second, 1), 1);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, RejectsWrongEpochGapsOvergrantAndReceiveOverflow) {
    const urmaFrame invalid[] = {
        {URMA_FRAME_DATA, 41, 0, 0, 1, (const unsigned char *)"a"},
        {URMA_FRAME_DATA, 42, 1, 0, 1, (const unsigned char *)"a"},
        {URMA_FRAME_CREDIT, 42, 0, UINT64_MAX, 0, NULL},
        {URMA_FRAME_CLOSE, 42, 1, 0, 0, NULL}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        urmaSession *session = createSession(4, 1, 1);
        ASSERT_TRUE(session != NULL);
        EXPECT_EQ(deliverFrame(session, &invalid[i]), URMA_PROTOCOL_INVALID) << i;
        EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_FAILED);
        EXPECT_EQ(urmaSessionFree(session), 1);
    }
    urmaSession *session = createSession(4, 1, 1);
    ASSERT_TRUE(session != NULL);
    urmaFrame frame = {URMA_FRAME_DATA, 42, 0, 0, 1, (const unsigned char *)"a"};
    EXPECT_EQ(deliverFrame(session, &frame), URMA_PROTOCOL_OK);
    frame.sequence = 1;
    EXPECT_EQ(deliverFrame(session, &frame), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, ZeroLengthOperationsDoNotChangeStateOrOutput) {
    urmaSession *session = createSession(4, 1, 1);
    ASSERT_TRUE(session != NULL);
    uint64_t operation = 99;
    urmaFrame frame;
    memset(&frame, 0x5a, sizeof(frame));
    unsigned char saved[sizeof(frame)];
    memcpy(saved, &frame, sizeof(frame));
    EXPECT_EQ(urmaSessionRead(session, NULL, 0).status, URMA_IO_OK);
    EXPECT_EQ(urmaSessionWrite(session, NULL, 0, &frame, &operation).status, URMA_IO_OK);
    EXPECT_EQ(memcmp(&frame, saved, sizeof(frame)), 0);
    EXPECT_EQ(operation, 99u);
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_READY);
    char output;
    EXPECT_EQ(urmaSessionRead(session, &output, 1).status, URMA_IO_AGAIN);
    EXPECT_EQ(urmaSessionFree(session), 1);
    urmaHello hello = {0, 0, 4, 1};
    EXPECT_TRUE(urmaSessionCreate(0, &hello, &hello) == NULL);
    hello.receive_slots = 0;
    EXPECT_TRUE(urmaSessionCreate(42, &hello, &hello) == NULL);
}

TEST(UrmaSessionTest, ConsumedButUnpostedSlotCannotBeOverwritten) {
    urmaSession *session = createSession(4, 1, 1);
    ASSERT_TRUE(session != NULL);
    urmaFrame frame = {URMA_FRAME_DATA, 42, 0, 0, 1, (const unsigned char *)"a"};
    EXPECT_EQ(deliverFrame(session, &frame), URMA_PROTOCOL_OK);
    char output;
    EXPECT_EQ(urmaSessionRead(session, &output, 1).bytes, 1u);
    frame.sequence = 1;
    EXPECT_EQ(deliverFrame(session, &frame), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_FAILED);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, CloseDoesNotDiscardUnreadDataAndRejectsLaterData) {
    urmaSession *session = createSession(4, 2, 2);
    ASSERT_TRUE(session != NULL);
    urmaFrame data = {URMA_FRAME_DATA, 42, 0, 0, 1, (const unsigned char *)"a"};
    urmaFrame close = {URMA_FRAME_CLOSE, 42, 1, 0, 0, NULL};
    EXPECT_EQ(deliverFrame(session, &data), URMA_PROTOCOL_OK);
    EXPECT_EQ(deliverFrame(session, &close), URMA_PROTOCOL_OK);
    urmaSessionBeginClose(session);
    EXPECT_EQ(urmaSessionMakeClose(session, &close), URMA_IO_OK);
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_QUIESCING);
    char output;
    EXPECT_EQ(urmaSessionRead(session, &output, 1).bytes, 1u);
    EXPECT_EQ(output, 'a');
    EXPECT_EQ(urmaSessionGetState(session), URMA_SESSION_CLOSED);
    EXPECT_EQ(urmaSessionRead(session, &output, 1).status, URMA_IO_EOF);
    EXPECT_EQ(urmaSessionFree(session), 1);

    session = createSession(4, 1, 1);
    ASSERT_TRUE(session != NULL);
    close.sequence = 0;
    EXPECT_EQ(deliverFrame(session, &close), URMA_PROTOCOL_OK);
    EXPECT_EQ(deliverFrame(session, &data), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(urmaSessionFree(session), 1);
}

TEST(UrmaSessionTest, RepeatedSlotWraparoundPreservesBinaryStream) {
    urmaSession *sender = createSession(7, 2, 2);
    urmaSession *receiver = createSession(7, 2, 2);
    ASSERT_TRUE(sender != NULL);
    ASSERT_TRUE(receiver != NULL);
    uint64_t offset = 0;
    for (unsigned int round = 0; round < 1024; round++) {
        unsigned char input[7];
        for (size_t i = 0; i < sizeof(input); i++) input[i] = (round + 17 * i) & 0xff;
        urmaFrame frame, credit;
        uint64_t operation;
        urmaIoResult written = urmaSessionWrite(sender, input, sizeof(input), &frame, &operation);
        EXPECT_EQ(written.status, URMA_IO_OK);
        EXPECT_EQ(frame.sequence, offset);
        offset += written.bytes;
        EXPECT_EQ(deliverFrame(receiver, &frame), URMA_PROTOCOL_OK);
        EXPECT_EQ(urmaSessionComplete(sender, 42, operation, 1), 1);
        unsigned char output[7];
        urmaIoResult read = urmaSessionRead(receiver, output, round % 7 + 1);
        EXPECT_EQ(read.status, URMA_IO_OK);
        size_t copied = read.bytes;
        while (copied < sizeof(output)) {
            read = urmaSessionRead(receiver, output + copied, sizeof(output) - copied);
            EXPECT_EQ(read.status, URMA_IO_OK);
            if (read.status != URMA_IO_OK || !read.bytes) break;
            copied += read.bytes;
        }
        EXPECT_EQ(copied, sizeof(output));
        EXPECT_EQ(memcmp(input, output, sizeof(output)), 0);
        uint64_t ticket = urmaSessionRepostTicket(receiver);
        EXPECT_NE(ticket, 0u);
        EXPECT_EQ(urmaSessionReposted(receiver, ticket, &credit), 1);
        EXPECT_EQ(deliverFrame(sender, &credit), URMA_PROTOCOL_OK);
    }
    EXPECT_EQ(urmaSessionFree(sender), 1);
    EXPECT_EQ(urmaSessionFree(receiver), 1);
}

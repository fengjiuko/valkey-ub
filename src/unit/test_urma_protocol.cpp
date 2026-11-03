/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <gtest/gtest.h>
#include <string.h>

extern "C" {
#include "urma_protocol.h"
}

TEST(UrmaProtocolTest, KnownWireVectorAndUnalignedInput) {
    const unsigned char expected[] = {
        'V', 'K', 'U', 'R', 1, 0, 2, 0,
        0, 0, 0, 3, 0, 0, 0, 0,
        1, 2, 3, 4, 5, 6, 7, 8,
        0, 0, 0, 0, 0, 0, 0, 9,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0xff, 0x42};
    unsigned char wire[sizeof(expected) + 1];
    const unsigned char payload[] = {0, 0xff, 0x42};
    urmaFrame frame = {URMA_FRAME_DATA, UINT64_C(0x0102030405060708), 9, 0, sizeof(payload), payload};
    size_t written = 0;
    ASSERT_EQ(urmaEncodeFrame(wire + 1, sizeof(wire) - 1, &frame, 3, &written), URMA_PROTOCOL_OK);
    ASSERT_EQ(written, sizeof(expected));
    EXPECT_EQ(memcmp(wire + 1, expected, sizeof(expected)), 0);
    urmaFrame decoded;
    ASSERT_EQ(urmaDecodeFrame(wire + 1, written, 3, &decoded), URMA_PROTOCOL_OK);
    EXPECT_EQ(decoded.type, URMA_FRAME_DATA);
    EXPECT_EQ(decoded.session_id, frame.session_id);
    EXPECT_EQ(decoded.sequence, 9u);
    EXPECT_EQ(decoded.payload_len, sizeof(payload));
    EXPECT_EQ(decoded.payload, wire + 1 + URMA_PROTOCOL_HEADER_SIZE);
    EXPECT_EQ(memcmp(decoded.payload, payload, sizeof(payload)), 0);
}

TEST(UrmaProtocolTest, TruncationAndTrailingBytesLeaveOutputUntouched) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + 5];
    const unsigned char payload[] = {1, 2, 3, 4};
    urmaFrame frame = {URMA_FRAME_DATA, 1, 0, 0, sizeof(payload), payload};
    size_t written = 0;
    ASSERT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 4, &written), URMA_PROTOCOL_OK);
    urmaFrame output;
    memset(&output, 0xa5, sizeof(output));
    unsigned char saved[sizeof(output)];
    memcpy(saved, &output, sizeof(output));
    for (size_t length = 0; length < written; length++) {
        EXPECT_EQ(urmaDecodeFrame(wire, length, 4, &output), URMA_PROTOCOL_INCOMPLETE) << length;
        EXPECT_EQ(memcmp(&output, saved, sizeof(output)), 0);
    }
    EXPECT_EQ(urmaDecodeFrame(wire, written + 1, 4, &output), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(memcmp(&output, saved, sizeof(output)), 0);
}

TEST(UrmaProtocolTest, InvalidHeaderIsRejectedBeforePayloadArrives) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + 1];
    const unsigned char payload[] = {1};
    urmaFrame frame = {URMA_FRAME_DATA, 1, 0, 0, 1, payload};
    size_t written = 0;
    ASSERT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 1, &written), URMA_PROTOCOL_OK);
    const size_t offsets[] = {0, 4, 5, 6, 7, 12, 13, 14, 15};
    urmaFrame decoded;
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        unsigned char saved = wire[offsets[i]];
        wire[offsets[i]] = 0xff;
        EXPECT_EQ(urmaDecodeFrame(wire, URMA_PROTOCOL_HEADER_SIZE, 1, &decoded), URMA_PROTOCOL_INVALID) << offsets[i];
        wire[offsets[i]] = saved;
    }
    wire[8] = 0xff;
    EXPECT_EQ(urmaDecodeFrame(wire, URMA_PROTOCOL_HEADER_SIZE, URMA_PROTOCOL_MAX_PAYLOAD, &decoded), URMA_PROTOCOL_INVALID);
    wire[8] = 0;
    memset(wire + 16, 0, 8);
    EXPECT_EQ(urmaDecodeFrame(wire, written, 1, &decoded), URMA_PROTOCOL_INVALID);
}

TEST(UrmaProtocolTest, EncodeFailurePreservesBufferAndLength) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + 2];
    memset(wire, 0x5a, sizeof(wire));
    unsigned char saved[sizeof(wire)];
    memcpy(saved, wire, sizeof(wire));
    const unsigned char payload[] = {1, 2};
    urmaFrame frame = {URMA_FRAME_DATA, 1, 0, 0, 2, payload};
    size_t written = 17;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire) - 1, &frame, 2, &written), URMA_PROTOCOL_INCOMPLETE);
    EXPECT_EQ(written, 17u);
    EXPECT_EQ(memcmp(wire, saved, sizeof(wire)), 0);
    frame.sequence = UINT64_MAX - 1;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 2, &written), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(written, 17u);
    EXPECT_EQ(memcmp(wire, saved, sizeof(wire)), 0);
    frame.sequence = 0;
    frame.payload = NULL;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 2, &written), URMA_PROTOCOL_INVALID);
}

TEST(UrmaProtocolTest, ControlFieldsAndLimitsAreValidated) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE];
    urmaFrame frame = {URMA_FRAME_CREDIT, 1, 0, 7, 0, NULL};
    size_t written = 0;
    urmaFrame decoded;
    ASSERT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 1, &written), URMA_PROTOCOL_OK);
    ASSERT_EQ(urmaDecodeFrame(wire, written, 1, &decoded), URMA_PROTOCOL_OK);
    EXPECT_EQ(decoded.value, 7u);
    frame.sequence = 1;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 1, &written), URMA_PROTOCOL_INVALID);
    frame.sequence = 0;
    frame.value = 0;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 1, &written), URMA_PROTOCOL_INVALID);
    frame.type = URMA_FRAME_CLOSE;
    frame.sequence = UINT64_MAX;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 1, &written), URMA_PROTOCOL_OK);
    frame.value = 1;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 1, &written), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(urmaDecodeFrame(wire, sizeof(wire), 0, &decoded), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(urmaDecodeFrame(wire, sizeof(wire), URMA_PROTOCOL_MAX_PAYLOAD + 1, &decoded), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(urmaDecodeFrame(NULL, 0, 1, &decoded), URMA_PROTOCOL_INVALID);
}

TEST(UrmaProtocolTest, HelloNegotiatesDirectionalCapacityAndRequiredFeatures) {
    const urmaHello local = {3, 1, 1024, 8};
    urmaHello peer = {5, 1, 512, 3};
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + URMA_PROTOCOL_HELLO_SIZE];
    size_t written = 0;
    ASSERT_EQ(urmaEncodeHello(wire, sizeof(wire), 42, &peer, &written), URMA_PROTOCOL_OK);
    urmaFrame frame;
    ASSERT_EQ(urmaDecodeFrame(wire, written, 1, &frame), URMA_PROTOCOL_OK);
    urmaHello decoded;
    ASSERT_EQ(urmaDecodeHello(&frame, &decoded), URMA_PROTOCOL_OK);
    EXPECT_EQ(decoded.supported_features, peer.supported_features);
    EXPECT_EQ(decoded.required_features, peer.required_features);
    EXPECT_EQ(decoded.max_payload, peer.max_payload);
    EXPECT_EQ(decoded.receive_slots, peer.receive_slots);
    urmaNegotiated negotiated;
    ASSERT_EQ(urmaNegotiateHello(&local, &decoded, &negotiated), URMA_PROTOCOL_OK);
    EXPECT_EQ(negotiated.features, 1u);
    EXPECT_EQ(negotiated.max_payload, 512u);
    EXPECT_EQ(negotiated.peer_receive_slots, 3u);
    peer.required_features = 4;
    EXPECT_EQ(urmaNegotiateHello(&local, &peer, &negotiated), URMA_PROTOCOL_INVALID);
    peer.required_features = 2;
    EXPECT_EQ(urmaEncodeHello(wire, sizeof(wire), 42, &peer, &written), URMA_PROTOCOL_INVALID);
    peer.required_features = 0;
    peer.receive_slots = URMA_PROTOCOL_MAX_SLOTS + 1;
    EXPECT_EQ(urmaNegotiateHello(&local, &peer, &negotiated), URMA_PROTOCOL_INVALID);
    peer.receive_slots = 1;
    peer.max_payload = 0;
    EXPECT_EQ(urmaEncodeHello(wire, sizeof(wire), 42, &peer, &written), URMA_PROTOCOL_INVALID);
}

TEST(UrmaProtocolTest, MalformedHelloIsRejectedWithoutChangingOutputs) {
    const urmaHello hello = {1, 1, 1, 1};
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + URMA_PROTOCOL_HELLO_SIZE];
    size_t written;
    ASSERT_EQ(urmaEncodeHello(wire, sizeof(wire), 1, &hello, &written), URMA_PROTOCOL_OK);
    urmaFrame frame;
    wire[URMA_PROTOCOL_HEADER_SIZE + 15] = 0;
    EXPECT_EQ(urmaDecodeFrame(wire, written, 1, &frame), URMA_PROTOCOL_INVALID);
    wire[URMA_PROTOCOL_HEADER_SIZE + 15] = 1;
    wire[39] = 2;
    EXPECT_EQ(urmaDecodeFrame(wire, written, 1, &frame), URMA_PROTOCOL_INVALID);
    urmaNegotiated result;
    memset(&result, 0x5a, sizeof(result));
    unsigned char saved[sizeof(result)];
    memcpy(saved, &result, sizeof(result));
    urmaHello invalid = hello;
    invalid.max_payload = URMA_PROTOCOL_MAX_PAYLOAD + 1;
    EXPECT_EQ(urmaNegotiateHello(&hello, &invalid, &result), URMA_PROTOCOL_INVALID);
    EXPECT_EQ(memcmp(&result, saved, sizeof(result)), 0);
}

TEST(UrmaProtocolTest, PayloadCanOverlapDestination) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + 3];
    memcpy(wire, "abc", 3);
    urmaFrame frame = {URMA_FRAME_DATA, 1, 0, 0, 3, wire};
    size_t written;
    ASSERT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, 3, &written), URMA_PROTOCOL_OK);
    urmaFrame decoded;
    ASSERT_EQ(urmaDecodeFrame(wire, written, 3, &decoded), URMA_PROTOCOL_OK);
    EXPECT_EQ(memcmp(decoded.payload, "abc", 3), 0);
}

TEST(UrmaProtocolTest, MaximumPayloadAndOffsetBoundary) {
    static unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + URMA_PROTOCOL_MAX_PAYLOAD];
    urmaFrame frame = {URMA_FRAME_DATA, UINT64_MAX, UINT64_MAX - URMA_PROTOCOL_MAX_PAYLOAD,
                       0, URMA_PROTOCOL_MAX_PAYLOAD, wire + URMA_PROTOCOL_HEADER_SIZE};
    size_t written;
    ASSERT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, URMA_PROTOCOL_MAX_PAYLOAD, &written), URMA_PROTOCOL_OK);
    urmaFrame decoded;
    EXPECT_EQ(urmaDecodeFrame(wire, written, URMA_PROTOCOL_MAX_PAYLOAD, &decoded), URMA_PROTOCOL_OK);
    EXPECT_EQ(decoded.payload_len, URMA_PROTOCOL_MAX_PAYLOAD);
    EXPECT_EQ(urmaDecodeFrame(wire, written, URMA_PROTOCOL_MAX_PAYLOAD - 1, &decoded), URMA_PROTOCOL_INVALID);
    frame.sequence++;
    EXPECT_EQ(urmaEncodeFrame(wire, sizeof(wire), &frame, URMA_PROTOCOL_MAX_PAYLOAD, &written), URMA_PROTOCOL_INVALID);
    wire[24] = 0xff;
    memset(wire + 25, 0xff, 7);
    EXPECT_EQ(urmaDecodeFrame(wire, written, URMA_PROTOCOL_MAX_PAYLOAD, &decoded), URMA_PROTOCOL_INVALID);
}

TEST(UrmaProtocolTest, RoundTripsDeterministicBinaryPayloadsAndOffsets) {
    unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + 257];
    unsigned char payload[257];
    uint64_t seed = UINT64_C(0x9123abc4567);
    for (unsigned int iteration = 0; iteration < 512; iteration++) {
        seed = seed * UINT64_C(6364136223846793005) + 1;
        uint32_t length = (seed % sizeof(payload)) + 1;
        for (uint32_t i = 0; i < length; i++) payload[i] = (seed >> (i % 8) * 8) & 0xff;
        urmaFrame input = {URMA_FRAME_DATA, seed | 1, seed & UINT64_C(0x7fffffffffffffff), 0, length, payload};
        size_t written;
        ASSERT_EQ(urmaEncodeFrame(wire, sizeof(wire), &input, sizeof(payload), &written), URMA_PROTOCOL_OK);
        urmaFrame output;
        ASSERT_EQ(urmaDecodeFrame(wire, written, sizeof(payload), &output), URMA_PROTOCOL_OK);
        EXPECT_EQ(output.session_id, input.session_id);
        EXPECT_EQ(output.sequence, input.sequence);
        EXPECT_EQ(output.payload_len, length);
        EXPECT_EQ(memcmp(output.payload, payload, length), 0);
    }
}

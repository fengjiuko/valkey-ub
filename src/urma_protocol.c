/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "urma_protocol.h"
#include <string.h>

static const unsigned char urma_magic[4] = {'V', 'K', 'U', 'R'};

/* Byte-wise encoding permits unaligned buffers on every supported platform. */
static uint64_t urmaLoadInteger(const unsigned char *src, size_t length) {
    uint64_t value = 0;
    for (size_t i = 0; i < length; i++) value = (value << 8) | src[i];
    return value;
}

static void urmaStoreInteger(unsigned char *dst, uint64_t value, size_t length) {
    for (size_t i = length; i > 0; i--) {
        dst[i - 1] = value & 0xff;
        value >>= 8;
    }
}

static int urmaHelloValid(const urmaHello *hello) {
    return hello && hello->max_payload > 0 && hello->max_payload <= URMA_PROTOCOL_MAX_PAYLOAD &&
           hello->receive_slots > 0 && hello->receive_slots <= URMA_PROTOCOL_MAX_SLOTS &&
           !(hello->required_features & ~hello->supported_features);
}

static int urmaHeaderValid(const urmaFrame *frame, uint32_t max_payload) {
    if (!max_payload || max_payload > URMA_PROTOCOL_MAX_PAYLOAD || !frame->session_id) return 0;

    switch (frame->type) {
    case URMA_FRAME_HELLO:
        return frame->payload_len == URMA_PROTOCOL_HELLO_SIZE && frame->sequence == 0;
    case URMA_FRAME_DATA:
        return frame->payload_len > 0 && frame->payload_len <= max_payload && frame->value == 0 &&
               frame->sequence <= UINT64_MAX - frame->payload_len;
    case URMA_FRAME_CREDIT:
        return frame->payload_len == 0 && frame->sequence == 0 && frame->value > 0;
    case URMA_FRAME_CLOSE:
        return frame->payload_len == 0 && frame->value == 0;
    default:
        return 0;
    }
}

urmaProtocolResult urmaDecodeHello(const urmaFrame *frame, urmaHello *hello) {
    if (!frame || !hello || frame->type != URMA_FRAME_HELLO || !frame->payload ||
        !urmaHeaderValid(frame, URMA_PROTOCOL_MAX_PAYLOAD))
        return URMA_PROTOCOL_INVALID;

    urmaHello decoded = {
        .supported_features = urmaLoadInteger(frame->payload, 8),
        .required_features = frame->value,
        .max_payload = urmaLoadInteger(frame->payload + 8, 4),
        .receive_slots = urmaLoadInteger(frame->payload + 12, 4),
    };
    if (!urmaHelloValid(&decoded)) return URMA_PROTOCOL_INVALID;
    *hello = decoded;
    return URMA_PROTOCOL_OK;
}

urmaProtocolResult urmaEncodeFrame(unsigned char *dst,
                                   size_t capacity,
                                   const urmaFrame *frame,
                                   uint32_t max_payload,
                                   size_t *written) {
    if (!dst || !frame || !written || !urmaHeaderValid(frame, max_payload) ||
        (frame->payload_len && !frame->payload))
        return URMA_PROTOCOL_INVALID;

    if (frame->type == URMA_FRAME_HELLO) {
        urmaHello hello;
        if (urmaDecodeHello(frame, &hello) != URMA_PROTOCOL_OK) return URMA_PROTOCOL_INVALID;
    }

    size_t length = URMA_PROTOCOL_HEADER_SIZE + (size_t)frame->payload_len;
    if (capacity < length) return URMA_PROTOCOL_INCOMPLETE;

    /* Move before writing the header: payload may reside inside dst. */
    if (frame->payload_len) memmove(dst + URMA_PROTOCOL_HEADER_SIZE, frame->payload, frame->payload_len);
    memcpy(dst, urma_magic, sizeof(urma_magic));
    dst[4] = 1; /* Wire version 1.0. No extensions are defined yet. */
    dst[5] = 0;
    dst[6] = frame->type;
    dst[7] = 0;
    urmaStoreInteger(dst + 8, frame->payload_len, 4);
    memset(dst + 12, 0, 4);
    urmaStoreInteger(dst + 16, frame->session_id, 8);
    urmaStoreInteger(dst + 24, frame->sequence, 8);
    urmaStoreInteger(dst + 32, frame->value, 8);
    *written = length;
    return URMA_PROTOCOL_OK;
}

urmaProtocolResult urmaDecodeFrame(const unsigned char *src,
                                   size_t length,
                                   uint32_t max_payload,
                                   urmaFrame *frame) {
    if (!src || !frame || !max_payload || max_payload > URMA_PROTOCOL_MAX_PAYLOAD) return URMA_PROTOCOL_INVALID;
    if (length < URMA_PROTOCOL_HEADER_SIZE) return URMA_PROTOCOL_INCOMPLETE;
    if (memcmp(src, urma_magic, sizeof(urma_magic)) || src[4] != 1 || src[5] != 0 || src[7] != 0 ||
        urmaLoadInteger(src + 12, 4) != 0)
        return URMA_PROTOCOL_INVALID;

    urmaFrame decoded = {
        .type = src[6],
        .session_id = urmaLoadInteger(src + 16, 8),
        .sequence = urmaLoadInteger(src + 24, 8),
        .value = urmaLoadInteger(src + 32, 8),
        .payload_len = urmaLoadInteger(src + 8, 4),
        .payload = src + URMA_PROTOCOL_HEADER_SIZE,
    };
    if (!urmaHeaderValid(&decoded, max_payload)) return URMA_PROTOCOL_INVALID;
    size_t expected = URMA_PROTOCOL_HEADER_SIZE + (size_t)decoded.payload_len;
    if (length < expected) return URMA_PROTOCOL_INCOMPLETE;
    if (length != expected) return URMA_PROTOCOL_INVALID;

    if (decoded.type == URMA_FRAME_HELLO) {
        urmaHello hello;
        if (urmaDecodeHello(&decoded, &hello) != URMA_PROTOCOL_OK) return URMA_PROTOCOL_INVALID;
    }
    *frame = decoded;
    return URMA_PROTOCOL_OK;
}

urmaProtocolResult urmaEncodeHello(unsigned char *dst,
                                   size_t capacity,
                                   uint64_t session_id,
                                   const urmaHello *hello,
                                   size_t *written) {
    if (!urmaHelloValid(hello)) return URMA_PROTOCOL_INVALID;
    unsigned char payload[URMA_PROTOCOL_HELLO_SIZE];
    urmaStoreInteger(payload, hello->supported_features, 8);
    urmaStoreInteger(payload + 8, hello->max_payload, 4);
    urmaStoreInteger(payload + 12, hello->receive_slots, 4);
    urmaFrame frame = {
        .type = URMA_FRAME_HELLO,
        .session_id = session_id,
        .sequence = 0,
        .value = hello->required_features,
        .payload_len = sizeof(payload),
        .payload = payload,
    };
    return urmaEncodeFrame(dst, capacity, &frame, hello->max_payload, written);
}

urmaProtocolResult urmaNegotiateHello(const urmaHello *local,
                                      const urmaHello *peer,
                                      urmaNegotiated *negotiated) {
    if (!negotiated || !urmaHelloValid(local) || !urmaHelloValid(peer) ||
        (local->required_features & ~peer->supported_features) ||
        (peer->required_features & ~local->supported_features))
        return URMA_PROTOCOL_INVALID;
    urmaNegotiated result = {
        .features = local->supported_features & peer->supported_features,
        .max_payload = local->max_payload < peer->max_payload ? local->max_payload : peer->max_payload,
        .peer_receive_slots = peer->receive_slots,
    };
    *negotiated = result;
    return URMA_PROTOCOL_OK;
}

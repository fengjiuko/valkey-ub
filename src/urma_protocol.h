/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_URMA_PROTOCOL_H
#define VALKEY_URMA_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define URMA_PROTOCOL_HEADER_SIZE 40
#define URMA_PROTOCOL_HELLO_SIZE 16
#define URMA_PROTOCOL_MAX_PAYLOAD 65536
#define URMA_PROTOCOL_MAX_SLOTS 1024

typedef enum {
    URMA_FRAME_HELLO = 1,
    URMA_FRAME_DATA,
    URMA_FRAME_CREDIT,
    URMA_FRAME_CLOSE
} urmaFrameType;

typedef enum {
    URMA_PROTOCOL_INVALID = -1,
    URMA_PROTOCOL_OK = 0,
    URMA_PROTOCOL_INCOMPLETE = 1
} urmaProtocolResult;

/* A decoded payload borrows the input buffer; it is not an owned allocation.
 * sequence is a byte offset for DATA/CLOSE, value a cumulative slot grant for
 * CREDIT or the required feature mask for HELLO. Other fields must be zero. */
typedef struct {
    uint8_t type;
    uint64_t session_id;
    uint64_t sequence;
    uint64_t value;
    uint32_t payload_len;
    const unsigned char *payload;
} urmaFrame;

typedef struct {
    uint64_t supported_features;
    uint64_t required_features;
    uint32_t max_payload;
    uint32_t receive_slots;
} urmaHello;

typedef struct {
    uint64_t features;
    uint32_t max_payload;
    uint32_t peer_receive_slots;
} urmaNegotiated;

/* Encode one complete frame without allocation. max_payload is the local or
 * negotiated DATA limit. Payload may overlap dst. On failure dst and written
 * are unchanged. Header bytes and reserved fields have a canonical encoding. */
urmaProtocolResult urmaEncodeFrame(unsigned char *dst,
                                   size_t capacity,
                                   const urmaFrame *frame,
                                   uint32_t max_payload,
                                   size_t *written);

/* Decode exactly one message, rejecting trailing bytes. INCOMPLETE is returned
 * for a truncated, otherwise valid header/frame. Declared lengths are bounded
 * before waiting for payload. On failure frame is unchanged. */
urmaProtocolResult urmaDecodeFrame(const unsigned char *src,
                                   size_t length,
                                   uint32_t max_payload,
                                   urmaFrame *frame);

/* HELLO describes stream limits and capabilities, not hardware endpoints or
 * authentication. Endpoint exchange belongs to the bootstrap implementation. */
urmaProtocolResult urmaEncodeHello(unsigned char *dst,
                                   size_t capacity,
                                   uint64_t session_id,
                                   const urmaHello *hello,
                                   size_t *written);
urmaProtocolResult urmaDecodeHello(const urmaFrame *frame, urmaHello *hello);

/* Both peers' required features must be supported by the other. Unknown
 * optional features are ignored through intersection. Output is unchanged on
 * failure; peer_receive_slots is deliberately not the minimum of both peers. */
urmaProtocolResult urmaNegotiateHello(const urmaHello *local,
                                      const urmaHello *peer,
                                      urmaNegotiated *negotiated);

#endif /* VALKEY_URMA_PROTOCOL_H */

/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "urma_session.h"
#include "zmalloc.h"
#include <stdbool.h>
#include <string.h>

typedef struct {
    uint64_t operation_id;
    uint32_t length;
    uint32_t offset;
} urmaSessionSlot;

struct urmaSession {
    uint64_t id;
    urmaSessionState state;
    uint32_t max_payload;
    uint32_t slots;
    uint32_t peer_slots;
    uint32_t tx_pending;
    uint64_t tx_frames;
    uint64_t tx_credit;
    uint64_t tx_offset;
    uint64_t rx_frames;
    uint64_t rx_consumed;
    uint64_t rx_reposted;
    uint64_t rx_offset;
    bool peer_closed;
    bool close_sent;
    urmaSessionSlot *tx;
    urmaSessionSlot *rx;
    unsigned char *tx_data;
    unsigned char *rx_data;
};

static void urmaSessionUpdateClosed(urmaSession *session) {
    if (session->state == URMA_SESSION_QUIESCING && session->close_sent && session->peer_closed &&
        session->rx_consumed == session->rx_frames && !session->tx_pending)
        session->state = URMA_SESSION_CLOSED;
}

urmaSession *urmaSessionCreate(uint64_t session_id, const urmaHello *local, const urmaHello *peer) {
    urmaNegotiated negotiated;
    if (!session_id || urmaNegotiateHello(local, peer, &negotiated) != URMA_PROTOCOL_OK) return NULL;

    /* Protocol caps bound this arithmetic even on 32-bit hosts. */
    size_t metadata_size = sizeof(urmaSessionSlot) * local->receive_slots;
    size_t data_size = (size_t)negotiated.max_payload * local->receive_slots;
    urmaSession *session = ztrycalloc(sizeof(*session) + 2 * metadata_size + 2 * data_size);
    if (!session) return NULL;
    session->id = session_id;
    session->state = URMA_SESSION_READY;
    session->max_payload = negotiated.max_payload;
    session->slots = local->receive_slots;
    session->peer_slots = negotiated.peer_receive_slots;
    session->tx_credit = negotiated.peer_receive_slots;
    session->tx = (urmaSessionSlot *)(session + 1);
    session->rx = session->tx + session->slots;
    session->tx_data = (unsigned char *)(session->rx + session->slots);
    session->rx_data = session->tx_data + data_size;
    return session;
}

int urmaSessionFree(urmaSession *session) {
    if (session && session->tx_pending) return 0;
    zfree(session);
    return 1;
}

urmaSessionState urmaSessionGetState(const urmaSession *session) {
    return session->state;
}

void urmaSessionFail(urmaSession *session) {
    session->state = URMA_SESSION_FAILED;
}

urmaIoResult urmaSessionWrite(urmaSession *session,
                              const void *data,
                              size_t length,
                              urmaFrame *frame,
                              uint64_t *operation_id) {
    if (!length) return (urmaIoResult){URMA_IO_OK, 0};
    if (!data || !frame || !operation_id || session->state != URMA_SESSION_READY)
        return (urmaIoResult){URMA_IO_ERROR, 0};
    if (session->tx_pending == session->slots || session->tx_frames == session->tx_credit)
        return (urmaIoResult){URMA_IO_AGAIN, 0};

    uint32_t count = length > session->max_payload ? session->max_payload : (uint32_t)length;
    if (session->tx_offset > UINT64_MAX - count || session->tx_frames == UINT64_MAX) {
        urmaSessionFail(session);
        return (urmaIoResult){URMA_IO_ERROR, 0};
    }

    uint32_t index = 0;
    while (session->tx[index].operation_id) index++;
    unsigned char *buffer = session->tx_data + (size_t)index * session->max_payload;
    memcpy(buffer, data, count);
    urmaFrame result = {
        .type = URMA_FRAME_DATA,
        .session_id = session->id,
        .sequence = session->tx_offset,
        .value = 0,
        .payload_len = count,
        .payload = buffer,
    };
    session->tx_frames++;
    session->tx_offset += count;
    session->tx_pending++;
    session->tx[index].operation_id = session->tx_frames;
    *operation_id = session->tx_frames;
    *frame = result;
    return (urmaIoResult){URMA_IO_OK, count};
}

int urmaSessionComplete(urmaSession *session, uint64_t session_id, uint64_t operation_id, int success) {
    if (session->id != session_id || !operation_id) return 0;
    for (uint32_t i = 0; i < session->slots; i++) {
        if (session->tx[i].operation_id != operation_id) continue;
        session->tx[i].operation_id = 0;
        session->tx_pending--;
        if (!success) urmaSessionFail(session);
        urmaSessionUpdateClosed(session);
        return 1;
    }
    return 0;
}

urmaProtocolResult urmaSessionReceive(urmaSession *session, const unsigned char *wire, size_t length) {
    if (session->state == URMA_SESSION_FAILED || session->state == URMA_SESSION_CLOSED) return URMA_PROTOCOL_INVALID;
    urmaFrame frame;
    if (urmaDecodeFrame(wire, length, session->max_payload, &frame) != URMA_PROTOCOL_OK || frame.session_id != session->id)
        goto invalid;

    switch (frame.type) {
    case URMA_FRAME_DATA: {
        if (session->peer_closed || frame.sequence != session->rx_offset ||
            session->rx_frames - session->rx_reposted == session->slots)
            goto invalid;
        uint32_t index = session->rx_frames % session->slots;
        memcpy(session->rx_data + (size_t)index * session->max_payload, frame.payload, frame.payload_len);
        session->rx[index].length = frame.payload_len;
        session->rx[index].offset = 0;
        session->rx_frames++;
        session->rx_offset += frame.payload_len;
        break;
    }
    case URMA_FRAME_CREDIT:
        /* Credit cannot grant more empty slots than the peer advertised, even
         * if every accepted DATA frame has already been consumed. */
        if (frame.value > session->tx_frames && frame.value - session->tx_frames > session->peer_slots) goto invalid;
        if (frame.value > session->tx_credit) session->tx_credit = frame.value;
        break;
    case URMA_FRAME_CLOSE:
        if (frame.sequence != session->rx_offset) goto invalid;
        session->peer_closed = 1;
        urmaSessionUpdateClosed(session);
        break;
    default:
        goto invalid;
    }
    return URMA_PROTOCOL_OK;

invalid:
    urmaSessionFail(session);
    return URMA_PROTOCOL_INVALID;
}

urmaIoResult urmaSessionRead(urmaSession *session, void *data, size_t length) {
    if (!length) return (urmaIoResult){URMA_IO_OK, 0};
    if (!data || session->state == URMA_SESSION_FAILED) return (urmaIoResult){URMA_IO_ERROR, 0};

    size_t copied = 0;
    while (copied < length && session->rx_consumed < session->rx_frames) {
        uint32_t index = session->rx_consumed % session->slots;
        urmaSessionSlot *slot = &session->rx[index];
        size_t count = slot->length - slot->offset;
        if (count > length - copied) count = length - copied;
        memcpy((unsigned char *)data + copied,
               session->rx_data + (size_t)index * session->max_payload + slot->offset,
               count);
        copied += count;
        slot->offset += count;
        if (slot->offset == slot->length) session->rx_consumed++;
    }
    urmaSessionUpdateClosed(session);
    if (copied) return (urmaIoResult){URMA_IO_OK, copied};
    return (urmaIoResult){session->peer_closed ? URMA_IO_EOF : URMA_IO_AGAIN, 0};
}

uint64_t urmaSessionRepostTicket(const urmaSession *session) {
    if (session->state == URMA_SESSION_FAILED || session->state == URMA_SESSION_CLOSED ||
        session->rx_reposted == session->rx_consumed)
        return 0;
    return session->rx_reposted + 1;
}

int urmaSessionReposted(urmaSession *session, uint64_t ticket, urmaFrame *credit) {
    if (!credit || !ticket || ticket != urmaSessionRepostTicket(session)) return 0;
    if (ticket > UINT64_MAX - session->slots) {
        urmaSessionFail(session);
        return 0;
    }
    session->rx_reposted = ticket;
    *credit = (urmaFrame){
        .type = URMA_FRAME_CREDIT,
        .session_id = session->id,
        .sequence = 0,
        .value = ticket + session->slots,
        .payload_len = 0,
        .payload = NULL,
    };
    return 1;
}

void urmaSessionBeginClose(urmaSession *session) {
    if (session->state == URMA_SESSION_READY) session->state = URMA_SESSION_QUIESCING;
}

urmaIoStatus urmaSessionMakeClose(urmaSession *session, urmaFrame *frame) {
    if (!frame || session->state == URMA_SESSION_FAILED || session->state == URMA_SESSION_READY) return URMA_IO_ERROR;
    if (session->tx_pending || session->close_sent) return URMA_IO_AGAIN;
    *frame = (urmaFrame){
        .type = URMA_FRAME_CLOSE,
        .session_id = session->id,
        .sequence = session->tx_offset,
        .value = 0,
        .payload_len = 0,
        .payload = NULL,
    };
    session->close_sent = 1;
    urmaSessionUpdateClosed(session);
    return URMA_IO_OK;
}

/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef VALKEY_URMA_SESSION_H
#define VALKEY_URMA_SESSION_H

#include "urma_protocol.h"

typedef struct urmaSession urmaSession;

typedef enum {
    URMA_SESSION_READY,
    URMA_SESSION_QUIESCING,
    URMA_SESSION_CLOSED,
    URMA_SESSION_FAILED
} urmaSessionState;

typedef enum {
    URMA_IO_ERROR = -1,
    URMA_IO_OK,
    URMA_IO_AGAIN,
    URMA_IO_EOF
} urmaIoStatus;

typedef struct {
    urmaIoStatus status;
    size_t bytes;
} urmaIoResult;

/* The stream model owns bounded RX/TX buffers, not hardware resources. Call
 * only after bootstrap and receive posting succeed; one thread owns it.
 * Creation is fallible and uses the negotiated payload and local slot count.
 * The future runtime must apply its global memory budget before creation. */
urmaSession *urmaSessionCreate(uint64_t session_id, const urmaHello *local, const urmaHello *peer);
/* Conservative admission charge for a local configuration, including metadata.
 * Returns zero for invalid limits. No allocation or SDK access is performed. */
size_t urmaSessionAllocationSize(const urmaHello *local);
int urmaSessionReadable(const urmaSession *session);
int urmaSessionWritable(const urmaSession *session);

/* Stop all backend deliveries and quiesce hardware before freeing. Returns 0
 * while TX frames are borrowed; the session and buffers then remain alive. */
int urmaSessionFree(urmaSession *session);
urmaSessionState urmaSessionGetState(const urmaSession *session);
void urmaSessionFail(urmaSession *session);

/* Accept a continuous prefix into owned storage and return a DATA frame plus
 * a nonzero operation id. Caller may immediately reuse input. Submit returned
 * frames in order; retain/retry the same frame if the backend is temporarily
 * unable to post it. It stays borrowed until Complete. Outputs are unchanged
 * on AGAIN/error/zero-length writes. No command execution is acknowledged. */
urmaIoResult urmaSessionWrite(urmaSession *session,
                              const void *data,
                              size_t length,
                              urmaFrame *frame,
                              uint64_t *operation_id);
/* Completions may be out of order. The epoch and operation id reject stale or
 * duplicate completions. Local completion never restores remote credit. */
int urmaSessionComplete(urmaSession *session, uint64_t session_id, uint64_t operation_id, int success);

/* Consume exactly one complete wire frame. A malformed message, wrong epoch,
 * noncontiguous DATA or capacity violation fails the session. */
urmaProtocolResult urmaSessionReceive(urmaSession *session, const unsigned char *wire, size_t length);
urmaIoResult urmaSessionRead(urmaSession *session, void *data, size_t length);

/* Fully consumed RX slots are not yet reusable. RepostTicket returns the
 * oldest slot's monotonically increasing ticket, or 0 if none. Only after a
 * real successful receive repost may the runtime call Reposted and send the
 * resulting cumulative CREDIT. An invalid ticket changes nothing. */
uint64_t urmaSessionRepostTicket(const urmaSession *session);
int urmaSessionReposted(urmaSession *session, uint64_t ticket, urmaFrame *credit);

/* BeginClose immediately prevents new writes but allows reading. MakeClose
 * emits one directional EOF after all borrowed TX frames complete. It returns
 * AGAIN until then, or after the EOF was already emitted. This is graceful
 * stream shutdown, not forced abort; use Fail for an immediate error. */
void urmaSessionBeginClose(urmaSession *session);
urmaIoStatus urmaSessionMakeClose(urmaSession *session, urmaFrame *frame);

#endif /* VALKEY_URMA_SESSION_H */

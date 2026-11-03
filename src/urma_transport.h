/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef VALKEY_URMA_TRANSPORT_H
#define VALKEY_URMA_TRANSPORT_H

#include "urma_session.h"

#define URMA_ENDPOINT_SIZE 64
#define URMA_ERROR_SIZE 192
#define URMA_DEFAULT_PROGRESS_TIMEOUT 10000

typedef struct urmaRuntime urmaRuntime;
typedef struct urmaTransport urmaTransport;

/* One runtime per process, owned by its creating thread. No calls in fork
 * children. The budget bounds application buffers, not driver/kernel memory.
 * Device must advertise native UB, RC, RTP and target ordering; no fallback. */
urmaRuntime *urmaRuntimeOpen(const char *device, uint32_t eid_index, size_t budget, char *error);
int urmaRuntimeClose(urmaRuntime *runtime);
size_t urmaRuntimeMemory(const urmaRuntime *runtime);
/* Set before creating transports. Positive milliseconds, at most INT_MAX.
 * Deadlines cover accepted TX and receive-repost stalls, not idle RX or a
 * peer withholding credit. Expiry never establishes that DMA has stopped. */
int urmaRuntimeSetProgressTimeout(urmaRuntime *runtime, unsigned int milliseconds);
void urmaRuntimeReap(urmaRuntime *runtime);

/* Creation reserves memory before registering/posting and exports two RC
 * endpoints: DATA/CLOSE and an independent CREDIT channel. Endpoint bytes
 * contain a token; exchange only through an explicitly secured bootstrap.
 * local limits may not change after creation. Session starts on Connect. */
urmaTransport *urmaTransportCreate(urmaRuntime *runtime, uint64_t epoch, const urmaHello *local, char *error);
void urmaTransportEndpoint(const urmaTransport *transport, unsigned char endpoint[URMA_ENDPOINT_SIZE]);
int urmaTransportConnect(urmaTransport *transport, const unsigned char endpoint[URMA_ENDPOINT_SIZE], const urmaHello *peer);
/* Bounded polling; a full budget is a request for another progress turn. */
int urmaTransportProgress(urmaTransport *transport, unsigned int budget);
urmaIoResult urmaTransportRead(urmaTransport *transport, void *data, size_t length);
urmaIoResult urmaTransportWrite(urmaTransport *transport, const void *data, size_t length);
int urmaTransportReadable(const urmaTransport *transport);
int urmaTransportWritable(const urmaTransport *transport);
const char *urmaTransportError(const urmaTransport *transport);
int urmaTransportShutdown(urmaTransport *transport);

/* Abort closes logical access immediately. Delete failures retain the entire
 * object and its admission charge; retry Destroy, never free its storage.
 * A successful resource deletion is required before unregistration/freeing.
 * Provider DMA/quiescence guarantees still require platform qualification. */
int urmaTransportDestroy(urmaTransport *transport);
/* Transfer ownership to runtime, retaining failed deletions for later retry. */
void urmaTransportRetire(urmaTransport *transport);

#endif

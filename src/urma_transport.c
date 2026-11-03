/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "urma_transport.h"

#ifdef USE_URMA
#include "zmalloc.h"
#include <urma_api.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    unsigned char *data;
    uint64_t cookie;
    uint64_t operation;
    uint64_t ticket;
    uint64_t deadline;
    uint32_t length;
    bool posted;
} urmaBuffer;

struct urmaRuntime {
    urma_context_t *context;
    urma_device_attr_t attributes;
    size_t budget, used;
    unsigned int transports;
    unsigned int progress_timeout;
    unsigned int retired_count;
    urmaRuntimeStats stats;
    pid_t owner;
    urmaTransport *retired;
};

struct urmaTransport {
    urmaRuntime *runtime;
    urmaTransport *next;
    urmaSession *session;
    urmaHello local;
    uint64_t epoch, cookie, received, pending_credit, sent_credit;
    size_t charge;
    uint32_t count, stride;
    bool connected, failed, closing;
    bool bound[2];
    urma_jfce_t *event;
    urma_jfc_t *completion;
    urma_jfr_t *receive[2];
    urma_jetty_t *jetty[2];
    urma_target_jetty_t *peer[2];
    urma_target_seg_t *segment;
    urmaBuffer *tx, *rx, *control;
    unsigned char *wire;
    size_t wire_size;
    char error[URMA_ERROR_SIZE];
};

static bool initialized;

static void setError(char *error, const char *format, ...) {
    if (!error) return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, URMA_ERROR_SIZE, format, args);
    va_end(args);
}

static int failTransport(urmaTransport *transport, const char *operation, int status) {
    if (!transport->failed) {
        setError(transport->error, "%s failed (status=%d)", operation, status);
        transport->runtime->stats.errors++;
        if (status == ETIMEDOUT) transport->runtime->stats.progress_timeouts++;
    }
    transport->failed = true;
    if (transport->session) urmaSessionFail(transport->session);
    return -1;
}

static int owned(const urmaRuntime *runtime) {
    return runtime->owner == getpid();
}

urmaRuntime *urmaRuntimeOpen(const char *device, uint32_t eid_index, size_t budget, char *error) {
    if (initialized || !device || !*device || strlen(device) >= URMA_MAX_NAME || !budget) {
        setError(error, "Invalid URMA runtime configuration or runtime already open");
        return NULL;
    }
    urma_status_t status = urma_init(NULL);
    if (status != URMA_SUCCESS) {
        setError(error, "urma_init failed (status=%d); check installed provider and UB driver", status);
        return NULL;
    }
    urmaRuntime *runtime = ztrycalloc(sizeof(*runtime));
    if (!runtime) {
        urma_uninit();
        setError(error, "URMA runtime allocation failed");
        return NULL;
    }
    urma_device_t *dev = urma_get_device_by_name((char *)device);
    if (!dev || dev->type != URMA_TRANSPORT_UB ||
        urma_query_device(dev, &runtime->attributes) != URMA_SUCCESS) {
        setError(error, "Device is unavailable or is not native URMA_TRANSPORT_UB");
        goto invalid;
    }
    const urma_device_cap_t *cap = &runtime->attributes.dev_cap;
    if (!(cap->trans_mode & URMA_TM_RC) || !cap->rc_order_cap.bs.ot || !cap->rc_tp_cap.bs.rtp ||
        !cap->max_jfs_sge || !cap->max_jfr_sge) {
        setError(error, "Device must support RC, RTP, target ordering and one send/receive SGE");
        goto invalid;
    }
    runtime->context = urma_create_context(dev, eid_index);
    if (!runtime->context) {
        setError(error, "urma_create_context failed; check EID index and driver");
        goto invalid;
    }
    runtime->budget = budget;
    runtime->progress_timeout = URMA_DEFAULT_PROGRESS_TIMEOUT;
    runtime->owner = getpid();
    initialized = true;
    return runtime;
invalid:
    zfree(runtime);
    urma_uninit();
    return NULL;
}

size_t urmaRuntimeMemory(const urmaRuntime *runtime) {
    return runtime->used;
}

void urmaRuntimeGetStats(const urmaRuntime *runtime, urmaRuntimeStats *stats) {
    *stats = runtime->stats;
    stats->buffer_used = runtime->used;
    stats->buffer_limit = runtime->budget;
    stats->transports = runtime->transports;
    stats->retired = runtime->retired_count;
}

int urmaRuntimeSetProgressTimeout(urmaRuntime *runtime, unsigned int milliseconds) {
    if (!owned(runtime) || runtime->transports || !milliseconds || milliseconds > INT_MAX) return 0;
    runtime->progress_timeout = milliseconds;
    return 1;
}

static int monotonicMilliseconds(urmaTransport *transport, uint64_t *result) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return failTransport(transport, "clock_gettime", errno);
    uint64_t fraction = now.tv_nsec / 1000000;
    if (now.tv_sec < 0 || (uint64_t)now.tv_sec > (UINT64_MAX - fraction) / 1000)
        return failTransport(transport, "Monotonic clock overflow", EOVERFLOW);
    *result = (uint64_t)now.tv_sec * 1000 + fraction;
    return 0;
}

static int startDeadline(urmaTransport *transport, urmaBuffer *buffer) {
    uint64_t now;
    if (monotonicMilliseconds(transport, &now) < 0) return -1;
    if (!buffer->deadline) {
        if (now > UINT64_MAX - transport->runtime->progress_timeout)
            return failTransport(transport, "Progress deadline overflow", EOVERFLOW);
        buffer->deadline = now + transport->runtime->progress_timeout;
    }
    if (now >= buffer->deadline) return failTransport(transport, "URMA progress timeout", ETIMEDOUT);
    return 0;
}

static int checkDeadlines(urmaTransport *transport) {
    uint64_t now = 0;
    bool have_time = false;
    for (uint32_t i = 0; i < 2 * transport->count + 3; i++) {
        urmaBuffer *buffer = &transport->tx[i];
        if (!buffer->deadline) continue;
        if (!have_time && monotonicMilliseconds(transport, &now) < 0) return -1;
        have_time = true;
        if (now >= buffer->deadline) return failTransport(transport, "URMA progress timeout", ETIMEDOUT);
    }
    return 0;
}

void urmaRuntimeReap(urmaRuntime *runtime) {
    if (!owned(runtime)) return;
    urmaTransport **link = &runtime->retired;
    while (*link) {
        urmaTransport *transport = *link;
        urmaTransport *next = transport->next;
        if (urmaTransportDestroy(transport)) {
            *link = next;
            runtime->retired_count--;
        } else
            link = &transport->next;
    }
}

int urmaRuntimeClose(urmaRuntime *runtime) {
    if (!runtime) return 1;
    if (!owned(runtime)) return 0;
    urmaRuntimeReap(runtime);
    if (runtime->transports) return 0;
    if (runtime->context) {
        if (urma_delete_context(runtime->context) != URMA_SUCCESS) return 0;
        runtime->context = NULL;
    }
    if (urma_uninit() != URMA_SUCCESS) return 0;
    initialized = false;
    zfree(runtime);
    return 1;
}

static uint64_t newCookie(urmaTransport *transport) {
    if (transport->cookie == UINT64_MAX) {
        failTransport(transport, "Completion cookie exhausted", EOVERFLOW);
        return 0;
    }
    return ++transport->cookie;
}

static int postReceive(urmaTransport *transport, urmaBuffer *buffer, int channel) {
    if (buffer->deadline && startDeadline(transport, buffer) < 0) return -1;
    uint64_t cookie = newCookie(transport);
    if (!cookie) return -1;
    urma_sge_t sge = {.addr = (uintptr_t)buffer->data,
                      .len = channel ? URMA_PROTOCOL_HEADER_SIZE : transport->stride,
                      .tseg = transport->segment};
    urma_jfr_wr_t wr = {.src = {.sge = &sge, .num_sge = 1}, .user_ctx = cookie};
    urma_jfr_wr_t *bad = NULL;
    urma_status_t status = urma_post_jfr_wr(transport->receive[channel], &wr, &bad);
    /* Retry only when the SDK identifies this single WR as not accepted.
     * Ambiguous failures retain storage until endpoint destruction. */
    if ((status == URMA_EAGAIN || status == URMA_ENOMEM) && bad == &wr) {
        transport->runtime->stats.post_retries++;
        return startDeadline(transport, buffer) < 0 ? -1 : 0;
    }
    if (status != URMA_SUCCESS) return failTransport(transport, "urma_post_jfr_wr", status);
    buffer->cookie = cookie;
    buffer->posted = true;
    buffer->deadline = 0;
    return 1;
}

static int postSend(urmaTransport *transport, urmaBuffer *buffer, int channel) {
    if (startDeadline(transport, buffer) < 0) return -1;
    uint64_t cookie = newCookie(transport);
    if (!cookie) return -1;
    urma_sge_t sge = {.addr = (uintptr_t)buffer->data, .len = buffer->length, .tseg = transport->segment};
    urma_jfs_wr_t wr = {.opcode = URMA_OPC_SEND, .tjetty = transport->peer[channel], .user_ctx = cookie};
    wr.flag.bs.complete_enable = 1;
    wr.flag.bs.place_order = 2; /* Strong order within this RC endpoint. */
    wr.send.src.sge = &sge;
    wr.send.src.num_sge = 1;
    urma_jfs_wr_t *bad = NULL;
    urma_status_t status = urma_post_jetty_send_wr(transport->jetty[channel], &wr, &bad);
    if ((status == URMA_EAGAIN || status == URMA_ENOMEM) && bad == &wr) {
        transport->runtime->stats.post_retries++;
        return 0;
    }
    if (status != URMA_SUCCESS) return failTransport(transport, "urma_post_jetty_send_wr", status);
    buffer->cookie = cookie;
    buffer->posted = true;
    return 1;
}

static int createChannel(urmaTransport *transport, int channel, uint32_t token) {
    urma_jfr_cfg_t receive = {.depth = channel ? 2 : transport->count,
                              .trans_mode = URMA_TM_RC,
                              .max_sge = 1,
                              .jfc = transport->completion,
                              .token_value = {.token = token}};
    receive.flag.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
    receive.flag.bs.order_type = URMA_OT;
    transport->receive[channel] = urma_create_jfr(transport->runtime->context, &receive);
    if (!transport->receive[channel]) return -1;
    urma_jetty_cfg_t jetty = {0};
    jetty.flag.bs.share_jfr = 1;
    jetty.shared.jfr = transport->receive[channel];
    jetty.shared.jfc = transport->completion;
    jetty.jfs_cfg.depth = channel ? 1 : transport->count;
    jetty.jfs_cfg.trans_mode = URMA_TM_RC;
    jetty.jfs_cfg.max_sge = 1;
    jetty.jfs_cfg.rnr_retry = 3;
    jetty.jfs_cfg.err_timeout = 14;
    jetty.jfs_cfg.flag.bs.order_type = URMA_OT;
    jetty.jfs_cfg.jfc = transport->completion;
    transport->jetty[channel] = urma_create_jetty(transport->runtime->context, &jetty);
    return transport->jetty[channel] ? 0 : -1;
}

urmaTransport *urmaTransportCreate(urmaRuntime *runtime, uint64_t epoch, const urmaHello *local, char *error) {
    size_t session_size = urmaSessionAllocationSize(local);
    if (!owned(runtime) || !epoch || !session_size || !runtime->context) {
        setError(error, "Invalid URMA transport limits, epoch or owner process");
        return NULL;
    }
    uint32_t count = local->receive_slots + 1; /* An extra RX slot reserves ordered CLOSE. */
    uint32_t stride = local->max_payload + URMA_PROTOCOL_HEADER_SIZE;
    const urma_device_cap_t *cap = &runtime->attributes.dev_cap;
    if (cap->max_msg_size < stride || cap->max_jfs_depth < count || cap->max_jfr_depth < count ||
        cap->max_jfc_depth < 2 * count + 3) {
        setError(error, "URMA device message or queue limits are insufficient");
        return NULL;
    }
    size_t metadata = sizeof(urmaBuffer) * (2 * count + 3);
    size_t wire_size = 2 * (size_t)count * stride + 3 * URMA_PROTOCOL_HEADER_SIZE;
    size_t allocation = sizeof(urmaTransport) + metadata + wire_size;
    size_t charge = allocation + session_size;
    if (charge > runtime->budget - runtime->used) {
        runtime->stats.budget_rejections++;
        setError(error, "URMA application buffer budget exhausted");
        return NULL;
    }
    urmaTransport *transport = ztrycalloc(allocation);
    if (!transport) {
        setError(error, "URMA transport allocation failed");
        return NULL;
    }
    runtime->used += charge;
    runtime->transports++;
    transport->runtime = runtime;
    transport->local = *local;
    transport->epoch = epoch;
    transport->count = count;
    transport->stride = stride;
    transport->charge = charge;
    transport->tx = (urmaBuffer *)(transport + 1);
    transport->rx = transport->tx + count;
    transport->control = transport->rx + count;
    transport->wire = (unsigned char *)(transport->control + 3);
    transport->wire_size = wire_size;
    for (uint32_t i = 0; i < count; i++) {
        transport->tx[i].data = transport->wire + (size_t)i * stride;
        transport->rx[i].data = transport->wire + (size_t)(count + i) * stride;
    }
    for (unsigned int i = 0; i < 3; i++)
        transport->control[i].data = transport->wire + 2 * (size_t)count * stride + i * URMA_PROTOCOL_HEADER_SIZE;

    uint32_t tokens[2];
    ssize_t random_size;
    do {
        random_size = getrandom(tokens, sizeof(tokens), 0);
    } while (random_size < 0 && errno == EINTR);
    if (random_size != sizeof(tokens)) goto failed;
    transport->event = urma_create_jfce(runtime->context);
    if (!transport->event) goto failed;
    urma_jfc_cfg_t completion = {.depth = 2 * count + 3, .jfce = transport->event};
    transport->completion = urma_create_jfc(runtime->context, &completion);
    if (!transport->completion) goto failed;
    urma_seg_cfg_t segment = {.va = (uintptr_t)transport->wire, .len = wire_size};
    segment.flag.bs.access = URMA_ACCESS_LOCAL_ONLY;
    transport->segment = urma_register_seg(runtime->context, &segment);
    if (!transport->segment || createChannel(transport, 0, tokens[0]) || createChannel(transport, 1, tokens[1])) goto failed;
    for (uint32_t i = 0; i < count; i++) {
        if (postReceive(transport, &transport->rx[i], 0) != 1) goto failed;
    }
    for (unsigned int i = 1; i < 3; i++) {
        if (postReceive(transport, &transport->control[i], 1) != 1) goto failed;
    }
    return transport;
failed:
    runtime->stats.initialization_failures++;
    setError(error, "URMA resource creation/receive posting failed: %s", transport->error);
    urmaTransportRetire(transport);
    return NULL;
}

static void store32(unsigned char *bytes, uint32_t value) {
    for (int i = 3; i >= 0; i--) {
        bytes[i] = value & 255;
        value >>= 8;
    }
}

static uint32_t load32(const unsigned char *bytes) {
    return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

void urmaTransportEndpoint(const urmaTransport *transport, unsigned char endpoint[URMA_ENDPOINT_SIZE]) {
    memcpy(endpoint, "VKUE", 4);
    endpoint[4] = 1;
    endpoint[5] = URMA_TM_RC;
    endpoint[6] = URMA_OT;
    endpoint[7] = URMA_RTP;
    for (int i = 0; i < 2; i++) {
        const urma_jetty_id_t *id = &transport->jetty[i]->jetty_id;
        memcpy(endpoint + 8 + i * 24, id->eid.raw, 16);
        store32(endpoint + 24 + i * 24, id->uasid);
        store32(endpoint + 28 + i * 24, id->id);
        store32(endpoint + 56 + i * 4, transport->receive[i]->jfr_cfg.token_value.token);
    }
}

int urmaTransportConnect(urmaTransport *transport, const unsigned char endpoint[URMA_ENDPOINT_SIZE], const urmaHello *peer) {
    if (!owned(transport->runtime) || transport->connected || transport->failed || !endpoint ||
        memcmp(endpoint, "VKUE", 4) || endpoint[4] != 1 || endpoint[5] != URMA_TM_RC ||
        endpoint[6] != URMA_OT || endpoint[7] != URMA_RTP)
        return failTransport(transport, "Invalid endpoint contract", EPROTO);
    transport->session = urmaSessionCreate(transport->epoch, &transport->local, peer);
    if (!transport->session) return failTransport(transport, "Stream negotiation/allocation", EINVAL);
    for (int i = 0; i < 2; i++) {
        urma_rjetty_t remote = {.trans_mode = URMA_TM_RC, .tp_type = URMA_RTP, .type = URMA_JETTY};
        memcpy(remote.jetty_id.eid.raw, endpoint + 8 + i * 24, 16);
        remote.jetty_id.uasid = load32(endpoint + 24 + i * 24);
        remote.jetty_id.id = load32(endpoint + 28 + i * 24);
        remote.flag.bs.order_type = URMA_OT;
        remote.flag.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
        urma_token_t token = {.token = load32(endpoint + 56 + i * 4)};
        if (!remote.jetty_id.id) return failTransport(transport, "Zero remote jetty id", EPROTO);
        transport->peer[i] = urma_import_jetty(transport->runtime->context, &remote, &token);
        if (!transport->peer[i]) return failTransport(transport, "urma_import_jetty", errno);
        urma_status_t status = urma_bind_jetty(transport->jetty[i], transport->peer[i]);
        if (status != URMA_SUCCESS) return failTransport(transport, "urma_bind_jetty", status);
        transport->bound[i] = true;
    }
    transport->connected = true;
    transport->runtime->stats.ready_total++;
    return 0;
}

static urmaBuffer *findCookie(urmaBuffer *buffers, uint32_t count, uint64_t cookie) {
    for (uint32_t i = 0; i < count; i++) {
        if (buffers[i].posted && buffers[i].cookie == cookie) return &buffers[i];
    }
    return NULL;
}

static int receiveCompletion(urmaTransport *transport, const urma_cr_t *completion) {
    urmaBuffer *buffer = findCookie(transport->rx, transport->count, completion->user_ctx);
    int channel = 0;
    if (!buffer) {
        channel = 1;
        buffer = findCookie(transport->control + 1, 2, completion->user_ctx);
    }
    if (!buffer) return failTransport(transport, "Unknown RX completion", EPROTO);
    buffer->posted = false;
    uint32_t capacity = channel ? URMA_PROTOCOL_HEADER_SIZE : transport->stride;
    urmaFrame frame;
    if (completion->opcode != URMA_CR_OPC_SEND || completion->completion_len > capacity ||
        urmaDecodeFrame(buffer->data, completion->completion_len, transport->local.max_payload, &frame) != URMA_PROTOCOL_OK ||
        (channel ? frame.type != URMA_FRAME_CREDIT : frame.type != URMA_FRAME_DATA && frame.type != URMA_FRAME_CLOSE))
        return failTransport(transport, "Invalid RX frame", EPROTO);
    if (frame.session_id != transport->epoch ||
        (urmaSessionGetState(transport->session) == URMA_SESSION_CLOSED && frame.type != URMA_FRAME_CREDIT))
        return failTransport(transport, "Frame after stream closure or wrong epoch", EPROTO);
    if (urmaSessionGetState(transport->session) != URMA_SESSION_CLOSED &&
        urmaSessionReceive(transport->session, buffer->data, completion->completion_len) != URMA_PROTOCOL_OK)
        return failTransport(transport, "Invalid stream transition", EPROTO);
    transport->runtime->stats.rx_frames++;
    transport->runtime->stats.rx_wire_bytes += completion->completion_len;
    if (channel) return postReceive(transport, buffer, 1) < 0 ? -1 : 0;
    if (frame.type == URMA_FRAME_DATA) buffer->ticket = ++transport->received;
    return 0;
}

static int sendCompletion(urmaTransport *transport, const urma_cr_t *completion) {
    urmaBuffer *buffer = findCookie(transport->tx, transport->count, completion->user_ctx);
    if (!buffer) buffer = findCookie(transport->control, 1, completion->user_ctx);
    if (!buffer) return failTransport(transport, "Unknown TX completion", EPROTO);
    buffer->posted = false;
    transport->runtime->stats.tx_completions++;
    transport->runtime->stats.tx_wire_bytes += buffer->length;
    buffer->length = 0;
    buffer->deadline = 0;
    if (buffer->operation) {
        if (!urmaSessionComplete(transport->session, transport->epoch, buffer->operation, 1))
            return failTransport(transport, "Stale TX operation", EPROTO);
        buffer->operation = 0;
    }
    return 0;
}

static int replenish(urmaTransport *transport) {
    for (;;) {
        uint64_t ticket = urmaSessionRepostTicket(transport->session);
        if (!ticket) break;
        urmaBuffer *buffer = NULL;
        for (uint32_t i = 0; i < transport->count; i++)
            if (transport->rx[i].ticket == ticket) {
                buffer = &transport->rx[i];
                break;
            }
        if (!buffer) return failTransport(transport, "Missing RX repost ticket", EPROTO);
        int result = postReceive(transport, buffer, 0);
        if (result <= 0) return result;
        urmaFrame credit;
        if (!urmaSessionReposted(transport->session, ticket, &credit)) return failTransport(transport, "RX credit", EPROTO);
        buffer->ticket = 0;
        transport->pending_credit = credit.value;
    }
    for (int i = 1; i < 3; i++) {
        if (!transport->control[i].posted && postReceive(transport, &transport->control[i], 1) < 0) return -1;
    }
    return 0;
}

static int submitPending(urmaTransport *transport) {
    /* DATA frames must be submitted in accepted byte order, even after a
     * temporarily full hardware queue. Never let a later write overtake. */
    for (;;) {
        urmaBuffer *oldest = NULL;
        for (uint32_t i = 0; i < transport->count; i++) {
            urmaBuffer *buffer = &transport->tx[i];
            if (buffer->length && !buffer->posted && (!oldest || buffer->operation < oldest->operation)) oldest = buffer;
        }
        if (!oldest) break;
        int result = postSend(transport, oldest, 0);
        if (result <= 0) return result;
    }
    urmaBuffer *control = &transport->control[0];
    if (!control->posted && transport->pending_credit > transport->sent_credit) {
        urmaFrame credit = {.type = URMA_FRAME_CREDIT, .session_id = transport->epoch, .value = transport->pending_credit};
        size_t length;
        if (urmaEncodeFrame(control->data, URMA_PROTOCOL_HEADER_SIZE, &credit, transport->local.max_payload, &length) != URMA_PROTOCOL_OK)
            return failTransport(transport, "Encode CREDIT", EPROTO);
        control->length = length;
        int result = postSend(transport, control, 1);
        if (result < 0) return -1;
        if (result) transport->sent_credit = credit.value;
    }
    return 0;
}

int urmaTransportProgress(urmaTransport *transport, unsigned int budget) {
    if (!owned(transport->runtime)) return -1;
    if (!transport->connected || transport->failed) return -1;
    unsigned int processed = 0;
    while (processed < budget) {
        urma_cr_t completions[16];
        unsigned int count = budget - processed;
        if (count > 16) count = 16;
        int polled = urma_poll_jfc(transport->completion, count, completions);
        if (polled < 0) return failTransport(transport, "urma_poll_jfc", polled);
        if (!polled) break;
        for (int i = 0; i < polled; i++) {
            urma_cr_t *completion = &completions[i];
            if (completion->status != URMA_CR_SUCCESS)
                return failTransport(transport, "URMA work completion", completion->status);
            int result = completion->flag.bs.s_r ? receiveCompletion(transport, completion) : sendCompletion(transport, completion);
            if (result < 0) return -1;
        }
        processed += polled;
    }
    /* Drain observed completions before expiring the operations they finish.
     * Retries and unrelated traffic never extend an individual deadline. */
    if (checkDeadlines(transport) < 0 || replenish(transport) < 0 || submitPending(transport) < 0) return -1;
    return processed;
}

urmaIoResult urmaTransportWrite(urmaTransport *transport, const void *data, size_t length) {
    if (!owned(transport->runtime) || !transport->connected || transport->failed) return (urmaIoResult){URMA_IO_ERROR, 0};
    if (!length) return (urmaIoResult){URMA_IO_OK, 0};
    urmaBuffer *buffer = NULL;
    for (uint32_t i = 0; i < transport->count - 1; i++)
        if (!transport->tx[i].length) {
            buffer = &transport->tx[i];
            break;
        }
    if (!buffer) return (urmaIoResult){URMA_IO_AGAIN, 0};
    urmaFrame frame;
    uint64_t operation;
    urmaIoResult result = urmaSessionWrite(transport->session, data, length, &frame, &operation);
    if (result.status != URMA_IO_OK) return result;
    buffer->operation = operation;
    size_t encoded;
    if (urmaEncodeFrame(buffer->data, transport->stride, &frame, transport->local.max_payload, &encoded) != URMA_PROTOCOL_OK) {
        failTransport(transport, "Encode DATA", EPROTO);
        return result;
    }
    buffer->length = encoded;
    if (startDeadline(transport, buffer) < 0) return result;
    /* Any submission error is latched; the accepted prefix remains consumed. */
    submitPending(transport);
    return result;
}

urmaIoResult urmaTransportRead(urmaTransport *transport, void *data, size_t length) {
    if (!owned(transport->runtime) || !transport->connected || transport->failed) return (urmaIoResult){URMA_IO_ERROR, 0};
    if (!length) return (urmaIoResult){URMA_IO_OK, 0};
    urmaIoResult result = urmaSessionRead(transport->session, data, length);
    replenish(transport);
    submitPending(transport);
    return result;
}

int urmaTransportReadable(const urmaTransport *transport) {
    return transport->failed || (transport->session && urmaSessionReadable(transport->session));
}

int urmaTransportWritable(const urmaTransport *transport) {
    return !transport->failed && transport->session && urmaSessionWritable(transport->session);
}

const char *urmaTransportError(const urmaTransport *transport) {
    return transport->error;
}

int urmaTransportShutdown(urmaTransport *transport) {
    if (!owned(transport->runtime) || !transport->connected || transport->failed) return -1;
    urmaSessionBeginClose(transport->session);
    urmaBuffer *buffer = &transport->tx[transport->count - 1];
    if (!transport->closing) {
        urmaFrame close;
        urmaIoStatus status = urmaSessionMakeClose(transport->session, &close);
        if (status == URMA_IO_AGAIN) return 0;
        size_t length;
        if (status != URMA_IO_OK || urmaEncodeFrame(buffer->data, transport->stride, &close, transport->local.max_payload, &length) != URMA_PROTOCOL_OK)
            return failTransport(transport, "Encode CLOSE", EPROTO);
        buffer->length = length;
        transport->closing = true;
    }
    if (submitPending(transport) < 0) return -1;
    return !buffer->length;
}

int urmaTransportDestroy(urmaTransport *transport) {
    if (!transport) return 1;
    if (!owned(transport->runtime)) return 0;
    /* A normal owner close is cancellation, not a transport error. */
    if (!transport->failed) {
        setError(transport->error, "Transport aborted (status=%d)", ECANCELED);
        transport->failed = true;
        if (transport->session) urmaSessionFail(transport->session);
    }
    for (int i = 0; i < 2; i++) {
        if (transport->jetty[i]) {
            urma_jetty_attr_t attr = {.mask = JETTY_STATE, .state = URMA_JETTY_STATE_ERROR};
            if (urma_modify_jetty(transport->jetty[i], &attr) != URMA_SUCCESS) return 0;
            if (transport->bound[i]) {
                if (urma_unbind_jetty(transport->jetty[i]) != URMA_SUCCESS) return 0;
                transport->bound[i] = false;
            }
            if (urma_delete_jetty(transport->jetty[i]) != URMA_SUCCESS) return 0;
            transport->jetty[i] = NULL;
        }
        if (transport->peer[i]) {
            if (urma_unimport_jetty(transport->peer[i]) != URMA_SUCCESS) return 0;
            transport->peer[i] = NULL;
        }
        if (transport->receive[i]) {
            if (urma_delete_jfr(transport->receive[i]) != URMA_SUCCESS) return 0;
            transport->receive[i] = NULL;
        }
    }
    if (transport->completion) {
        if (urma_delete_jfc(transport->completion) != URMA_SUCCESS) return 0;
        transport->completion = NULL;
    }
    if (transport->event) {
        if (urma_delete_jfce(transport->event) != URMA_SUCCESS) return 0;
        transport->event = NULL;
    }
    if (transport->segment) {
        if (urma_unregister_seg(transport->segment) != URMA_SUCCESS) return 0;
        transport->segment = NULL;
    }
    if (transport->session) {
        for (uint32_t i = 0; i < transport->count; i++)
            if (transport->tx[i].operation) urmaSessionComplete(transport->session, transport->epoch, transport->tx[i].operation, 0);
        if (!urmaSessionFree(transport->session)) return 0;
    }
    transport->runtime->used -= transport->charge;
    transport->runtime->transports--;
    zfree(transport);
    return 1;
}

void urmaTransportRetire(urmaTransport *transport) {
    if (!transport) return;
    urmaRuntime *runtime = transport->runtime;
    if (urmaTransportDestroy(transport)) return;
    transport->next = runtime->retired;
    runtime->retired = transport;
    runtime->retired_count++;
}
#endif /* USE_URMA */

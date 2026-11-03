/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "server.h"
#include "connection.h"

#ifdef USE_URMA
#include "connhelpers.h"
#include "urma_transport.h"
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

#define URMA_HELLO_WIRE_SIZE (URMA_PROTOCOL_HEADER_SIZE + URMA_PROTOCOL_HELLO_SIZE)
#define URMA_HANDSHAKE_LIMIT 64
#define URMA_HANDSHAKE_TIMEOUT 5000

typedef struct {
    connection connection;
    urmaTransport *transport;
    listNode *node;
    uint64_t epoch;
    long long deadline;
    urmaHello peer;
    unsigned char bootstrap[URMA_ENDPOINT_SIZE];
    size_t offset;
    int phase, postpone;
    bool negotiating;
    char error[URMA_ERROR_SIZE];
} urmaConnection;

static ConnectionType CT_Urma;
static urmaRuntime *runtime;
static list *connections;
static unsigned int handshakes;
static long long poll_timer = AE_ERR;
static bool poll_due;
static const urmaHello local_hello = {0, 0, 4096, 16};

static void urmaEvent(aeEventLoop *loop, int fd, void *data, int mask);

static int connectionError(urmaConnection *urma, int code, const char *message) {
    urma->connection.last_errno = code;
    urma->connection.state = CONN_STATE_ERROR;
    snprintf(urma->error, sizeof(urma->error), "%s", message);
    return C_ERR;
}

static int updateBootstrapEvents(urmaConnection *urma) {
    connection *conn = &urma->connection;
    aeDeleteFileEvent(server.el, conn->fd, AE_READABLE | AE_WRITABLE);
    int mask = conn->state == CONN_STATE_ACCEPTING && !(urma->phase & 1) ? AE_WRITABLE : AE_READABLE;
    if (aeCreateFileEvent(server.el, conn->fd, mask | connGetAEPriorityFlag(conn), urmaEvent, urma) == AE_ERR)
        return connectionError(urma, errno, "Cannot register URMA bootstrap event");
    return C_OK;
}

static int finishAccept(urmaConnection *urma) {
    if (urma->negotiating) {
        handshakes--;
        urma->negotiating = false;
    }
    ConnectionCallbackFunc handler = urma->connection.conn_handler;
    urma->connection.conn_handler = NULL; /* Exactly once, including failure. */
    return callHandler(&urma->connection, handler);
}

/* The only bytes permitted on TCP are HELLO, endpoints and RDY1. Once READY,
 * even a single application byte on TCP is a protocol error, never fallback. */
static void driveHandshake(urmaConnection *urma) {
    connection *conn = &urma->connection;
    for (int step = 0; step < 8 && conn->state == CONN_STATE_ACCEPTING; step++) {
        if (mstime() >= urma->deadline) {
            connectionError(urma, ETIMEDOUT, "URMA bootstrap timeout");
            break;
        }
        size_t length = urma->phase < 2 ? URMA_HELLO_WIRE_SIZE : urma->phase < 4 ? URMA_ENDPOINT_SIZE
                                                                                 : 4;
        ssize_t count;
        if (urma->phase & 1)
            count = recv(conn->fd, urma->bootstrap + urma->offset, length - urma->offset, MSG_DONTWAIT);
        else
            count = send(conn->fd, urma->bootstrap + urma->offset, length - urma->offset, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (count <= 0) {
            connectionError(urma, count ? errno : ECONNRESET, "URMA bootstrap disconnected");
            break;
        }
        urma->offset += count;
        if (urma->offset != length) break;
        urma->offset = 0;
        switch (urma->phase++) {
        case 1: {
            urmaFrame frame;
            urmaNegotiated negotiated;
            if (urmaDecodeFrame(urma->bootstrap, length, local_hello.max_payload, &frame) != URMA_PROTOCOL_OK ||
                frame.session_id != urma->epoch || urmaDecodeHello(&frame, &urma->peer) != URMA_PROTOCOL_OK ||
                urmaNegotiateHello(&local_hello, &urma->peer, &negotiated) != URMA_PROTOCOL_OK) {
                connectionError(urma, EPROTO, "Invalid URMA HELLO");
                break;
            }
            urma->transport = urmaTransportCreate(runtime, urma->epoch, &local_hello, urma->error);
            if (!urma->transport) {
                conn->state = CONN_STATE_ERROR;
                break;
            }
            urmaTransportEndpoint(urma->transport, urma->bootstrap);
            break;
        }
        case 3:
            if (urmaTransportConnect(urma->transport, urma->bootstrap, &urma->peer) != 0) {
                connectionError(urma, EPROTO, urmaTransportError(urma->transport));
                break;
            }
            memcpy(urma->bootstrap, "RDY1", 4);
            break;
        case 4:
            conn->state = CONN_STATE_CONNECTED;
            break;
        }
    }
    if (conn->state != CONN_STATE_ERROR) updateBootstrapEvents(urma);
    if (conn->state == CONN_STATE_ERROR) aeDeleteFileEvent(server.el, conn->fd, AE_READABLE | AE_WRITABLE);
    if (conn->state != CONN_STATE_ACCEPTING) finishAccept(urma);
}

static void urmaEvent(aeEventLoop *loop, int fd, void *data, int mask) {
    UNUSED(loop);
    UNUSED(fd);
    UNUSED(mask);
    urmaConnection *urma = data;
    if (server.in_fork_child != CHILD_TYPE_NONE) return;
    if (urma->connection.state == CONN_STATE_ACCEPTING) {
        driveHandshake(urma);
        return;
    }
    unsigned char byte;
    ssize_t count = recv(urma->connection.fd, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
    connectionError(urma, count > 0 ? EPROTO : ECONNRESET,
                    count > 0 ? "Application bytes on URMA bootstrap socket" : "URMA bootstrap closed");
    aeDeleteFileEvent(server.el, urma->connection.fd, AE_READABLE | AE_WRITABLE);
    poll_due = true;
}

static connection *createUrma(void) {
    urmaConnection *urma = zcalloc(sizeof(*urma));
    urma->connection.type = &CT_Urma;
    urma->connection.fd = -1;
    urma->connection.iovcnt = IOV_MAX;
    return &urma->connection;
}

static connection *createAcceptedUrma(int fd, void *private_data) {
    UNUSED(private_data);
    connection *conn = createUrma();
    conn->fd = fd;
    conn->state = CONN_STATE_ACCEPTING;
    return conn;
}

static int acceptUrma(connection *conn, ConnectionCallbackFunc handler) {
    urmaConnection *urma = (urmaConnection *)conn;
    if (handshakes >= URMA_HANDSHAKE_LIMIT)
        return connectionError(urma, EBUSY, "URMA handshake admission limit reached");
    ssize_t count;
    do {
        count = getrandom(&urma->epoch, sizeof(urma->epoch), 0);
    } while (count < 0 && errno == EINTR);
    if (count != sizeof(urma->epoch) || !urma->epoch)
        return connectionError(urma, EIO, "Cannot generate URMA session epoch");
    size_t length;
    if (urmaEncodeHello(urma->bootstrap, sizeof(urma->bootstrap), urma->epoch, &local_hello, &length) != URMA_PROTOCOL_OK)
        return connectionError(urma, EPROTO, "Cannot encode URMA HELLO");
    conn->conn_handler = handler;
    urma->deadline = mstime() + URMA_HANDSHAKE_TIMEOUT;
    if (updateBootstrapEvents(urma) != C_OK) return C_ERR;
    urma->negotiating = true;
    handshakes++;
    listAddNodeTail(connections, urma);
    urma->node = listLast(connections);
    return C_OK;
}

static void closeUrma(connection *conn) {
    urmaConnection *urma = (urmaConnection *)conn;
    if (conn->fd != -1) {
        aeDeleteFileEvent(server.el, conn->fd, AE_READABLE | AE_WRITABLE);
        close(conn->fd);
        conn->fd = -1;
    }
    if (connHasRefs(conn)) {
        conn->flags |= CONN_FLAG_CLOSE_SCHEDULED;
        return;
    }
    if (urma->node) listDelNode(connections, urma->node);
    if (urma->negotiating) handshakes--;
    if (server.in_fork_child == CHILD_TYPE_NONE) urmaTransportRetire(urma->transport);
    zfree(urma);
}

static int resultToConnection(urmaConnection *urma, urmaIoResult result) {
    if (result.status == URMA_IO_OK) return result.bytes;
    if (result.status == URMA_IO_AGAIN) {
        errno = EAGAIN;
        return -1;
    }
    if (result.status == URMA_IO_EOF) {
        urma->connection.state = CONN_STATE_CLOSED;
        return 0;
    }
    return connectionError(urma, EIO, urmaTransportError(urma->transport));
}

static int canIO(urmaConnection *urma) {
    if (server.in_fork_child != CHILD_TYPE_NONE) return 0;
    return urma->connection.state == CONN_STATE_CONNECTED && urma->transport;
}

static int readUrma(connection *conn, void *data, size_t length) {
    urmaConnection *urma = (urmaConnection *)conn;
    if (!canIO(urma)) return -1;
    if (length > INT_MAX) length = INT_MAX;
    return resultToConnection(urma, urmaTransportRead(urma->transport, data, length));
}

static int writeUrma(connection *conn, const void *data, size_t length) {
    urmaConnection *urma = (urmaConnection *)conn;
    if (!canIO(urma)) return -1;
    return resultToConnection(urma, urmaTransportWrite(urma->transport, data, length));
}

static int writevUrma(connection *conn, const struct iovec *iov, int count) {
    int written = 0;
    for (int i = 0; i < count; i++) {
        if (!iov[i].iov_len) continue;
        int result = writeUrma(conn, iov[i].iov_base, iov[i].iov_len);
        if (result < 0) return written ? written : result;
        written += result;
        if ((size_t)result != iov[i].iov_len) break;
    }
    return written;
}

static int setReadHandler(connection *conn, ConnectionCallbackFunc handler) {
    conn->read_handler = handler;
    return C_OK;
}
static int setWriteHandler(connection *conn, ConnectionCallbackFunc handler, int barrier) {
    conn->write_handler = handler;
    if (barrier)
        conn->flags |= CONN_FLAG_WRITE_BARRIER;
    else
        conn->flags &= ~CONN_FLAG_WRITE_BARRIER;
    return C_OK;
}
static const char *lastError(connection *conn) {
    return ((urmaConnection *)conn)->error;
}
static int urmaType(void) {
    return CONN_TYPE_URMA;
}
static int urmaAddress(connection *conn, char *ip, size_t size, int *port, int remote) {
    return anetFdToString(conn->fd, ip, size, port, remote);
}
/* Bootstrap locality does not attest to the origin of native data frames. */
static int urmaLocal(connection *conn) {
    UNUSED(conn);
    return 0;
}
static int isClosing(connection *conn) {
    return conn->state == CONN_STATE_ERROR || conn->state == CONN_STATE_CLOSED;
}

static int ready(const urmaConnection *urma, int write) {
    if (urma->connection.state == CONN_STATE_ACCEPTING || urma->connection.flags & CONN_FLAG_POSTPONE_UPDATE_STATE) return 0;
    if (urma->connection.state != CONN_STATE_CONNECTED) return 1;
    if (write && (urma->postpone & CONN_POSTPONE_WRITE)) return 0;
    if (!write && (urma->postpone & CONN_POSTPONE_READ)) return 0;
    return write ? urmaTransportWritable(urma->transport) : urmaTransportReadable(urma->transport);
}

static int hasPending(void) {
    if (!connections || server.in_fork_child != CHILD_TYPE_NONE) return 0;
    if (poll_due && listLength(connections)) return 1;
    listIter iterator;
    listNode *node;
    listRewind(connections, &iterator);
    while ((node = listNext(&iterator))) {
        urmaConnection *urma = listNodeValue(node);
        if ((urma->connection.read_handler && ready(urma, 0)) || (urma->connection.write_handler && ready(urma, 1))) return 1;
    }
    return 0;
}

static int processPending(void) {
    if (!connections || server.in_fork_child != CHILD_TYPE_NONE) return 0;
    bool poll = poll_due;
    poll_due = false;
    unsigned long remaining = listLength(connections);
    int processed = 0;
    while (remaining-- && listLength(connections)) {
        listNode *node = listFirst(connections);
        urmaConnection *urma = listNodeValue(node);
        connection *conn = &urma->connection;
        listUnlinkNode(connections, node);
        listLinkNodeTail(connections, node);
        if (conn->state == CONN_STATE_ACCEPTING) {
            if (mstime() >= urma->deadline) {
                connectionError(urma, ETIMEDOUT, "URMA bootstrap timeout");
                aeDeleteFileEvent(server.el, conn->fd, AE_READABLE | AE_WRITABLE);
                finishAccept(urma);
                processed++;
            }
            continue;
        }
        if (poll && conn->state == CONN_STATE_CONNECTED && urmaTransportProgress(urma->transport, 64) < 0)
            connectionError(urma, EIO, urmaTransportError(urma->transport));
        bool read = conn->read_handler && ready(urma, 0);
        bool write = conn->write_handler && ready(urma, 1);
        bool barrier = conn->flags & CONN_FLAG_WRITE_BARRIER;
        if (!barrier && read && !callHandler(conn, conn->read_handler)) {
            processed++;
            continue;
        }
        if (write && !callHandler(conn, conn->write_handler)) {
            processed++;
            continue;
        }
        if (barrier && read) callHandler(conn, conn->read_handler);
        if (read || write) processed++;
    }
    if (poll) urmaRuntimeReap(runtime);
    return processed;
}

static long long pollTick(aeEventLoop *loop, long long id, void *data) {
    UNUSED(loop);
    UNUSED(id);
    UNUSED(data);
    poll_due = true;
    return 1; /* Experimental polling mode, not a hardware event integration. */
}

static int listenUrma(connListener *listener) {
    if (server.saveparamslen || server.aof_enabled || server.primary_host) {
        serverLog(LL_WARNING, "Native URMA experiment requires save empty, appendonly no and no primary replication link; fork/persistence are not qualified");
        return C_ERR;
    }
    if (!server.urma_device || !server.urma_allow_insecure_bootstrap || server.io_threads_num != 1 ||
        server.cluster_enabled || server.tls_replication) {
        serverLog(LL_WARNING, "URMA requires urma-device, explicit urma-allow-insecure-bootstrap yes, io-threads 1; cluster and TLS replication combinations are not qualified");
        return C_ERR;
    }
    char error[URMA_ERROR_SIZE];
    runtime = urmaRuntimeOpen(server.urma_device, server.urma_eid_index, server.urma_buffer_limit, error);
    if (!runtime) {
        serverLog(LL_WARNING, "URMA: %s", error);
        return C_ERR;
    }
    if (listenToPort(listener) == C_ERR) return C_ERR;
    poll_timer = aeCreateTimeEvent(server.el, 1, pollTick, NULL, NULL);
    if (poll_timer == AE_ERR) return C_ERR;
    return C_OK;
}

static void closeListenerUrma(connListener *listener) {
    for (int i = 0; i < listener->count; i++) {
        aeDeleteFileEvent(server.el, listener->fd[i], AE_READABLE);
        close(listener->fd[i]);
    }
    listener->count = 0;
}

static void acceptHandler(aeEventLoop *loop, int fd, void *data, int mask) {
    UNUSED(loop);
    UNUSED(data);
    UNUSED(mask);
    int remaining = server.max_new_conns_per_cycle;
    while (remaining--) {
        char ip[NET_IP_STR_LEN];
        int port;
        int accepted = anetTcpAccept(server.neterr, fd, ip, sizeof(ip), &port);
        if (accepted == ANET_ERR) {
            if (anetRetryAcceptOnError(errno)) continue;
            return;
        }
        if (handshakes >= URMA_HANDSHAKE_LIMIT) {
            close(accepted);
            server.stat_rejected_conn++;
            continue;
        }
        anetNonBlock(NULL, accepted);
        anetEnableTcpNoDelay(NULL, accepted);
        if (server.tcpkeepalive) anetKeepAlive(NULL, accepted, server.tcpkeepalive);
        struct ClientFlags flags = {0};
        acceptCommonHandler(createAcceptedUrma(accepted, NULL), flags, ip);
    }
}

static void initUrma(void) {
    connections = listCreate();
}
static void cleanupUrma(void) {
    if (poll_timer != AE_ERR) aeDeleteTimeEvent(server.el, poll_timer);
    if (runtime && !urmaRuntimeClose(runtime)) serverLog(LL_WARNING, "URMA resources retained: deletion/quiescence incomplete");
    if (connections && !listLength(connections)) {
        listRelease(connections);
        connections = NULL;
    }
}
static void shutdownUrma(connection *conn) {
    if (canIO((urmaConnection *)conn)) urmaTransportShutdown(((urmaConnection *)conn)->transport);
}
static void postponeUrma(connection *conn, int mask) {
    ((urmaConnection *)conn)->postpone = mask;
}
static void updateUrma(connection *conn) {
    UNUSED(conn);
    poll_due = true;
}

/* Outgoing/internal links and synchronous I/O are intentionally unavailable
 * in this inbound-only experiment. They must never silently select TCP. */
static int connectUrma(connection *conn, const char *address, int port, const char *source, int multipath, ConnectionCallbackFunc handler) {
    UNUSED(address);
    UNUSED(port);
    UNUSED(source);
    UNUSED(multipath);
    UNUSED(handler);
    return connectionError((urmaConnection *)conn, ENOTSUP, "Outgoing URMA connections are not implemented");
}
static int blockingConnectUrma(connection *conn, const char *address, int port, long long timeout) {
    UNUSED(address);
    UNUSED(port);
    UNUSED(timeout);
    return connectionError((urmaConnection *)conn, ENOTSUP, "Blocking URMA connect is not implemented");
}
static ssize_t syncUnsupported(connection *conn, char *data, ssize_t size, long long timeout) {
    UNUSED(data);
    UNUSED(size);
    UNUSED(timeout);
    conn->last_errno = ENOTSUP;
    return -1;
}

static ConnectionType CT_Urma = {
    .get_type = urmaType,
    .init = initUrma,
    .cleanup = cleanupUrma,
    .ae_handler = urmaEvent,
    .accept_handler = acceptHandler,
    .addr = urmaAddress,
    .is_local = urmaLocal,
    .listen = listenUrma,
    .closeListener = closeListenerUrma,
    .conn_create = createUrma,
    .conn_create_accepted = createAcceptedUrma,
    .shutdown = shutdownUrma,
    .close = closeUrma,
    .connect = connectUrma,
    .blocking_connect = blockingConnectUrma,
    .accept = acceptUrma,
    .write = writeUrma,
    .writev = writevUrma,
    .read = readUrma,
    .set_write_handler = setWriteHandler,
    .set_read_handler = setReadHandler,
    .get_last_error = lastError,
    .sync_write = syncUnsupported,
    .sync_read = syncUnsupported,
    .sync_readline = syncUnsupported,
    .has_pending_data = hasPending,
    .process_pending_data = processPending,
    .postpone_update_state = postponeUrma,
    .update_state = updateUrma,
    .is_closing = isClosing,
};
int RegisterConnectionTypeUrma(void) {
    return connTypeRegister(&CT_Urma);
}
#else
int RegisterConnectionTypeUrma(void) {
    return C_ERR;
}
#endif /* USE_URMA */

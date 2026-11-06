/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "crc64.h"
#include "probe_io.h"
#include "remote_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>

#define VALUE_SIZE 65537
#define WIRE_SIZE (REMOTE_CACHE_HEADER_SIZE + REMOTE_VALUE_MAX_SIZE)

/* One client, four cache requests. This acceptance probe is not a cache
 * daemon, a DB command adapter, or a persistence/availability mechanism. */
static int listenOnce(const char *host, const char *port) {
  struct addrinfo hints = {.ai_family = AF_UNSPEC,
                           .ai_socktype = SOCK_STREAM,
                           .ai_flags = AI_NUMERICHOST | AI_NUMERICSERV};
  struct addrinfo *addresses = NULL;
  if (getaddrinfo(host, port, &hints, &addresses))
    return -1;
  int listener = -1;
  for (struct addrinfo *address = addresses; address;
       address = address->ai_next) {
    listener = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (listener < 0)
      continue;
    if (!bind(listener, address->ai_addr, address->ai_addrlen) &&
        !listen(listener, 1))
      break;
    close(listener);
    listener = -1;
  }
  freeaddrinfo(addresses);
  if (listener < 0)
    return -1;
  struct pollfd event = {.fd = listener, .events = POLLIN};
  int fd = poll(&event, 1, 30000) > 0 ? accept(listener, NULL, NULL) : -1;
  close(listener);
  if (fd >= 0 && fcntl(fd, F_SETFL, O_NONBLOCK) < 0) {
    close(fd);
    fd = -1;
  }
  return fd;
}

static urmaTransport *handshake(urmaRuntime *runtime, int fd, int server,
                                char *error) {
  unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + URMA_PROTOCOL_HELLO_SIZE];
  unsigned char endpoint[URMA_ENDPOINT_SIZE], remote[URMA_ENDPOINT_SIZE];
  urmaHello local = {0, 0, 4096, 16}, peer;
  urmaFrame frame;
  size_t length;
  uint64_t epoch = 0;
  if (server) {
    if (getrandom(&epoch, sizeof(epoch), 0) != sizeof(epoch) || !epoch ||
        urmaEncodeHello(wire, sizeof(wire), epoch, &local, &length) !=
            URMA_PROTOCOL_OK ||
        bootstrapIO(fd, wire, length, 1))
      return NULL;
  }
  if (bootstrapIO(fd, wire, sizeof(wire), 0) ||
      urmaDecodeFrame(wire, sizeof(wire), URMA_PROTOCOL_MAX_PAYLOAD, &frame) !=
          URMA_PROTOCOL_OK ||
      urmaDecodeHello(&frame, &peer) != URMA_PROTOCOL_OK ||
      (server && frame.session_id != epoch))
    return NULL;
  epoch = frame.session_id;
  urmaTransport *transport = urmaTransportCreate(runtime, epoch, &local, error);
  if (!transport)
    return NULL;
  if (!server && (urmaEncodeHello(wire, sizeof(wire), epoch, &local, &length) !=
                      URMA_PROTOCOL_OK ||
                  bootstrapIO(fd, wire, length, 1)))
    goto failed;
  urmaTransportEndpoint(transport, endpoint);
  if (server) {
    if (bootstrapIO(fd, endpoint, sizeof(endpoint), 1) ||
        bootstrapIO(fd, remote, sizeof(remote), 0))
      goto failed;
  } else {
    if (bootstrapIO(fd, remote, sizeof(remote), 0) ||
        bootstrapIO(fd, endpoint, sizeof(endpoint), 1))
      goto failed;
  }
  if (urmaTransportConnect(transport, remote, &peer))
    goto failed;
  if (server) {
    memcpy(wire, "RDY1", 4);
    if (bootstrapIO(fd, wire, 4, 1))
      goto failed;
  } else if (bootstrapIO(fd, wire, 4, 0) || memcmp(wire, "RDY1", 4)) {
    goto failed;
  }
  return transport;
failed:
  if (!urmaTransportDestroy(transport))
    urmaTransportRetire(transport);
  return NULL;
}

static int readMessage(urmaTransport *transport, unsigned char *wire,
                       remoteCacheMessage *message) {
  size_t length;
  if (streamIO(transport, wire, REMOTE_CACHE_HEADER_SIZE, 0) ||
      remoteCacheFrameSize(wire, REMOTE_CACHE_HEADER_SIZE, &length) !=
          REMOTE_CACHE_OK ||
      length > WIRE_SIZE ||
      streamIO(transport, wire + REMOTE_CACHE_HEADER_SIZE,
               length - REMOTE_CACHE_HEADER_SIZE, 0))
    return -1;
  return remoteCacheDecode(wire, length, message) == REMOTE_CACHE_OK ? 0 : -1;
}

static int exchange(urmaTransport *transport, unsigned char *wire,
                    const remoteCacheMessage *request,
                    remoteCacheMessage *reply) {
  size_t length;
  if (remoteCacheEncode(wire, WIRE_SIZE, request, &length) != REMOTE_CACHE_OK ||
      streamIO(transport, wire, length, 1))
    return -1;
  return readMessage(transport, wire, reply);
}

static int serve(urmaTransport *transport, unsigned char *wire) {
  unsigned char *arena = malloc(2 * VALUE_SIZE);
  uint64_t epoch = 0;
  remoteValuePool *pool = NULL;
  int result = 1;
  if (!arena || getrandom(&epoch, sizeof(epoch), 0) != sizeof(epoch) || !epoch)
    goto done;
  pool = remoteValuePoolCreate(arena, 2 * VALUE_SIZE, VALUE_SIZE, epoch);
  if (!pool)
    goto done;
  for (unsigned int i = 0; i < 4; i++) {
    remoteCacheMessage request, reply;
    remoteValueView view = {0};
    size_t length;
    if (readMessage(transport, wire, &request) || request.response)
      goto done;
    long long now = milliseconds();
    if (now < 0)
      goto done;
    remoteCacheApply(pool, &request, now, &reply, &view);
    int encoded =
        remoteCacheEncode(wire, WIRE_SIZE, &reply, &length) == REMOTE_CACHE_OK;
    /* The wire copy owns all transmitted bytes; the pool view is never
     * handed to the URMA runtime/DMA engine. */
    if (view.pool)
      remoteValueRelease(&view);
    if (!encoded || streamIO(transport, wire, length, 1))
      goto done;
  }
  remoteValueStats stats;
  remoteValuePoolStats(pool, &stats);
  printf("Cache pool: capacity=%zu metadata=%zu live=%zu puts=%llu hits=%llu "
         "misses=%llu\n",
         stats.capacity_bytes, stats.metadata_bytes, stats.live_bytes,
         (unsigned long long)stats.puts, (unsigned long long)stats.hits,
         (unsigned long long)stats.misses);
  result = 0;
done:
  if (!remoteValuePoolFree(pool))
    return 1;
  free(arena);
  return result;
}

static int checkCache(urmaTransport *transport, unsigned char *wire) {
  unsigned char *authority = malloc(VALUE_SIZE), *output = malloc(VALUE_SIZE);
  int result = 1;
  if (!authority || !output)
    goto done;
  for (uint32_t i = 0; i < VALUE_SIZE; i++)
    authority[i] = (i * 37) & 255;
  remoteCacheMessage put = {
      .opcode = REMOTE_CACHE_PUT,
      .request_id = 1,
      .identity = {.owner = 1, .object = 1},
      .version = 1,
      .ref = {.length = VALUE_SIZE,
              .checksum = crc64(0, authority, VALUE_SIZE)},
      .ttl = 60000,
      .payload_length = VALUE_SIZE,
      .payload = authority};
  remoteCacheMessage reply;
  remoteValueRef ref;
  if (exchange(transport, wire, &put, &reply) ||
      !remoteCacheAcceptPut(&put, &reply, put.identity, put.version, &ref))
    goto done;
  remoteCacheMessage get = {.opcode = REMOTE_CACHE_GET,
                            .request_id = 2,
                            .identity = put.identity,
                            .version = 2,
                            .ref = ref};
  /* A stale source version is a MISS, never a successful empty Value. */
  if (exchange(transport, wire, &get, &reply) || !reply.response ||
      reply.request_id != 2 || reply.opcode != REMOTE_CACHE_GET ||
      reply.status != REMOTE_VALUE_MISS)
    goto done;
  get.version = 1;
  get.request_id = 3;
  size_t written;
  if (exchange(transport, wire, &get, &reply) ||
      !remoteCacheAcceptGet(&get, &reply, put.identity, put.version, output,
                            VALUE_SIZE, &written) ||
      written != VALUE_SIZE || memcmp(output, authority, VALUE_SIZE))
    goto done;
  get.opcode = REMOTE_CACHE_DROP;
  get.request_id = 4;
  if (exchange(transport, wire, &get, &reply) || !reply.response ||
      reply.request_id != 4 || reply.opcode != REMOTE_CACHE_DROP ||
      reply.status != REMOTE_VALUE_OK)
    goto done;
  for (uint32_t i = 0; i < VALUE_SIZE; i++) {
    if (authority[i] != ((i * 37) & 255))
      goto done;
  }
  result = 0;
done:
  free(authority);
  free(output);
  return result;
}

static int finish(urmaTransport *transport) {
  long long now = milliseconds();
  if (now < 0 || now > LLONG_MAX - 10000)
    return -1;
  long long deadline = now + 10000;
  int peer_closed = 0;
  while ((now = milliseconds()) >= 0 && now < deadline) {
    if (urmaTransportProgress(transport, 64) < 0)
      return -1;
    int sent = urmaTransportShutdown(transport);
    if (sent < 0)
      return -1;
    if (!peer_closed) {
      unsigned char unexpected;
      urmaIoResult result = urmaTransportRead(transport, &unexpected, 1);
      if (result.status == URMA_IO_EOF)
        peer_closed = 1;
      if (result.status == URMA_IO_ERROR || result.bytes)
        return -1;
    }
    if (sent && peer_closed)
      return 0;
    usleep(1000);
  }
  return -1;
}

int main(int argc, char **argv) {
  if (argc != 7 || (strcmp(argv[1], "--serve") && strcmp(argv[1], "--check")) ||
      strcmp(argv[6], "--allow-insecure-bootstrap")) {
    fprintf(stderr,
            "Usage: %s --serve|--check HOST PORT DEVICE EID_INDEX "
            "--allow-insecure-bootstrap\n",
            argv[0]);
    return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
  }
  char *end;
  errno = 0;
  unsigned long index = strtoul(argv[5], &end, 10);
  if (errno || !*argv[5] || *end || index > UINT32_MAX || argv[5][0] == '-')
    return 2;
  unsigned long port = strtoul(argv[3], &end, 10);
  if (errno || !*argv[3] || *end || !port || port > 65535 || argv[3][0] == '-')
    return 2;
  int server = !strcmp(argv[1], "--serve");
  char error[URMA_ERROR_SIZE] = {0};
  urmaRuntime *runtime =
      urmaRuntimeOpen(argv[4], index, 16 * 1024 * 1024, error);
  if (!runtime) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  crc64_init();
  int fd = server ? listenOnce(argv[2], argv[3])
                  : connectBootstrap(argv[2], argv[3]);
  urmaTransport *transport =
      fd >= 0 ? handshake(runtime, fd, server, error) : NULL;
  unsigned char *wire = malloc(WIRE_SIZE);
  int result = 1;
  if (transport && wire &&
      !(server ? serve(transport, wire) : checkCache(transport, wire)) &&
      !finish(transport))
    result = 0;
  if (result)
    fprintf(stderr, "Cache probe failed: %s\n",
            transport ? urmaTransportError(transport) : error);
  if (fd >= 0)
    close(fd);
  if (!urmaTransportDestroy(transport)) {
    result = 1;
    urmaTransportRetire(transport);
  }
  if (!urmaRuntimeClose(runtime))
    result = 1;
  free(wire);
  if (!result)
    puts("Native URMA rebuildable cache PUT/version-MISS/GET/DROP: PASS (local "
         "authority retained)");
  return result;
}

/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "urma_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "probe_io.h"

static int expectReply(urmaTransport *transport, const char *expected) {
  char reply[64];
  size_t length = strlen(expected);
  return length <= sizeof(reply) && !streamIO(transport, reply, length, 0) &&
                 !memcmp(reply, expected, length)
             ? 0
             : -1;
}

static int checkCommands(urmaTransport *transport) {
  const char *password = getenv("VALKEY_URMA_PASSWORD");
  const char *username = getenv("VALKEY_URMA_USER");
  char command[4096];
  if (password) {
    if (!username)
      username = "default";
    int length =
        snprintf(command, sizeof(command),
                 "*3\r\n$4\r\nAUTH\r\n$%zu\r\n%s\r\n$%zu\r\n%s\r\n",
                 strlen(username), username, strlen(password), password);
    if (length < 0 || (size_t)length >= sizeof(command) ||
        streamIO(transport, command, length, 1) ||
        expectReply(transport, "+OK\r\n"))
      return -1;
    memset(command, 0, sizeof(command));
  }
  char ping[] = "*1\r\n$4\r\nPING\r\n";
  if (streamIO(transport, ping, strlen(ping), 1) ||
      expectReply(transport, "+PONG\r\n"))
    return -1;
  uint64_t id;
  if (getrandom(&id, sizeof(id), 0) != sizeof(id))
    return -1;
  char key[64];
  snprintf(key, sizeof(key), "__urma_probe:%016llx", (unsigned long long)id);
  const size_t value_size = 65537;
  unsigned char *value = malloc(value_size), *reply = malloc(value_size);
  if (!value || !reply) {
    free(value);
    free(reply);
    return -1;
  }
  for (size_t i = 0; i < value_size; i++)
    value[i] = (i * 37) & 255;
  int length = snprintf(command, sizeof(command),
                        "*3\r\n$3\r\nSET\r\n$%zu\r\n%s\r\n$%zu\r\n",
                        strlen(key), key, value_size);
  int result = streamIO(transport, command, length, 1) ||
               streamIO(transport, value, value_size, 1) ||
               streamIO(transport, "\r\n", 2, 1) ||
               expectReply(transport, "+OK\r\n");
  length = snprintf(command, sizeof(command),
                    "*2\r\n$3\r\nGET\r\n$%zu\r\n%s\r\n", strlen(key), key);
  if (!result)
    result = streamIO(transport, command, length, 1) ||
             expectReply(transport, "$65537\r\n") ||
             streamIO(transport, reply, value_size, 0) ||
             memcmp(reply, value, value_size) || expectReply(transport, "\r\n");
  length = snprintf(command, sizeof(command),
                    "*2\r\n$3\r\nDEL\r\n$%zu\r\n%s\r\n", strlen(key), key);
  if (!result)
    result = streamIO(transport, command, length, 1) ||
             expectReply(transport, ":1\r\n");
  free(value);
  free(reply);
  return result ? -1 : 0;
}

int main(int argc, char **argv) {
  if (argc != 6 || strcmp(argv[5], "--allow-insecure-bootstrap")) {
    fprintf(stderr,
            "Usage: %s HOST PORT DEVICE EID_INDEX --allow-insecure-bootstrap\n",
            argv[0]);
    return 2;
  }
  char *end;
  errno = 0;
  unsigned long index = strtoul(argv[4], &end, 10);
  if (errno || !*argv[4] || *end || index > UINT32_MAX || argv[4][0] == '-')
    return 2;
  char error[URMA_ERROR_SIZE];
  urmaRuntime *runtime =
      urmaRuntimeOpen(argv[3], index, 16 * 1024 * 1024, error);
  if (!runtime) {
    fprintf(stderr, "%s\n", error);
    return 1;
  }
  int fd = connectBootstrap(argv[1], argv[2]);
  urmaTransport *transport = NULL;
  int result = 1;
  unsigned char wire[URMA_PROTOCOL_HEADER_SIZE + URMA_PROTOCOL_HELLO_SIZE];
  urmaFrame frame;
  urmaHello peer;
  if (fd < 0 || bootstrapIO(fd, wire, sizeof(wire), 0) ||
      urmaDecodeFrame(wire, sizeof(wire), URMA_PROTOCOL_MAX_PAYLOAD, &frame) !=
          URMA_PROTOCOL_OK ||
      urmaDecodeHello(&frame, &peer) != URMA_PROTOCOL_OK)
    goto done;
  uint64_t epoch = frame.session_id;
  urmaHello local = {0, 0, 4096, 16};
  transport = urmaTransportCreate(runtime, epoch, &local, error);
  if (!transport) {
    fprintf(stderr, "%s\n", error);
    goto done;
  }
  size_t length;
  if (urmaEncodeHello(wire, sizeof(wire), epoch, &local, &length) !=
          URMA_PROTOCOL_OK ||
      bootstrapIO(fd, wire, length, 1))
    goto done;
  unsigned char endpoint[URMA_ENDPOINT_SIZE], remote[URMA_ENDPOINT_SIZE];
  if (bootstrapIO(fd, remote, sizeof(remote), 0))
    goto done;
  urmaTransportEndpoint(transport, endpoint);
  if (bootstrapIO(fd, endpoint, sizeof(endpoint), 1) ||
      urmaTransportConnect(transport, remote, &peer))
    goto done;
  if (bootstrapIO(fd, wire, 4, 0) || memcmp(wire, "RDY1", 4))
    goto done;
  if (checkCommands(transport))
    goto done;
  result = 0;
done:
  if (result)
    fprintf(stderr,
            "Native probe failed (bootstrap/command/timeout); transport: %s\n",
            transport ? urmaTransportError(transport) : "not connected");
  if (fd >= 0)
    close(fd);
  if (!urmaTransportDestroy(transport)) {
    fputs("URMA resource deletion incomplete\n", stderr);
    result = 1;
    urmaTransportRetire(transport);
  }
  if (!urmaRuntimeClose(runtime)) {
    fputs("URMA runtime cleanup incomplete\n", stderr);
    result = 1;
  }
  if (!result)
    puts("Native URMA PING and binary 65537-byte SET/GET/DEL: PASS");
  return result;
}

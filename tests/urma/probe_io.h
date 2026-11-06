/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef VALKEY_URMA_PROBE_IO_H
#define VALKEY_URMA_PROBE_IO_H
#include "urma_transport.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static inline long long milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 ||
      (uint64_t)now.tv_sec > (uint64_t)(LLONG_MAX - 1000) / 1000)
    return -1;
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static inline int connectBootstrap(const char *host, const char *port) {
  struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM},
                  *addresses = NULL;
  if (getaddrinfo(host, port, &hints, &addresses))
    return -1;
  int fd = -1;
  for (struct addrinfo *address = addresses; address;
       address = address->ai_next) {
    fd = socket(address->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0)
      continue;
    int result = connect(fd, address->ai_addr, address->ai_addrlen);
    if (result < 0 && errno == EINPROGRESS) {
      struct pollfd event = {.fd = fd, .events = POLLOUT};
      int error = 0;
      socklen_t size = sizeof(error);
      result =
          poll(&event, 1, 5000) > 0 &&
                  getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 &&
                  !error
              ? 0
              : -1;
    }
    if (!result)
      break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  return fd;
}

static inline int bootstrapIO(int fd, unsigned char *buffer, size_t length,
                              int write) {
  size_t offset = 0;
  long long now = milliseconds();
  if (now < 0 || now > LLONG_MAX - 5000)
    return -1;
  long long deadline = now + 5000;
  while (offset < length && (now = milliseconds()) >= 0 && now < deadline) {
    ssize_t count =
        write ? send(fd, buffer + offset, length - offset, MSG_NOSIGNAL)
              : recv(fd, buffer + offset, length - offset, 0);
    if (count > 0) {
      offset += count;
      continue;
    }
    if (!count || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      return -1;
    struct pollfd event = {.fd = fd, .events = write ? POLLOUT : POLLIN};
    poll(&event, 1, 10);
  }
  return offset == length ? 0 : -1;
}

static inline int streamIO(urmaTransport *transport, void *data, size_t length,
                           int write) {
  long long now = milliseconds();
  if (now < 0 || now > LLONG_MAX - 10000)
    return -1;
  long long deadline = now + 10000;
  size_t offset = 0;
  while (offset < length && (now = milliseconds()) >= 0 && now < deadline) {
    if (urmaTransportProgress(transport, 64) < 0)
      return -1;
    urmaIoResult result =
        write ? urmaTransportWrite(transport, (char *)data + offset,
                                   length - offset)
              : urmaTransportRead(transport, (char *)data + offset,
                                  length - offset);
    if (result.status == URMA_IO_ERROR || result.status == URMA_IO_EOF)
      return -1;
    offset += result.bytes;
    if (!result.bytes)
      usleep(1000);
  }
  return offset == length ? 0 : -1;
}

#endif

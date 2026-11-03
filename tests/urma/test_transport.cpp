/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <gtest/gtest.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <urma_api.h>
extern "C" {
#include "urma_transport.h"
}

/* A bounded SDK-seam test double. No network, kernel or DMA is simulated.
 * Its only purpose is exercising admission, retries and ownership failures. */
typedef struct {
  urma_jfc_t base;
  urma_cr_t queue[128];
  unsigned int head, tail;
} TestCompletion;
typedef struct {
  urma_jfr_t base;
  urma_sge_t receive[32];
  uint64_t cookies[32];
  unsigned int head, tail;
} TestReceive;
static urma_device_t device;
static urma_jetty_t *jetties[32];
static unsigned int next_id, allocations, attempts, fail_at;
static int block_send, fail_delete, fail_bind, fail_completion, ambiguous_send,
    native_ub = 1;
static int block_receive, suppress_completion, fake_clock, clock_failure;
static uint64_t clock_ms;

static void *allocate(size_t size) {
  if (++attempts == fail_at)
    return NULL;
  void *ptr = calloc(1, size);
  if (ptr)
    allocations++;
  return ptr;
}
static void release(void *ptr) {
  if (ptr) {
    allocations--;
    free(ptr);
  }
}
static void queueCompletion(urma_jfc_t *jfc, const urma_cr_t *completion) {
  TestCompletion *test = (TestCompletion *)jfc;
  ASSERT_LT(test->tail - test->head, 128u);
  test->queue[test->tail++ % 128] = *completion;
}
extern "C" {
int __real_clock_gettime(clockid_t, struct timespec *);
int __wrap_clock_gettime(clockid_t id, struct timespec *out) {
  if (!fake_clock || id != CLOCK_MONOTONIC)
    return __real_clock_gettime(id, out);
  if (clock_failure) {
    errno = EIO;
    return -1;
  }
  out->tv_sec = clock_ms / 1000;
  out->tv_nsec = (clock_ms % 1000) * 1000000;
  return 0;
}
urma_status_t urma_init(urma_init_attr_t *) { return URMA_SUCCESS; }
urma_status_t urma_uninit(void) { return URMA_SUCCESS; }
urma_device_t *urma_get_device_by_name(char *) {
  device.type = native_ub ? URMA_TRANSPORT_UB : URMA_TRANSPORT_SOFTUB;
  return &device;
}
urma_status_t urma_query_device(urma_device_t *, urma_device_attr_t *attr) {
  memset(attr, 0, sizeof(*attr));
  attr->dev_cap.trans_mode = URMA_TM_RC;
  attr->dev_cap.rc_order_cap.bs.ot = 1;
  attr->dev_cap.rc_tp_cap.bs.rtp = 1;
  attr->dev_cap.max_msg_size = 65576;
  attr->dev_cap.max_jfs_depth = attr->dev_cap.max_jfr_depth =
      attr->dev_cap.max_jfc_depth = 4096;
  attr->dev_cap.max_jfs_sge = attr->dev_cap.max_jfr_sge = 1;
  return URMA_SUCCESS;
}
urma_context_t *urma_create_context(urma_device_t *, uint32_t) {
  return (urma_context_t *)allocate(sizeof(urma_context_t));
}
urma_status_t urma_delete_context(urma_context_t *ctx) {
  release(ctx);
  return URMA_SUCCESS;
}
urma_jfce_t *urma_create_jfce(urma_context_t *) {
  return (urma_jfce_t *)allocate(sizeof(urma_jfce_t));
}
urma_status_t urma_delete_jfce(urma_jfce_t *event) {
  release(event);
  return URMA_SUCCESS;
}
urma_jfc_t *urma_create_jfc(urma_context_t *, urma_jfc_cfg_t *) {
  return (urma_jfc_t *)allocate(sizeof(TestCompletion));
}
urma_status_t urma_delete_jfc(urma_jfc_t *jfc) {
  release(jfc);
  return URMA_SUCCESS;
}
urma_target_seg_t *urma_register_seg(urma_context_t *, urma_seg_cfg_t *) {
  return (urma_target_seg_t *)allocate(sizeof(urma_target_seg_t));
}
urma_status_t urma_unregister_seg(urma_target_seg_t *segment) {
  release(segment);
  return URMA_SUCCESS;
}
urma_jfr_t *urma_create_jfr(urma_context_t *, urma_jfr_cfg_t *cfg) {
  TestReceive *test = (TestReceive *)allocate(sizeof(TestReceive));
  if (test)
    test->base.jfr_cfg = *cfg;
  return test ? &test->base : NULL;
}
urma_status_t urma_delete_jfr(urma_jfr_t *jfr) {
  release(jfr);
  return URMA_SUCCESS;
}
urma_jetty_t *urma_create_jetty(urma_context_t *, urma_jetty_cfg_t *cfg) {
  urma_jetty_t *jetty = (urma_jetty_t *)allocate(sizeof(urma_jetty_t));
  if (jetty) {
    jetty->jetty_cfg = *cfg;
    jetty->jetty_id.id = ++next_id;
    jetties[next_id % 32] = jetty;
  }
  return jetty;
}
urma_status_t urma_modify_jetty(urma_jetty_t *, urma_jetty_attr_t *) {
  return URMA_SUCCESS;
}
urma_status_t urma_delete_jetty(urma_jetty_t *jetty) {
  if (fail_delete)
    return URMA_EAGAIN;
  jetties[jetty->jetty_id.id % 32] = NULL;
  release(jetty);
  return URMA_SUCCESS;
}
urma_target_jetty_t *urma_import_jetty(urma_context_t *, urma_rjetty_t *remote,
                                       urma_token_t *) {
  EXPECT_EQ(remote->type, URMA_JETTY);
  EXPECT_EQ(remote->trans_mode, URMA_TM_RC);
  EXPECT_EQ((unsigned int)remote->flag.bs.order_type, (unsigned int)URMA_OT);
  urma_target_jetty_t *target =
      (urma_target_jetty_t *)allocate(sizeof(urma_target_jetty_t));
  if (target)
    target->id = remote->jetty_id;
  return target;
}
urma_status_t urma_unimport_jetty(urma_target_jetty_t *target) {
  release(target);
  return URMA_SUCCESS;
}
urma_status_t urma_bind_jetty(urma_jetty_t *, urma_target_jetty_t *) {
  return fail_bind ? URMA_EINVAL : URMA_SUCCESS;
}
urma_status_t urma_unbind_jetty(urma_jetty_t *) { return URMA_SUCCESS; }
urma_status_t urma_post_jfr_wr(urma_jfr_t *jfr, urma_jfr_wr_t *wr,
                               urma_jfr_wr_t **bad) {
  TestReceive *test = (TestReceive *)jfr;
  if (block_receive || test->tail - test->head == 32) {
    *bad = wr;
    return URMA_EAGAIN;
  }
  test->receive[test->tail % 32] = wr->src.sge[0];
  test->cookies[test->tail++ % 32] = wr->user_ctx;
  return URMA_SUCCESS;
}
urma_status_t urma_post_jetty_send_wr(urma_jetty_t *jetty, urma_jfs_wr_t *wr,
                                      urma_jfs_wr_t **bad) {
  if (ambiguous_send)
    return URMA_EAGAIN;
  if (block_send) {
    *bad = wr;
    return URMA_EAGAIN;
  }
  urma_jetty_t *peer = jetties[wr->tjetty->id.id % 32];
  if (!peer)
    return URMA_EINVAL;
  TestReceive *test = (TestReceive *)peer->jetty_cfg.shared.jfr;
  if (test->tail == test->head) {
    *bad = wr;
    return URMA_EAGAIN;
  }
  urma_sge_t *destination = &test->receive[test->head % 32];
  const urma_sge_t *source = wr->send.src.sge;
  if (source->len > destination->len)
    return URMA_EINVAL;
  memcpy((void *)(uintptr_t)destination->addr, (void *)(uintptr_t)source->addr,
         source->len);
  urma_cr_t rx = {};
  rx.flag.bs.s_r = 1;
  rx.opcode = URMA_CR_OPC_SEND;
  rx.completion_len = source->len;
  rx.user_ctx = test->cookies[test->head++ % 32];
  queueCompletion(test->base.jfr_cfg.jfc, &rx);
  urma_cr_t tx = {};
  if (fail_completion)
    tx.status = URMA_CR_WR_FLUSH_ERR;
  tx.user_ctx = wr->user_ctx;
  if (!suppress_completion)
    queueCompletion(jetty->jetty_cfg.jfs_cfg.jfc, &tx);
  return URMA_SUCCESS;
}
int urma_poll_jfc(urma_jfc_t *jfc, int count, urma_cr_t *out) {
  TestCompletion *test = (TestCompletion *)jfc;
  int polled = 0;
  while (polled < count && test->head != test->tail)
    out[polled++] = test->queue[test->head++ % 128];
  return polled;
}
}

TEST(UrmaTransportTest, RejectsSoftwareDeviceAndBoundsMemoryBeforeResources) {
  char error[URMA_ERROR_SIZE];
  native_ub = 0;
  EXPECT_TRUE(urmaRuntimeOpen("test", 0, 1024 * 1024, error) == NULL);
  native_ub = 1;
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1, error);
  ASSERT_TRUE(runtime != NULL);
  urmaHello hello = {0, 0, 4, 1};
  unsigned int before = attempts;
  EXPECT_TRUE(urmaTransportCreate(runtime, 42, &hello, error) == NULL);
  EXPECT_EQ(attempts, before);
  EXPECT_EQ(urmaRuntimeMemory(runtime), 0u);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
}

TEST(UrmaTransportTest,
     InitializationFailuresUnwindAndFailedDeletionRetainsAdmission) {
  char error[URMA_ERROR_SIZE];
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  urmaHello hello = {0, 0, 4, 1};
  for (unsigned int step = 1; step <= 7; step++) {
    fail_at = attempts + step;
    EXPECT_TRUE(urmaTransportCreate(runtime, 42, &hello, error) == NULL);
    EXPECT_EQ(urmaRuntimeMemory(runtime), 0u);
    EXPECT_EQ(allocations, 1u);
  }
  fail_at = 0;
  urmaTransport *transport = urmaTransportCreate(runtime, 42, &hello, error);
  ASSERT_TRUE(transport != NULL);
  size_t charge = urmaRuntimeMemory(runtime);
  fail_delete = 1;
  EXPECT_EQ(urmaTransportDestroy(transport), 0);
  EXPECT_EQ(urmaRuntimeMemory(runtime), charge);
  EXPECT_EQ(urmaRuntimeClose(runtime), 0);
  urmaTransportRetire(transport);
  fail_delete = 0;
  urmaRuntimeReap(runtime);
  EXPECT_EQ(urmaRuntimeMemory(runtime), 0u);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
}

TEST(UrmaTransportTest,
     InvalidEndpointAndBindFailureAbortWithoutFreeingLiveBuffers) {
  char error[URMA_ERROR_SIZE];
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  urmaHello hello = {0, 0, 4, 1};
  unsigned char endpoint[URMA_ENDPOINT_SIZE];
  for (int mode = 0; mode < 2; mode++) {
    urmaTransport *transport = urmaTransportCreate(runtime, 42, &hello, error);
    ASSERT_TRUE(transport != NULL);
    urmaTransportEndpoint(transport, endpoint);
    if (!mode)
      endpoint[0] = 0;
    fail_bind = mode;
    EXPECT_EQ(urmaTransportConnect(transport, endpoint, &hello), -1);
    EXPECT_EQ(urmaTransportWritable(transport), 0);
    EXPECT_EQ(urmaTransportWrite(transport, "x", 1).status, URMA_IO_ERROR);
    EXPECT_GT(urmaRuntimeMemory(runtime), 0u);
    EXPECT_EQ(urmaTransportDestroy(transport), 1);
    fail_bind = 0;
  }
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
}

TEST(UrmaTransportTest, FailedWorkCompletionPreventsFurtherWrites) {
  char error[URMA_ERROR_SIZE];
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  urmaHello hello = {0, 0, 4, 1};
  urmaTransport *transport = urmaTransportCreate(runtime, 42, &hello, error);
  ASSERT_TRUE(transport != NULL);
  unsigned char endpoint[URMA_ENDPOINT_SIZE];
  urmaTransportEndpoint(transport, endpoint);
  EXPECT_EQ(urmaTransportConnect(transport, endpoint, &hello), 0);
  fail_completion = 1;
  EXPECT_EQ(urmaTransportWrite(transport, "x", 1).bytes, 1u);
  EXPECT_EQ(urmaTransportProgress(transport, 16), -1);
  EXPECT_EQ(urmaTransportWrite(transport, "y", 1).status, URMA_IO_ERROR);
  fail_completion = 0;
  EXPECT_EQ(urmaTransportDestroy(transport), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
}

TEST(UrmaTransportTest, AmbiguousSubmissionIsNotRetried) {
  char error[URMA_ERROR_SIZE];
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  urmaHello hello = {0, 0, 4, 1};
  urmaTransport *transport = urmaTransportCreate(runtime, 42, &hello, error);
  ASSERT_TRUE(transport != NULL);
  unsigned char endpoint[URMA_ENDPOINT_SIZE];
  urmaTransportEndpoint(transport, endpoint);
  EXPECT_EQ(urmaTransportConnect(transport, endpoint, &hello), 0);
  ambiguous_send = 1;
  EXPECT_EQ(urmaTransportWrite(transport, "x", 1).bytes, 1u);
  ambiguous_send = 0;
  EXPECT_EQ(urmaTransportProgress(transport, 16), -1);
  EXPECT_EQ(urmaTransportWrite(transport, "y", 1).status, URMA_IO_ERROR);
  EXPECT_EQ(urmaTransportDestroy(transport), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
}

static urmaTransport *connectSelf(urmaRuntime *runtime) {
  char error[URMA_ERROR_SIZE];
  urmaHello hello = {0, 0, 4, 1};
  urmaTransport *transport = urmaTransportCreate(runtime, 42, &hello, error);
  if (!transport)
    return NULL;
  unsigned char endpoint[URMA_ENDPOINT_SIZE];
  urmaTransportEndpoint(transport, endpoint);
  EXPECT_EQ(urmaTransportConnect(transport, endpoint, &hello), 0);
  return transport;
}

TEST(UrmaTransportTest,
     QueuedSendDeadlineDoesNotRenewOnRetryOrReleaseDmaStorage) {
  char error[URMA_ERROR_SIZE];
  fake_clock = 1;
  clock_ms = 1000;
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, 0), 0);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, UINT32_MAX), 0);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, 10), 1);
  urmaTransport *transport = connectSelf(runtime);
  ASSERT_TRUE(transport != NULL);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, 20), 0);
  size_t charge = urmaRuntimeMemory(runtime);
  block_send = 1;
  EXPECT_EQ(urmaTransportWrite(transport, "x", 1).bytes, 1u);
  clock_ms += 9;
  EXPECT_GE(urmaTransportProgress(transport, 16), 0);
  clock_ms++;
  EXPECT_EQ(urmaTransportProgress(transport, 16), -1);
  EXPECT_TRUE(strstr(urmaTransportError(transport), "progress timeout") !=
              NULL);
  EXPECT_EQ(urmaRuntimeMemory(runtime), charge);
  block_send = 0;
  fail_delete = 1;
  EXPECT_EQ(urmaTransportDestroy(transport), 0);
  EXPECT_EQ(urmaRuntimeMemory(runtime), charge);
  urmaTransportRetire(transport);
  fail_delete = 0;
  urmaRuntimeReap(runtime);
  EXPECT_EQ(urmaRuntimeMemory(runtime), 0u);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
  fake_clock = 0;
}

TEST(UrmaTransportTest, MissingSendCompletionExpiresDespiteUnrelatedReceive) {
  char error[URMA_ERROR_SIZE];
  fake_clock = 1;
  clock_ms = 1000;
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, 10), 1);
  urmaTransport *transport = connectSelf(runtime);
  ASSERT_TRUE(transport != NULL);
  suppress_completion = 1;
  EXPECT_EQ(urmaTransportWrite(transport, "x", 1).bytes, 1u);
  clock_ms += 10;
  EXPECT_EQ(urmaTransportProgress(transport, 16), -1);
  EXPECT_EQ(urmaTransportWrite(transport, "y", 1).status, URMA_IO_ERROR);
  suppress_completion = 0;
  EXPECT_EQ(urmaTransportDestroy(transport), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
  fake_clock = 0;
}

TEST(UrmaTransportTest, ObservedCompletionAndIdleCreditWaitDoNotExpire) {
  char error[URMA_ERROR_SIZE];
  fake_clock = 1;
  clock_ms = 1000;
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, 10), 1);
  urmaTransport *transport = connectSelf(runtime);
  ASSERT_TRUE(transport != NULL);
  EXPECT_EQ(urmaTransportWrite(transport, "xy", 2).bytes, 2u);
  clock_ms += 100;
  EXPECT_GE(urmaTransportProgress(transport, 16), 0);
  EXPECT_EQ(urmaTransportWritable(transport), 0);
  clock_ms += 10000;
  EXPECT_GE(urmaTransportProgress(transport, 16), 0);
  char output[2];
  EXPECT_EQ(urmaTransportRead(transport, output, sizeof(output)).bytes, 2u);
  EXPECT_EQ(memcmp(output, "xy", 2), 0);
  clock_ms += 100;
  EXPECT_GE(urmaTransportProgress(transport, 16), 0);
  EXPECT_EQ(urmaTransportWritable(transport), 1);
  EXPECT_EQ(urmaTransportDestroy(transport), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
  fake_clock = 0;
}

TEST(UrmaTransportTest, ReceiveRepostStallExpiresWithoutGrantingCredit) {
  char error[URMA_ERROR_SIZE];
  fake_clock = 1;
  clock_ms = 1000;
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  EXPECT_EQ(urmaRuntimeSetProgressTimeout(runtime, 10), 1);
  urmaTransport *transport = connectSelf(runtime);
  ASSERT_TRUE(transport != NULL);
  EXPECT_EQ(urmaTransportWrite(transport, "x", 1).bytes, 1u);
  EXPECT_GE(urmaTransportProgress(transport, 16), 0);
  block_receive = 1;
  char output;
  EXPECT_EQ(urmaTransportRead(transport, &output, 1).bytes, 1u);
  EXPECT_EQ(urmaTransportWritable(transport), 0);
  clock_ms += 10;
  EXPECT_EQ(urmaTransportProgress(transport, 16), -1);
  block_receive = 0;
  EXPECT_EQ(urmaTransportDestroy(transport), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
  fake_clock = 0;
}

TEST(UrmaTransportTest, ClockFailureAfterWriteAcceptanceStillAllowsCleanup) {
  char error[URMA_ERROR_SIZE];
  fake_clock = 1;
  clock_ms = 1000;
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  urmaTransport *transport = connectSelf(runtime);
  ASSERT_TRUE(transport != NULL);
  clock_failure = 1;
  EXPECT_EQ(urmaTransportWrite(transport, "x", 1).bytes, 1u);
  clock_failure = 0;
  EXPECT_EQ(urmaTransportProgress(transport, 16), -1);
  EXPECT_TRUE(strstr(urmaTransportError(transport), "clock_gettime") != NULL);
  EXPECT_EQ(urmaTransportDestroy(transport), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
  fake_clock = 0;
}

TEST(UrmaTransportTest, RetryCreditAndClosePreserveByteStream) {
  char error[URMA_ERROR_SIZE];
  urmaRuntime *runtime = urmaRuntimeOpen("test", 0, 1024 * 1024, error);
  ASSERT_TRUE(runtime != NULL);
  urmaHello hello = {0, 0, 4, 2};
  urmaTransport *sender = urmaTransportCreate(runtime, 42, &hello, error);
  urmaTransport *receiver = urmaTransportCreate(runtime, 42, &hello, error);
  ASSERT_TRUE(sender != NULL);
  ASSERT_TRUE(receiver != NULL);
  unsigned char first[URMA_ENDPOINT_SIZE], second[URMA_ENDPOINT_SIZE];
  urmaTransportEndpoint(sender, first);
  urmaTransportEndpoint(receiver, second);
  EXPECT_EQ(urmaTransportConnect(sender, second, &hello), 0);
  EXPECT_EQ(urmaTransportConnect(receiver, first, &hello), 0);
  for (unsigned int round = 0; round < 64; round++) {
    block_send = 1;
    EXPECT_EQ(urmaTransportWrite(sender, "abcd", 4).bytes, 4u);
    EXPECT_EQ(urmaTransportWrite(sender, "ef", 2).bytes, 2u);
    EXPECT_EQ(urmaTransportWrite(sender, "g", 1).status, URMA_IO_AGAIN);
    block_send = 0;
    EXPECT_GE(urmaTransportProgress(sender, 16), 0);
    EXPECT_GE(urmaTransportProgress(receiver, 16), 0);
    char output[8] = {0};
    EXPECT_EQ(urmaTransportRead(receiver, output, sizeof(output)).bytes, 6u);
    EXPECT_EQ(memcmp(output, "abcdef", 6), 0);
    EXPECT_GE(urmaTransportProgress(receiver, 16), 0);
    EXPECT_GE(urmaTransportProgress(sender, 16), 0);
    EXPECT_EQ(urmaTransportWritable(sender), 1);
  }
  EXPECT_EQ(urmaTransportShutdown(sender), 0);
  EXPECT_GE(urmaTransportProgress(receiver, 16), 0);
  char output;
  EXPECT_EQ(urmaTransportRead(receiver, &output, 1).status, URMA_IO_EOF);
  EXPECT_GE(urmaTransportProgress(sender, 16), 0);
  EXPECT_EQ(urmaTransportShutdown(sender), 1);
  EXPECT_EQ(urmaTransportDestroy(sender), 1);
  EXPECT_EQ(urmaTransportDestroy(receiver), 1);
  EXPECT_EQ(urmaRuntimeClose(runtime), 1);
  EXPECT_EQ(allocations, 0u);
}

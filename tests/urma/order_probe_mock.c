/* Copyright (c) Valkey Contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */
/* SDK-seam double: exercises diagnostics and cleanup, not hardware semantics.
 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <urma_api.h>

static urma_device_t device;
static urma_context_t context;
static urma_jfce_t event;
static urma_jfc_t completion;
static urma_jfr_t receive;
static urma_jetty_t jetty;
static int context_live, event_live, completion_live, receive_live, jetty_live;
static unsigned int current_order;

static int scenario(const char *name) {
  const char *value = getenv("ORDER_PROBE_MOCK_FAILURE");
  return value && strcmp(value, name) == 0;
}

urma_status_t urma_init(urma_init_attr_t *config) {
  (void)config;
  puts("MOCK SDK: no hardware initialized");
  return URMA_SUCCESS;
}
urma_status_t urma_uninit(void) {
  assert(!context_live && !event_live && !completion_live && !receive_live &&
         !jetty_live);
  puts("mock.uninit");
  return URMA_SUCCESS;
}
urma_device_t *urma_get_device_by_name(char *name) {
  (void)name;
  device.type = scenario("not-ub") ? URMA_TRANSPORT_IB : URMA_TRANSPORT_UB;
  return &device;
}
urma_status_t urma_query_device(urma_device_t *dev, urma_device_attr_t *attr) {
  (void)dev;
  memset(attr, 0, sizeof(*attr));
  attr->dev_cap.trans_mode = URMA_TM_RC;
  attr->dev_cap.rc_tp_cap.bs.rtp = 1;
  /* Deliberately leave rc_order=0: creation must be diagnosed independently. */
  attr->dev_cap.max_jfs_sge = attr->dev_cap.max_jfr_sge = 1;
  attr->dev_cap.max_jfs_depth = attr->dev_cap.max_jfr_depth =
      attr->dev_cap.max_jfc_depth = 32;
  return URMA_SUCCESS;
}
urma_context_t *urma_create_context(urma_device_t *dev, uint32_t index) {
  (void)dev;
  (void)index;
  if (scenario("context")) {
    errno = ENODEV;
    return NULL;
  }
  assert(!context_live);
  context_live = 1;
  return &context;
}
urma_status_t urma_delete_context(urma_context_t *ctx) {
  assert(ctx == &context && context_live);
  assert(!event_live && !completion_live && !receive_live && !jetty_live);
  context_live = 0;
  puts("mock.delete_context");
  return URMA_SUCCESS;
}
urma_jfce_t *urma_create_jfce(urma_context_t *ctx) {
  assert(ctx == &context && context_live && !event_live);
  event_live = 1;
  return &event;
}
urma_status_t urma_delete_jfce(urma_jfce_t *value) {
  assert(value == &event && event_live && !completion_live);
  event_live = 0;
  return URMA_SUCCESS;
}
urma_jfc_t *urma_create_jfc(urma_context_t *ctx, urma_jfc_cfg_t *config) {
  assert(ctx == &context && event_live && !completion_live);
  assert(config->jfce == &event && config->depth == 16);
  if (scenario("jfc")) {
    errno = ENOMEM;
    return NULL;
  }
  completion_live = 1;
  return &completion;
}
urma_status_t urma_delete_jfc(urma_jfc_t *value) {
  assert(value == &completion && completion_live && !receive_live &&
         !jetty_live);
  completion_live = 0;
  return URMA_SUCCESS;
}
urma_jfr_t *urma_create_jfr(urma_context_t *ctx, urma_jfr_cfg_t *config) {
  assert(ctx == &context && completion_live && !receive_live);
  assert(config->trans_mode == URMA_TM_RC && config->max_sge == 1);
  current_order = config->flag.bs.order_type;
  if (current_order == URMA_OT && scenario("ot-jfr")) {
    errno = EOPNOTSUPP;
    return NULL;
  }
  receive_live = 1;
  return &receive;
}
urma_status_t urma_delete_jfr(urma_jfr_t *value) {
  assert(value == &receive && receive_live && !jetty_live);
  receive_live = 0;
  return URMA_SUCCESS;
}
urma_jetty_t *urma_create_jetty(urma_context_t *ctx, urma_jetty_cfg_t *config) {
  assert(ctx == &context && receive_live && !jetty_live);
  assert(config->flag.bs.share_jfr && config->shared.jfr == &receive);
  assert(config->shared.jfc == &completion &&
         config->jfs_cfg.jfc == &completion);
  assert(config->jfs_cfg.trans_mode == URMA_TM_RC &&
         config->jfs_cfg.max_sge == 1);
  assert(config->jfs_cfg.flag.bs.order_type == current_order);
  if (current_order == URMA_OT && scenario("ot-jetty")) {
    errno = EOPNOTSUPP;
    return NULL;
  }
  jetty_live = 1;
  return &jetty;
}
urma_status_t urma_delete_jetty(urma_jetty_t *value) {
  assert(value == &jetty && jetty_live);
  if (scenario("delete-jetty"))
    return URMA_FAIL;
  jetty_live = 0;
  return URMA_SUCCESS;
}

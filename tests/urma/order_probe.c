/* Copyright (c) Valkey Contributors
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <urma_api.h>

/* No peer, registered memory, posted work or data transfer. A successful create
 * does not establish the ordering semantics required by the Valkey adapter. */
static int checkOrder(urma_context_t *context, urma_order_type_t order,
                      const char *label) {
  urma_jfce_t *event = NULL;
  urma_jfc_t *completion = NULL;
  urma_jfr_t *receive = NULL;
  urma_jetty_t *jetty = NULL;
  const char *stage = "create_jfce";
  int result = 0;
  errno = 0;
  event = urma_create_jfce(context);
  if (!event)
    goto done;
  urma_jfc_cfg_t cq = {.depth = 16, .jfce = event};
  stage = "create_jfc";
  errno = 0;
  completion = urma_create_jfc(context, &cq);
  if (!completion)
    goto done;
  urma_jfr_cfg_t rq = {.depth = 8,
                       .trans_mode = URMA_TM_RC,
                       .max_sge = 1,
                       .jfc = completion,
                       .token_value = {.token = 0x1234}};
  rq.flag.bs.token_policy = URMA_TOKEN_PLAIN_TEXT;
  rq.flag.bs.order_type = order;
  stage = "create_jfr";
  errno = 0;
  receive = urma_create_jfr(context, &rq);
  if (!receive)
    goto done;
  urma_jetty_cfg_t config = {0};
  config.flag.bs.share_jfr = 1;
  config.shared.jfr = receive;
  config.shared.jfc = completion;
  config.jfs_cfg.depth = 8;
  config.jfs_cfg.trans_mode = URMA_TM_RC;
  config.jfs_cfg.max_sge = 1;
  config.jfs_cfg.rnr_retry = 3;
  config.jfs_cfg.err_timeout = 14;
  config.jfs_cfg.flag.bs.order_type = order;
  config.jfs_cfg.jfc = completion;
  stage = "create_jetty";
  errno = 0;
  jetty = urma_create_jetty(context, &config);
  if (!jetty)
    goto done;
  result = 1;
done:
  if (result)
    printf("order=%s requested=%u resources=created\n", label,
           (unsigned int)order);
  else
    printf("order=%s requested=%u failed_stage=%s errno=%d (%s)\n", label,
           (unsigned int)order, stage, errno, strerror(errno));
  /* Stop at the first deletion failure; do not delete live dependencies. */
  urma_status_t status;
#define DELETE_RESOURCE(resource, function)                                    \
  do {                                                                         \
    if (resource) {                                                            \
      status = function(resource);                                             \
      printf("order=%s cleanup=%s status=%d\n", label, #function, status);     \
      if (status != URMA_SUCCESS)                                              \
        return -1;                                                             \
    }                                                                          \
  } while (0)
  DELETE_RESOURCE(jetty, urma_delete_jetty);
  DELETE_RESOURCE(receive, urma_delete_jfr);
  DELETE_RESOURCE(completion, urma_delete_jfc);
  DELETE_RESOURCE(event, urma_delete_jfce);
#undef DELETE_RESOURCE
  return result;
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s DEVICE EID_INDEX\n", argv[0]);
    return 2;
  }
  char *end;
  errno = 0;
  unsigned long index = strtoul(argv[2], &end, 10);
  if (errno || !*argv[2] || *end || argv[2][0] == '-' || index > UINT32_MAX)
    return 2;
  puts("Resource-creation diagnostic only; no peer or data transfer");
  urma_status_t status = urma_init(NULL);
  printf("urma_init.status=%d\n", status);
  if (status != URMA_SUCCESS)
    return 1;
  int result = 1;
  urma_device_t *device = urma_get_device_by_name(argv[1]);
  urma_device_attr_t attributes = {0};
  if (!device || device->type != URMA_TRANSPORT_UB) {
    fputs("Selected device is unavailable or not native UB\n", stderr);
    goto uninit;
  }
  status = urma_query_device(device, &attributes);
  printf("device=%s eid_index=%lu query.status=%d\n", argv[1], index, status);
  if (status != URMA_SUCCESS)
    goto uninit;
  printf("modes=%u rc_order=%u rc_tp=%u ot=%u ol=%u send_sge=%u recv_sge=%u\n",
         attributes.dev_cap.trans_mode, attributes.dev_cap.rc_order_cap.value,
         attributes.dev_cap.rc_tp_cap.value,
         attributes.dev_cap.rc_order_cap.bs.ot,
         attributes.dev_cap.rc_order_cap.bs.ol, attributes.dev_cap.max_jfs_sge,
         attributes.dev_cap.max_jfr_sge);
  if (!(attributes.dev_cap.trans_mode & URMA_TM_RC) ||
      !attributes.dev_cap.max_jfs_sge || !attributes.dev_cap.max_jfr_sge ||
      attributes.dev_cap.max_jfs_depth < 8 ||
      attributes.dev_cap.max_jfr_depth < 8 ||
      attributes.dev_cap.max_jfc_depth < 16) {
    fputs(
        "RC, SGE or queue limits do not meet this diagnostic's requirements\n",
        stderr);
    goto uninit;
  }
  errno = 0;
  urma_context_t *context = urma_create_context(device, (uint32_t)index);
  if (!context) {
    fprintf(stderr, "urma_create_context failed: errno=%d (%s)\n", errno,
            strerror(errno));
    goto uninit;
  }
  int defaults = checkOrder(context, URMA_DEF_ORDER, "default");
  if (defaults < 0)
    goto retained;
  int target = checkOrder(context, URMA_OT, "OT");
  if (target < 0)
    goto retained;
  result = !(defaults && target);
  status = urma_delete_context(context);
  printf("urma_delete_context.status=%d\n", status);
  if (status != URMA_SUCCESS)
    goto retained;
uninit:
  status = urma_uninit();
  printf("urma_uninit.status=%d\n", status);
  puts("Resource creation does not prove SEND/RECV ordering or Valkey "
       "correctness");
  return result || status != URMA_SUCCESS;
retained:
  fputs("Cleanup incomplete; dependent resources not explicitly deleted; "
        "exiting process\n",
        stderr);
  return 1;
}

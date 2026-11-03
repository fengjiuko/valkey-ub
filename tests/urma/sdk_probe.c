/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <urma_api.h>

int main(int argc, char **argv) {
  struct utsname host;
  if (uname(&host) == 0)
    printf("platform=%s kernel=%s machine=%s page_size=%ld\n", host.sysname,
           host.release, host.machine, sysconf(_SC_PAGESIZE));
  if (argc == 2 && strcmp(argv[1], "--link-only") == 0) {
    puts("SDK headers/link OK; device initialization and hardware not tested");
    return 0;
  }
  if (argc != 2 || strcmp(argv[1], "--inventory") != 0) {
    fprintf(stderr, "Usage: %s --link-only|--inventory\n", argv[0]);
    return 2;
  }
  urma_status_t status = urma_init(NULL);
  printf("urma_init.status=%d\n", status);
  if (status != URMA_SUCCESS)
    return 1;
  int count = 0;
  urma_device_t **devices = urma_get_device_list(&count);
  printf("device_count=%d\n", count);
  int result = !devices || count == 0;
  for (int i = 0; devices && i < count; i++) {
    urma_device_attr_t attr = {0};
    status = urma_query_device(devices[i], &attr);
    printf("device=%s transport_type=%d query.status=%d\n", devices[i]->name,
           devices[i]->type, status);
    if (status != URMA_SUCCESS) {
      result = 1;
      continue;
    }
    printf("modes=%u rc_order=%u rc_tp=%u max_message=%llu send_depth=%u "
           "recv_depth=%u completion_depth=%u\n",
           attr.dev_cap.trans_mode, attr.dev_cap.rc_order_cap.value,
           attr.dev_cap.rc_tp_cap.value,
           (unsigned long long)attr.dev_cap.max_msg_size,
           attr.dev_cap.max_jfs_depth, attr.dev_cap.max_jfr_depth,
           attr.dev_cap.max_jfc_depth);
    uint32_t eid_count = 0;
    urma_eid_info_t *eids = urma_get_eid_list(devices[i], &eid_count);
    for (uint32_t j = 0; eids && j < eid_count; j++) {
      printf("eid_index=%u eid=", eids[j].eid_index);
      for (unsigned int k = 0; k < sizeof(eids[j].eid.raw); k++)
        printf("%02x", eids[j].eid.raw[k]);
      putchar('\n');
    }
    if (eids)
      urma_free_eid_list(eids);
  }
  if (devices)
    urma_free_device_list(devices);
  status = urma_uninit();
  printf("urma_uninit.status=%d\n", status);
  return result || status != URMA_SUCCESS;
}

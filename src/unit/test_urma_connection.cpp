/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "generated_wrappers.hpp"
extern "C" {
#include "server.h"
}

TEST(UrmaConnectionTest, BackgroundForkIsRejectedBeforeCreatingAChild) {
    int previous_port = server.urma_port;
    int previous_verbosity = server.verbosity;
    pid_t previous_child = server.child_pid;
    server.urma_port = 6380;
    server.verbosity = LL_WARNING + 1;
    /* Even a regression must stop at the existing busy-child guard instead
     * of forking the test runner. Its EALREADY differs from ENOTSUP. */
    server.child_pid = 12345;
    errno = 0;
    int result = serverFork(CHILD_TYPE_RDB);
    int error = errno;
    server.urma_port = previous_port;
    server.verbosity = previous_verbosity;
    server.child_pid = previous_child;
    EXPECT_EQ(result, -1);
    EXPECT_EQ(error, ENOTSUP);
}

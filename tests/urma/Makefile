# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
CXX ?= c++
URMA_CFLAGS ?= -isystem /usr/include/ub/umdk/urma
URMA_LIBS ?= -lurma
OUT ?= ../../build/urma-runtime
CHECK_FLAGS = -g -O1 -std=gnu11 -ffunction-sections -fdata-sections -DUSE_URMA -I../../src $(URMA_CFLAGS) $(SANITIZER_FLAGS)
OBJECTS = $(addprefix $(OUT)/,urma_protocol.o urma_session.o urma_transport.o zmalloc.o serverassert.o)

.PHONY: all test clean
all: $(OUT)/sdk-probe $(OUT)/client-probe

$(OUT):
	mkdir -p $@

$(OUT)/%.o: ../../src/%.c | $(OUT)
	$(CC) $(CHECK_FLAGS) -c $< -o $@

$(OUT)/sdk-probe: sdk_probe.c | $(OUT)
	$(CC) -std=gnu11 -Wall -Wextra -Werror $(URMA_CFLAGS) $< $(URMA_LIBS) -o $@

$(OUT)/client-probe: client_probe.c $(OBJECTS)
	$(CC) $(CHECK_FLAGS) -Wall -Wextra -Werror $^ -Wl,--gc-sections $(URMA_LIBS) -o $@

# This runner substitutes SDK calls at the SDK seam. It is never linked into
# server/probe binaries and does not establish hardware compatibility.
$(OUT)/transport-tests: test_transport.cpp $(OBJECTS)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I../../src $(URMA_CFLAGS) $(SANITIZER_FLAGS) $^ -Wl,--gc-sections -Wl,--wrap=clock_gettime -lgtest_main -lgtest -pthread -o $@

test: $(OUT)/transport-tests
	$(OUT)/transport-tests

clean:
	rm -f $(OBJECTS) $(OUT)/transport-tests $(OUT)/sdk-probe $(OUT)/client-probe

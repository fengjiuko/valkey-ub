# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
CC ?= cc
CXX ?= c++
URMA_CFLAGS ?= -isystem /usr/include/ub/umdk/urma
URMA_LIBS ?= -lurma
OUT ?= ../../build/urma-runtime
CHECK_FLAGS = -g -O1 -std=gnu11 -ffunction-sections -fdata-sections -DUSE_URMA -I../../src $(URMA_CFLAGS) $(SANITIZER_FLAGS)
OBJECTS = $(addprefix $(OUT)/,urma_protocol.o urma_session.o urma_transport.o zmalloc.o serverassert.o)
MEMORY_OBJECTS = $(addprefix $(OUT)/,remote_value.o remote_cache.o crc64.o crcspeed.o crccombine.o zmalloc.o serverassert.o)

.PHONY: all test memory-test clean
all: $(OUT)/sdk-probe $(OUT)/client-probe $(OUT)/cache-probe

$(OUT):
	mkdir -p $@

$(OUT)/%.o: ../../src/%.c | $(OUT)
	$(CC) $(CHECK_FLAGS) -MMD -MP -c $< -o $@

-include $(sort $(OBJECTS:.o=.d) $(MEMORY_OBJECTS:.o=.d))

$(OUT)/sdk-probe: sdk_probe.c | $(OUT)
	$(CC) -std=gnu11 -Wall -Wextra -Werror $(URMA_CFLAGS) $< $(URMA_LIBS) -o $@

$(OUT)/client-probe: client_probe.c $(OBJECTS)
	$(CC) $(CHECK_FLAGS) -Wall -Wextra -Werror $(filter %.c %.o,$^) -Wl,--gc-sections $(URMA_LIBS) -o $@

$(OUT)/client-probe: probe_io.h

# This runner substitutes SDK calls at the SDK seam. It is never linked into
# server/probe binaries and does not establish hardware compatibility.
$(OUT)/transport-tests: test_transport.cpp $(sort $(OBJECTS) $(MEMORY_OBJECTS))
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I../../src $(URMA_CFLAGS) $(SANITIZER_FLAGS) $^ -Wl,--gc-sections -Wl,--wrap=clock_gettime -lgtest_main -lgtest -pthread -o $@

test: $(OUT)/transport-tests
	$(OUT)/transport-tests

$(OUT)/memory-tests: ../../src/unit/test_remote_value.cpp ../../src/unit/test_remote_cache.cpp $(MEMORY_OBJECTS)
	$(CXX) -std=c++17 -Wall -Wextra -Werror -I../../src $(SANITIZER_FLAGS) $^ -Wl,--gc-sections -lgtest_main -lgtest -pthread -o $@

# The pool/codec tests do not call the SDK or access devices.
memory-test: $(OUT)/memory-tests
	$(OUT)/memory-tests

$(OUT)/cache-probe: cache_probe.c $(sort $(OBJECTS) $(MEMORY_OBJECTS))
	$(CC) $(CHECK_FLAGS) -Wall -Wextra -Werror $(filter %.c %.o,$^) -Wl,--gc-sections $(URMA_LIBS) -o $@

$(OUT)/cache-probe: probe_io.h

clean:
	rm -f $(sort $(OBJECTS) $(MEMORY_OBJECTS)) $(OUT)/transport-tests $(OUT)/memory-tests $(OUT)/sdk-probe $(OUT)/client-probe $(OUT)/cache-probe
	rm -f $(sort $(OBJECTS:.o=.d) $(MEMORY_OBJECTS:.o=.d))

# Thread Pool Job Scheduler - build file
#   make            build bin/scheduler
#   make test       build and run all unit tests
#   make bench      build and run the benchmark
#   make demo       run the non-interactive demonstration
#   make asan       run the tests with AddressSanitizer + UBSan
#   make tsan       run the tests with ThreadSanitizer
#   make clean      remove build output

CC       ?= gcc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wformat=2 -Wundef
CPPFLAGS := -Iinclude -D_POSIX_C_SOURCE=200809L
OPT      ?= -O2 -g
EXTRA    ?=
CFLAGS   := $(CSTD) $(WARN) $(OPT) $(EXTRA)
LDLIBS   := -pthread

BUILD    ?= build
BIN      ?= bin

LIB_SRCS := src/config.c src/job.c src/job_queue.c src/scheduler.c \
            src/worker_pool.c src/task_handlers.c src/metrics.c src/logger.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(BUILD)/%.o)
TESTS    := test_queue test_scheduler test_task_handlers test_fault_handling
TEST_BINS:= $(TESTS:%=$(BIN)/%)

.PHONY: all test bench demo run asan tsan clean dirs

all: $(BIN)/scheduler

$(BUILD) $(BIN):
	mkdir -p $@

$(BUILD)/%.o: src/%.c $(wildcard include/*.h) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread -c $< -o $@

$(BIN)/scheduler: $(LIB_OBJS) $(BUILD)/main.o | $(BIN)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BIN)/test_%: tests/test_%.c tests/test_common.h $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) -Itests $< $(LIB_OBJS) -o $@ $(LDLIBS)

$(BIN)/benchmark: benchmarks/benchmark.c $(LIB_OBJS) | $(BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_OBJS) -o $@ $(LDLIBS)

test: $(TEST_BINS)
	@mkdir -p logs reports
	@fail=0; for t in $(TEST_BINS); do echo "=== $$t"; ./$$t || fail=1; done; \
	if [ $$fail -eq 0 ]; then echo "ALL TEST SUITES PASSED"; else echo "SOME TESTS FAILED"; exit 1; fi

bench: $(BIN)/benchmark
	@mkdir -p reports
	./$(BIN)/benchmark

demo: $(BIN)/scheduler
	./$(BIN)/scheduler --demo

run: $(BIN)/scheduler
	./$(BIN)/scheduler

asan:
	$(MAKE) BUILD=build/asan BIN=build/asan/bin OPT="-O1 -g" \
	  EXTRA="-fsanitize=address,undefined -fno-omit-frame-pointer" test

tsan:
	$(MAKE) BUILD=build/tsan BIN=build/tsan/bin OPT="-O1 -g" \
	  EXTRA="-fsanitize=thread" test

clean:
	rm -rf build bin

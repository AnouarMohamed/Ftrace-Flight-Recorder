# Licensed under the Universal Permissive License (UPL), Version 1.0.
# See LICENSE.txt for details.

VERSION      ?= 1.4.0
CC           ?= cc
CFLAGS       ?= -O2 -g
CPPFLAGS     ?= -Isrc
LDFLAGS      ?=
LDLIBS       ?=
WARNFLAGS    ?= -Wall -Wextra -Wpedantic -Wformat=2 -Wshadow \
                -Wstrict-prototypes

CPPFLAGS     += -D_GNU_SOURCE -DFDR_VERSION=\"$(VERSION)\"
CSTD         ?= -std=c11

PREFIX       ?= /usr
SBINDIR      ?= $(PREFIX)/sbin
DATADIR      ?= $(PREFIX)/share
MANDIR8      ?= $(DATADIR)/man/man8
UNITDIR      ?= /usr/lib/systemd/system
SYSCONFDIR   ?= /etc
INSTALL      ?= install

SRCS         := src/main.c src/runtime.c src/util.c src/config.c \
                src/trace.c src/harvest.c src/process.c src/http.c
OBJS         := $(SRCS:.c=.o)
BUILD_DIR    := .build
TEST_BINS    := $(BUILD_DIR)/test_config $(BUILD_DIR)/test_harvest \
                $(BUILD_DIR)/test_trace
BENCH_BINS   := $(BUILD_DIR)/benchmark_harvest \
                $(BUILD_DIR)/benchmark_loss
TEXT_BUFFER_SIZES := 4096 8192 16384 65536
TEXT_BUFFER_BINS := $(addprefix $(BUILD_DIR)/fdrd-buffer-,\
                    $(TEXT_BUFFER_SIZES))
PERF_BINS    := $(TEXT_BUFFER_BINS) $(BUILD_DIR)/sched_load \
                $(BUILD_DIR)/per_cpu_capture
SANITIZER_CC ?= clang

RPMBUILD_DIR ?= $(HOME)/rpmbuild
LATEST_VERS  ?= $(VERSION)
TEST_CONFIG  ?= tests/fixtures

.PHONY: all clean install uninstall check sanitize benchmark benchmark-loss \
        performance-binaries rpm srpm tarball

all: fdrd

fdrd: $(OBJS)
	$(CC) $(CSTD) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

src/%.o: src/%.c src/fdr.h
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/test_config: tests/test_config.c src/runtime.c src/util.c \
    src/config.c src/fdr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -o $@ tests/test_config.c \
	    src/runtime.c src/util.c src/config.c $(LDLIBS)

$(BUILD_DIR)/test_harvest: tests/test_harvest.c src/runtime.c src/util.c \
    src/harvest.c src/fdr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -o $@ tests/test_harvest.c \
	    src/runtime.c src/util.c src/harvest.c $(LDLIBS)

$(BUILD_DIR)/test_trace: tests/test_trace.c src/runtime.c src/util.c \
    src/trace.c src/fdr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -o $@ tests/test_trace.c \
	    src/runtime.c src/util.c src/trace.c $(LDLIBS)

$(BUILD_DIR)/benchmark_harvest: tests/benchmarks/benchmark_harvest.c \
    src/runtime.c src/util.c src/harvest.c src/fdr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -o $@ \
	    tests/benchmarks/benchmark_harvest.c src/runtime.c src/util.c \
	    src/harvest.c $(LDLIBS)

$(BUILD_DIR)/benchmark_loss: tests/benchmarks/benchmark_loss.c \
    src/runtime.c src/util.c src/trace.c src/fdr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -o $@ \
	    tests/benchmarks/benchmark_loss.c src/runtime.c src/util.c \
	    src/trace.c $(LDLIBS)

$(BUILD_DIR)/fdrd-buffer-%: $(SRCS) src/fdr.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -DFDR_HARVEST_BUFFER_MIN=$*U $(CSTD) $(CFLAGS) \
	    $(WARNFLAGS) -o $@ $(SRCS) $(LDLIBS)

$(BUILD_DIR)/sched_load: tests/benchmarks/sched_load.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -pthread -o $@ $<

$(BUILD_DIR)/per_cpu_capture: tests/benchmarks/per_cpu_capture.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CSTD) $(CFLAGS) $(WARNFLAGS) -pthread -o $@ $<

check: fdrd $(TEST_BINS)
	./fdrd -n -c $(TEST_CONFIG)
	@if ./fdrd -n -c tests/invalid >/dev/null 2>&1; then \
		echo "expected invalid config to fail"; exit 1; \
	fi
	$(BUILD_DIR)/test_config
	$(BUILD_DIR)/test_harvest
	$(BUILD_DIR)/test_trace
	tests/test_runtime.sh ./fdrd

benchmark: $(BENCH_BINS)
	tests/benchmarks/run-collector.sh $(BUILD_DIR)/benchmark_harvest

benchmark-loss: $(BUILD_DIR)/benchmark_loss
	$(BUILD_DIR)/benchmark_loss

performance-binaries: $(PERF_BINS)

sanitize: | $(BUILD_DIR)
	$(SANITIZER_CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD_DIR)/fdrd-sanitize $(SRCS)
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/fdrd-sanitize \
	    -n -c $(TEST_CONFIG)
	$(SANITIZER_CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD_DIR)/test-config-sanitize tests/test_config.c \
	    src/runtime.c src/util.c src/config.c
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-config-sanitize
	$(SANITIZER_CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD_DIR)/test-harvest-sanitize tests/test_harvest.c \
	    src/runtime.c src/util.c src/harvest.c
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-harvest-sanitize
	$(SANITIZER_CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -o $(BUILD_DIR)/test-trace-sanitize tests/test_trace.c \
	    src/runtime.c src/util.c src/trace.c
	ASAN_OPTIONS=detect_leaks=1 $(BUILD_DIR)/test-trace-sanitize
	ASAN_OPTIONS=detect_leaks=1 tests/test_runtime.sh \
	    $(BUILD_DIR)/fdrd-sanitize

clean:
	rm -f fdrd $(OBJS)
	rm -rf $(BUILD_DIR)

install: fdrd
	$(INSTALL) -d $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 fdrd $(DESTDIR)$(SBINDIR)/fdrd
	$(INSTALL) -d $(DESTDIR)$(MANDIR8)
	$(INSTALL) -m 0644 fdrd.man $(DESTDIR)$(MANDIR8)/fdrd.8
	$(INSTALL) -d $(DESTDIR)$(UNITDIR)
	$(INSTALL) -m 0644 fdr.service $(DESTDIR)$(UNITDIR)/fdr.service
	$(INSTALL) -d $(DESTDIR)$(SYSCONFDIR)/fdr.d
	$(INSTALL) -d $(DESTDIR)$(DATADIR)/fdr/samples
	$(INSTALL) -m 0644 README.md $(DESTDIR)$(DATADIR)/fdr/README
	$(INSTALL) -m 0644 samples/nfs $(DESTDIR)$(DATADIR)/fdr/samples/nfs.conf
	$(INSTALL) -m 0644 samples/nfs.logrotate \
	    $(DESTDIR)$(DATADIR)/fdr/samples/nfs.logrotate

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/fdrd
	rm -f $(DESTDIR)$(MANDIR8)/fdrd.8
	rm -f $(DESTDIR)$(UNITDIR)/fdr.service
	rm -rf $(DESTDIR)$(DATADIR)/fdr

tarball:
	mkdir -p $(RPMBUILD_DIR)/SOURCES
	git archive --format=tar --prefix=fdr-$(LATEST_VERS)/ HEAD \
	    | xz -T0 > $(RPMBUILD_DIR)/SOURCES/fdr-$(LATEST_VERS).tar.xz

rpm: tarball
	mkdir -p $(RPMBUILD_DIR)/SPECS
	$(INSTALL) -m 0644 buildrpm/1.4/fdr.spec \
	    $(RPMBUILD_DIR)/SPECS/fdr.spec
	rpmbuild --define "_topdir $(RPMBUILD_DIR)" \
	    -bb $(RPMBUILD_DIR)/SPECS/fdr.spec

srpm: tarball
	mkdir -p $(RPMBUILD_DIR)/SPECS
	$(INSTALL) -m 0644 buildrpm/1.4/fdr.spec \
	    $(RPMBUILD_DIR)/SPECS/fdr.spec
	rpmbuild --define "_topdir $(RPMBUILD_DIR)" \
	    -bs $(RPMBUILD_DIR)/SPECS/fdr.spec

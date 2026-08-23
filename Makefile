# Licensed under the Universal Permissive License (UPL), Version 1.0.
# See LICENSE.txt for details.

CFLAGS       ?= -Wall -Wextra -Wpedantic -O2 -g \
                -Wno-format-truncation -Wno-stringop-truncation
CPPFLAGS     ?= -Isrc
LDFLAGS      ?=

PREFIX       := /usr
SBINDIR      := $(PREFIX)/sbin
DATADIR      := $(PREFIX)/share
MANDIR8      := $(DATADIR)/man/man8
UNITDIR      := /usr/lib/systemd/system
INSTALL      := install

SRCS         := src/main.c src/runtime.c src/util.c src/config.c \
                src/trace.c src/harvest.c src/process.c
OBJS         := $(SRCS:.c=.o)

RPMBUILD_DIR ?= $(HOME)
LATEST_VERS  ?= 1.3
TEST_CONFIG  ?= tests/fixtures

.PHONY: all clean install uninstall check rpm srpm tarball

all: fdrd

fdrd: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

src/%.o: src/%.c src/fdr.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f fdrd $(OBJS)

check: fdrd
	./fdrd -n -c $(TEST_CONFIG)
	@if ./fdrd -n -c tests/invalid >/dev/null 2>&1; then \
		echo "expected invalid config to fail"; exit 1; \
	fi

install: fdrd
	mkdir -p $(DESTDIR)$(SBINDIR)
	$(INSTALL) -m 0755 fdrd $(DESTDIR)$(SBINDIR)
	mkdir -p $(DESTDIR)$(MANDIR8)
	$(INSTALL) -m 0644 fdrd.man $(DESTDIR)$(MANDIR8)/fdrd.8
	mkdir -p $(DESTDIR)$(UNITDIR)
	$(INSTALL) -m 0644 fdr.service $(DESTDIR)$(UNITDIR)/fdr.service
	mkdir -p $(DESTDIR)$(DATADIR)/fdr/samples
	$(INSTALL) -m 0644 README.md $(DESTDIR)$(DATADIR)/fdr/README
	$(INSTALL) -m 0644 samples/nfs $(DESTDIR)$(DATADIR)/fdr/samples
	$(INSTALL) -m 0644 samples/nfs.logrotate $(DESTDIR)$(DATADIR)/fdr/samples

uninstall:
	rm -f $(DESTDIR)$(SBINDIR)/fdrd
	rm -f $(DESTDIR)$(MANDIR8)/fdrd.8
	rm -f $(DESTDIR)$(UNITDIR)/fdr.service
	rm -rf $(DESTDIR)$(DATADIR)/fdr

tarball: clean
	tar --transform "s/^./fdr-$(LATEST_VERS)/" \
		--xz -cf $(RPMBUILD_DIR)/SOURCES/fdr-$(LATEST_VERS).tar.xz .

release:
	git tag -f fdr-$(LATEST_VERS)
	git archive --format=tar --prefix=fdr-$(LATEST_VERS)/ fdr-$(LATEST_VERS) \
		| ( cd /tmp ; tar xf - )
	(cd /tmp ; tar cJf $(RPMBUILD_DIR)/SOURCES/fdr-$(LATEST_VERS).tar.xz \
		fdr-$(LATEST_VERS))

rpm: tarball
	cp buildrpm/$(LATEST_VERS)/fdr.spec \
		$(RPMBUILD_DIR)/rpmbuild/SPECS/fdr.spec
	rpmbuild -bb $(RPMBUILD_DIR)/rpmbuild/SPECS/fdr.spec

srpm: tarball
	cp buildrpm/$(LATEST_VERS)/fdr.spec \
		$(RPMBUILD_DIR)/rpmbuild/SPECS/fdr.spec
	rpmbuild -bs $(RPMBUILD_DIR)/rpmbuild/SPECS/fdr.spec

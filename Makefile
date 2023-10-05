PREFIX ?= /usr
ifeq ($(PREFIX),/usr)
ETC ?= /etc
else
ETC ?= $(PREFIX)/etc
endif

VERSION := $(shell git describe 2>/dev/null || awk -F'"' '/define BUTTOND_VERSION/ { print $$2 }' version.h)

.PHONY: all install clean

CFLAGS ?= -Wall -Wextra -DBUTTOND_VERSION=\"$(VERSION)\"

all: buttond

keynames.h: gen_keynames_h.sh
	./$^ > $@

buttond.o: buttond.c buttond.h time_utils.h utils.h keynames.h
input.o: input.c buttond.h time_utils.h utils.h
buttond: buttond.o input.o keys.o

clean:
	rm -f buttond buttond.o input.o keys.o

check:
	./tests.sh

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin buttond
	install -D -t $(DESTDIR)$(ETC)/init.d openrc/init.d/buttond
	install -D -t $(DESTDIR)$(ETC)/conf.d -m 0644 openrc/conf.d/buttond

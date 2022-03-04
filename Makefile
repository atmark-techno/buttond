PREFIX ?= /usr
ifeq ($(PREFIX),/usr)
ETC ?= /etc
else
ETC ?= $(PREFIX)/etc
endif


.PHONY: all install clean

CFLAGS ?= -Wall -Wextra

all: buttond

buttond: buttond.o input.o

clean:
	rm -f buttond buttond.o input.o

check:
	./tests.sh

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin buttond
	install -D -t $(DESTDIR)$(ETC)/init.d openrc/init.d/buttond
	install -D -t $(DESTDIR)$(ETC)/conf.d -m 0644 openrc/conf.d/buttond

PREFIX ?= /usr
ifeq ($(PREFIX),/usr)
ETC ?= /etc
else
ETC ?= $(PREFIX)/etc
endif


.PHONY: all install clean

CFLAGS ?= -Wall -Wextra

all: buttond

clean:
	rm -f buttond

check:
	./tests.sh

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin buttond
	install -D -t $(DESTDIR)$(ETC)/init.d openrc/init.d/buttond
	install -D -t $(DESTDIR)$(ETC)/conf.d openrc/conf.d/buttond

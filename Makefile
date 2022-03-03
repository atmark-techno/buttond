PREFIX ?= /usr

.PHONY: all install clean

CFLAGS ?= -Wall -Wextra

all: buttond

clean:
	rm -f buttond

check:
	./tests.sh

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin buttond

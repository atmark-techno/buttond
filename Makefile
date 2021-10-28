PREFIX ?= /usr

.PHONY: all install clean

CFLAGS ?= -Wall -Wextra

all: buttond

clean:
	rm -f buttond

install: all
	install -D -t $(DESTDIR)$(PREFIX)/bin buttond

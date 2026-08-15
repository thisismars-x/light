PREFIX ?= /usr/local

.PHONY: clean install

light: light.c
	cc -Wall -Wextra -O2 -pthread light.c -o light

install: light
	install -Dm755 light "$(DESTDIR)$(PREFIX)/bin/light"

clean:
	rm -f light

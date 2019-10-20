PREFIX  ?= /usr/local
bindir  ?= $(PREFIX)/bin
mandir  ?= $(PREFIX)/share/man
man1dir ?= $(mandir)/man1

VERSION=0.1

###############################################################################

CPPFLAGS=-DVERSION="\"$(VERSION)\""
CFLAGS=-pthread -Wall

default: all

ipc-server: ipc-server.c

ipc-client: ipc-client.c

install-bin: ipc-server ipc-client
	install -d $(DESTDIR)$(bindir)
	install -m755 ipc-client $(DESTDIR)$(bindir)
	install -m755 ipc-server $(DESTDIR)$(bindir)

install-man: ipc-server.1 ipc-server.1
	install -d $(DESTDIR)$(man1dir)
	install -m644 ipc-client.1 $(DESTDIR)$(man1dir)
	install -m644 ipc-server.1 $(DESTDIR)$(man1dir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/ipc-client
	rm -f $(DESTDIR)$(bindir)/ipc-server
	-rmdir $(DESTDIR)$(bindir)

uninstall-man:
	rm -f $(DESTDIR)$(man1dir)/ipc-client.1
	rm -f $(DESTDIR)$(man1dir)/ipc-server.1
	-rmdir $(DESTDIR)$(man1dir)
	-rmdir $(DESTDIR)$(mandir)

all: ipc-client ipc-server

man: ipc-client ipc-server
	help2man -N -n 'IPC STDIO SERVER' -o ipc-client.1 ./ipc-client
	help2man -N -n 'IPC STDIO CLIENT' -o ipc-server.1 ./ipc-server

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

clean:
	rm -f ipc-client ipc-server

.PHONY: default all man clean install install-bin install-man uninstall uninstall-bin uninstall-man

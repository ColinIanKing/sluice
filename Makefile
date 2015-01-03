VERSION=0.01.07

CFLAGS += -Wall -Wextra -DVERSION='"$(VERSION)"'

BINDIR=/usr/bin
MANDIR=/usr/share/man/man1

sluice: sluice.o
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

sluice.1.gz: sluice.1
	gzip -c $< > $@

dist:
	rm -rf sluice-$(VERSION)
	mkdir sluice-$(VERSION)
	cp -rp Makefile sluice.c sluice.1 COPYING sluice-$(VERSION)
	tar -zcf sluice-$(VERSION).tar.gz sluice-$(VERSION)
	rm -rf sluice-$(VERSION)

clean:
	rm -f sluice sluice.o sluice.1.gz
	rm -f sluice-$(VERSION).tar.gz

install: sluice sluice.1.gz
	mkdir -p ${DESTDIR}${BINDIR}
	cp sluice ${DESTDIR}${BINDIR}
	mkdir -p ${DESTDIR}${MANDIR}
	cp sluice.1.gz ${DESTDIR}${MANDIR}

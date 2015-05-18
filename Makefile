#
# Copyright (C) 2014-2015 Canonical, Ltd.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
#
VERSION=0.01.17

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


PREFIX?=/usr

VERSION=0.8

CFLAGS=-std=c99 -DVERSION='"$(VERSION)"' -O2 -Wall
LDLIBS=-lfuse
LDFLAGS=-s

all: chunkfs unchunkfs chunkfs.1.gz

install:
	install chunkfs ${DESTDIR}${PREFIX}/bin/
	install unchunkfs ${DESTDIR}${PREFIX}/bin/
	install -d ${DESTDIR}${PREFIX}/share/doc/chunkfs/examples/
	install writeoverlay.sh ${DESTDIR}${PREFIX}/share/doc/chunkfs/examples/
	install -m 644 chunkfs.1.gz ${DESTDIR}${PREFIX}/share/man/man1/
	ln -s chunkfs.1.gz ${DESTDIR}${PREFIX}/share/man/man1/unchunkfs.1.gz

clean:
	-rm chunkfs.o unchunkfs.o utils.o chunkfs unchunkfs chunkfs.1.gz

chunkfs: chunkfs.o utils.o

unchunkfs: unchunkfs.o utils.o

chunkfs.1.gz: manpage.pod
	pod2man -c '' -n CHUNKFS -r 'ChunkFS ${VERSION}' -s 1 $< | gzip > $@

.PHONY: all install clean


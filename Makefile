
PREFIX?=/usr/local

VERSION=0.2

CFLAGS=-std=c99 -O2 -Wall
LDFLAGS=-lfuse

all: chunkfs unchunkfs chunkfs.1.gz

install:
	install -o root -g root chunkfs ${DESTDIR}${PREFIX}/bin/
	install -o root -g root unchunkfs ${DESTDIR}${PREFIX}/bin/
	install -o root -g root -m 644 chunkfs.1.gz ${DESTDIR}${PREFIX}/share/man/man1/
	ln -s chunkfs.1.gz ${DESTDIR}${PREFIX}/share/man/man1/unchunkfs.1.gz

clean:
	-rm chunkfs.o unchunkfs.o utils.o chunkfs unchunkfs chunkfs.1.gz

chunkfs: chunkfs.o utils.o

unchunkfs: unchunkfs.o utils.o

chunkfs.1.gz: manpage.pod
	pod2man -c '' -n CHUNKFS -r 'ChunkFS ${VERSION}' -s 1 $< | gzip > $@

.PHONY: all install clean



PREFIX?=/usr/local

VERSION=0.1

CFLAGS=-std=c99 -O2 -Wall
LDFLAGS=-lfuse

all: chunkfs chunkfs.1.gz

install:
	install -o root -g root chunkfs ${DESTDIR}${PREFIX}/bin/
	install -o root -g root -m 644 chunkfs.1.gz ${DESTDIR}${PREFIX}/share/man/man1/

clean:
	-rm chunkfs chunkfs.1.gz

chunkfs.1.gz: manpage.pod
	pod2man -c '' -n CHUNKFS -r 'ChunkFS ${VERSION}' -s 1 $< | gzip > $@

.PHONY: all install clean


CC=gcc
CFLAGS=-O3 -g
LIBS=-pthread

all: writebuffer fwritebuffer

writebuffer: writebuffer.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

fwritebuffer: writebuffer.c
	$(CC) $(CFLAGS) -DFILE_BUFFER $< $(LIBS) -o $@

clean:
	rm -f writebuffer fwritebuffer

CC = gcc
CFLAGS = -Wall -Wextra -Wpedantic -std=c11

TARGETS = server client

.PHONY: all clean

all: $(TARGETS)

server: server.c protocol.c protocol.h common.h
	$(CC) $(CFLAGS) -o $@ server.c protocol.c

client: client.c protocol.c protocol.h common.h
	$(CC) $(CFLAGS) -o $@ client.c protocol.c

clean:
	rm -f $(TARGETS)
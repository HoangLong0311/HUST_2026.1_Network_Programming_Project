CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -Wall -Wextra -Wpedantic -std=c11

TARGETS = server client
TEST_TARGETS = tests/test_protocol tests/test_auction_state tests/test_server_integration

.PHONY: all clean test

all: $(TARGETS)

server: server.c protocol.c protocol.h common.h auction_state.c auction_state.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ server.c protocol.c auction_state.c

client: client.c protocol.c protocol.h common.h auction_state.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ client.c protocol.c

tests/test_protocol: tests/test_protocol.c protocol.c protocol.h common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_protocol.c protocol.c

tests/test_auction_state: tests/test_auction_state.c auction_state.c auction_state.h common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_auction_state.c auction_state.c

tests/test_server_integration: tests/test_server_integration.c protocol.c protocol.h auction_state.h common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. -o $@ tests/test_server_integration.c protocol.c

test: server $(TEST_TARGETS)
	./tests/test_protocol
	./tests/test_auction_state
	./tests/test_server_integration

clean:
	$(RM) $(TARGETS) $(TEST_TARGETS)

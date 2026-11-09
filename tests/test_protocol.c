#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void test_header_encoding(void)
{
    const struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_RECONNECT, 0x01020304U, 3U, 0U
    };
    struct message_header decoded;
    uint8_t encoded[PROTOCOL_HEADER_SIZE];

    assert(encode_header(&header, encoded) == PROTOCOL_OK);
    assert(encoded[0] == 0x41U);
    assert(encoded[1] == 0x55U);
    assert(encoded[3] == MSG_RECONNECT);
    assert(encoded[4] == 0x01U);
    assert(encoded[7] == 0x04U);
    assert(decode_header(encoded, &decoded) == PROTOCOL_OK);
    assert(decoded.request_id == header.request_id);
    assert(decoded.payload_length == header.payload_length);

    encoded[0] = 0U;
    assert(decode_header(encoded, &decoded) == PROTOCOL_ERROR);
}

static void test_message_round_trip(void)
{
    const char payload[] = "P001|session-token";
    const struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_RECONNECT, 7U,
        (uint32_t)(sizeof(payload) - 1U), 0U
    };
    struct message received;
    int sockets[2];

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_message(sockets[0], &header, payload) == PROTOCOL_OK);
    assert(recv_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.type == MSG_RECONNECT);
    assert(received.header.request_id == 7U);
    assert(received.header.payload_length == sizeof(payload) - 1U);
    assert(memcmp(received.payload, payload, sizeof(payload) - 1U) == 0);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    test_header_encoding();
    test_message_round_trip();
    puts("protocol tests passed");
    return 0;
}

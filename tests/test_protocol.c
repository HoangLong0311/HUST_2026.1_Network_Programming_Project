#include "protocol.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
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

static void test_header_limits_and_validation(void)
{
    struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_CONNECT, 0xffffffffU, 0U, 0U
    };
    struct message_header decoded;
    uint8_t encoded[PROTOCOL_HEADER_SIZE];

    assert(encode_header(NULL, encoded) == PROTOCOL_ERROR);
    assert(encode_header(&header, NULL) == PROTOCOL_ERROR);
    assert(decode_header(NULL, &decoded) == PROTOCOL_ERROR);
    assert(encode_header(&header, encoded) == PROTOCOL_OK);
    assert(decode_header(encoded, NULL) == PROTOCOL_ERROR);

    header.payload_length = PROTOCOL_MAX_PAYLOAD;
    assert(encode_header(&header, encoded) == PROTOCOL_OK);
    assert(decode_header(encoded, &decoded) == PROTOCOL_OK);
    assert(decoded.payload_length == PROTOCOL_MAX_PAYLOAD);
    assert(decoded.request_id == 0xffffffffU);
    header.payload_length = PROTOCOL_MAX_PAYLOAD + 1U;
    assert(encode_header(&header, encoded) == PROTOCOL_ERROR);

    /* 4097 in network byte order must be rejected on decode as well. */
    encoded[8] = 0U; encoded[9] = 0U; encoded[10] = 0x10U; encoded[11] = 1U;
    assert(decode_header(encoded, &decoded) == PROTOCOL_ERROR);
    encoded[11] = 0U;
    encoded[2] = PROTOCOL_VERSION + 1U;
    assert(decode_header(encoded, &decoded) == PROTOCOL_ERROR);
    encoded[2] = PROTOCOL_VERSION;
    encoded[15] = 1U;
    assert(decode_header(encoded, &decoded) == PROTOCOL_ERROR);
    encoded[15] = 0U;
    encoded[1] ^= 1U;
    assert(decode_header(encoded, &decoded) == PROTOCOL_ERROR);
}

static void test_fragmented_frame(void)
{
    const char payload[] = "fragmented";
    const struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_CONNECT, 19U,
        (uint32_t)(sizeof(payload) - 1U), 0U
    };
    uint8_t wire[PROTOCOL_HEADER_SIZE + sizeof(payload) - 1U];
    struct message received;
    int sockets[2];
    int status;
    pid_t child;

    assert(encode_header(&header, wire) == PROTOCOL_OK);
    memcpy(wire + PROTOCOL_HEADER_SIZE, payload, sizeof(payload) - 1U);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        size_t index;
        const struct timespec delay = {0, 1000000L};

        close(sockets[1]);
        for (index = 0; index < sizeof(wire); index++) {
            if (send_all(sockets[0], &wire[index], 1U) != PROTOCOL_OK) {
                _exit(EXIT_FAILURE);
            }
            (void)nanosleep(&delay, NULL);
        }
        close(sockets[0]);
        _exit(EXIT_SUCCESS);
    }
    close(sockets[0]);
    assert(recv_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.request_id == 19U);
    assert(received.header.payload_length == sizeof(payload) - 1U);
    assert(memcmp(received.payload, payload, sizeof(payload) - 1U) == 0);
    close(sockets[1]);
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS);
}

static void test_coalesced_frames(void)
{
    const struct message_header first = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_CONNECT, 1U, 1U, 0U
    };
    const struct message_header second = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_LIST_ROOMS, 2U, 0U, 0U
    };
    uint8_t wire[PROTOCOL_HEADER_SIZE * 2U + 1U];
    struct message received;
    int sockets[2];

    assert(encode_header(&first, wire) == PROTOCOL_OK);
    wire[PROTOCOL_HEADER_SIZE] = 'A';
    assert(encode_header(&second, wire + PROTOCOL_HEADER_SIZE + 1U) ==
           PROTOCOL_OK);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_all(sockets[0], wire, sizeof(wire)) == PROTOCOL_OK);
    assert(recv_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.type == MSG_CONNECT && received.payload[0] == 'A');
    assert(recv_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.type == MSG_LIST_ROOMS);
    assert(received.header.request_id == 2U);
    assert(received.header.payload_length == 0U);
    close(sockets[0]);
    close(sockets[1]);
}

static void test_eof_and_truncated_frames(void)
{
    const struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_CONNECT, 1U, 3U, 0U
    };
    uint8_t wire[PROTOCOL_HEADER_SIZE];
    struct message received;
    int sockets[2];

    assert(encode_header(&header, wire) == PROTOCOL_OK);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    close(sockets[0]);
    assert(recv_message(sockets[1], &received) == PROTOCOL_DISCONNECT);
    close(sockets[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_all(sockets[0], wire, 5U) == PROTOCOL_OK);
    close(sockets[0]);
    assert(recv_message(sockets[1], &received) == PROTOCOL_ERROR);
    close(sockets[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_all(sockets[0], wire, sizeof(wire)) == PROTOCOL_OK);
    assert(send_all(sockets[0], "ab", 2U) == PROTOCOL_OK);
    close(sockets[0]);
    assert(recv_message(sockets[1], &received) == PROTOCOL_ERROR);
    close(sockets[1]);

    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_all(sockets[0], wire, sizeof(wire)) == PROTOCOL_OK);
    close(sockets[0]);
    assert(recv_message(sockets[1], &received) == PROTOCOL_ERROR);
    close(sockets[1]);
}

static void test_maximum_payload(void)
{
    struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, MSG_CONNECT, 42U,
        PROTOCOL_MAX_PAYLOAD, 0U
    };
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
    struct message received;
    int sockets[2];
    size_t index;

    for (index = 0; index < sizeof(payload); index++) {
        payload[index] = (uint8_t)(index % 256U);
    }
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(send_message(sockets[0], &header, NULL) == PROTOCOL_ERROR);
    assert(send_message(sockets[0], &header, payload) == PROTOCOL_OK);
    assert(recv_message(sockets[1], &received) == PROTOCOL_OK);
    assert(received.header.payload_length == PROTOCOL_MAX_PAYLOAD);
    assert(memcmp(received.payload, payload, sizeof(payload)) == 0);
    close(sockets[0]);
    close(sockets[1]);
}

int main(void)
{
    /* Bound the runtime if a regression causes a blocking receive. */
    alarm(10);
    test_header_encoding();
    test_message_round_trip();
    test_header_limits_and_validation();
    test_fragmented_frame();
    test_coalesced_frames();
    test_eof_and_truncated_frames();
    test_maximum_payload();
    alarm(0);
    puts("protocol tests passed");
    return 0;
}

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #expr); \
    goto cleanup; } } while (0)

struct observed_events {
    int fd;
    const char *room;
    unsigned updates;
    unsigned warnings;
    unsigned results;
};
static struct observed_events observed[3];
static char last_response[PROTOCOL_MAX_PAYLOAD + 1U];

static int record_event(int fd, const struct message *msg, const char *payload)
{
    size_t i;
    if (msg->header.type != MSG_TIME_UPDATE &&
        msg->header.type != MSG_TIME_WARNING &&
        msg->header.type != MSG_AUCTION_RESULT) return 1;
    if (msg->header.request_id != 0U) return 0;
    for (i = 0; i < 3U; i++) {
        if (observed[i].fd == fd) {
            size_t len = strlen(observed[i].room);
            if (strncmp(payload, observed[i].room, len) != 0 ||
                payload[len] != '|') return 0;
            if (msg->header.type == MSG_TIME_UPDATE) observed[i].updates++;
            if (msg->header.type == MSG_TIME_WARNING) observed[i].warnings++;
            if (msg->header.type == MSG_AUCTION_RESULT &&
                ++observed[i].results > 1U) return 0;
            return 1;
        }
    }
    return 0;
}

static pid_t start_server(int *output_fd)
{
    int output_pipe[2];
    pid_t child;

    if (pipe(output_pipe) != 0) return -1;
    child = fork();
    if (child == 0) {
        close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(output_pipe[1]);
        execl("./server", "./server", "0", "5000", (char *)NULL);
        _exit(127);
    }
    close(output_pipe[1]);
    if (child < 0) { close(output_pipe[0]); return -1; }
    *output_fd = output_pipe[0];
    return child;
}

static int read_port(int fd)
{
    char line[128];
    unsigned int port;
    size_t used = 0;
    struct pollfd pfd = {fd, POLLIN, 0};

    while (used + 1U < sizeof(line)) {
        int ready = poll(&pfd, 1, 5000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(pfd.revents & (POLLIN | POLLHUP))) return -1;
        if (read(fd, line + used, 1) != 1) return -1;
        if (line[used++] == '\n') break;
    }
    line[used] = '\0';
    if (sscanf(line, "Server dang lang nghe tai port %u", &port) != 1 ||
        port == 0 || port > 65535U) return -1;
    return (int)port;
}

static int connect_client(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    struct timeval timeout = {8, 0};

    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_request(int fd, uint8_t type, uint32_t id, const char *payload)
{
    struct message_header header = {PROTOCOL_MAGIC, PROTOCOL_VERSION, type, id,
                                    (uint32_t)strlen(payload), 0};
    return send_message(fd, &header, payload) == PROTOCOL_OK;
}

static int receive_type(int fd, uint8_t type, uint32_t id, const char *expected)
{
    struct message msg;
    char text[PROTOCOL_MAX_PAYLOAD + 1U];
    unsigned int i;

    for (i = 0; i < 32U; i++) {
        if (recv_message(fd, &msg) != PROTOCOL_OK) return 0;
        memcpy(text, msg.payload, msg.header.payload_length);
        text[msg.header.payload_length] = '\0';
        if (!record_event(fd, &msg, text)) return 0;
        if (msg.header.type != type || msg.header.request_id != id) continue;
        memcpy(last_response, text, msg.header.payload_length + 1U);
        return expected == NULL || strstr(text, expected) != NULL;
    }
    return 0;
}

static int join_room(int fd, uint32_t id, const char *room)
{
    return send_request(fd, MSG_JOIN_ROOM, id, room) &&
           receive_type(fd, MSG_JOIN_RESULT, id, "OK|") &&
           receive_type(fd, MSG_ROOM_STATE, id, room);
}

int main(void)
{
    int output = -1, alice = -1, bob = -1, carol = -1, rogue = -1;
    int status = EXIT_FAILURE, port;
    char reconnect_payload[128];
    uint8_t bad_header[PROTOCOL_HEADER_SIZE] = {0};
    char byte;
    pid_t child = start_server(&output);

    signal(SIGPIPE, SIG_IGN);
    CHECK(child > 0);
    port = read_port(output);
    CHECK(port > 0);
    rogue = connect_client(port);
    CHECK(rogue >= 0);
    CHECK(send(rogue, bad_header, 1U, 0) == 1);
    alice = connect_client(port);
    bob = connect_client(port);
    carol = connect_client(port);
    CHECK(alice >= 0 && bob >= 0 && carol >= 0);
    observed[0].fd = alice; observed[0].room = "R001";
    observed[1].fd = bob; observed[1].room = "R001";
    observed[2].fd = carol; observed[2].room = "R002";
    CHECK(send_request(alice, MSG_CONNECT, 1U, "Alice"));
    CHECK(receive_type(alice, MSG_CONNECT_RESULT, 1U, "OK|P001|"));
    CHECK(send_request(bob, MSG_CONNECT, 2U, "Bob"));
    CHECK(receive_type(bob, MSG_CONNECT_RESULT, 2U, "OK|P002|"));
    CHECK(strlen(last_response) > 3U && strlen(last_response + 3) < sizeof(reconnect_payload));
    strcpy(reconnect_payload, last_response + 3);
    CHECK(send_request(carol, MSG_CONNECT, 3U, "Carol"));
    CHECK(receive_type(carol, MSG_CONNECT_RESULT, 3U, "OK|P003|"));
    /* A peer leaving a header incomplete must not block the other clients;
       once the invalid header is complete, only that peer is disconnected. */
    CHECK(send(rogue, bad_header + 1U, sizeof(bad_header) - 1U, 0) ==
          (ssize_t)(sizeof(bad_header) - 1U));
    CHECK(recv(rogue, &byte, 1U, 0) == 0);
    close(rogue);
    rogue = -1;
    CHECK(join_room(alice, 4U, "R001"));
    CHECK(join_room(bob, 5U, "R001"));
    CHECK(join_room(carol, 6U, "R002"));
    CHECK(send_request(alice, MSG_PLACE_BID, 7U, "R001|A001|100000"));
    CHECK(receive_type(alice, MSG_BID_RESULT, 7U, "OK|R001|100000"));
    CHECK(receive_type(bob, MSG_BID_UPDATE, 0U, "R001|A001|100000|P001"));
    CHECK(receive_type(alice, MSG_AUCTION_RESULT, 0U, "R001|A001|SOLD|P001|100000"));
    CHECK(receive_type(bob, MSG_AUCTION_RESULT, 0U, "R001|A001|SOLD|P001|100000"));
    CHECK(receive_type(carol, MSG_AUCTION_RESULT, 0U, "R002|A001|NO_BID"));
    CHECK(observed[0].updates > 0U && observed[1].updates > 0U &&
          observed[2].updates > 0U);
    CHECK(observed[0].warnings > 0U && observed[1].warnings > 0U &&
          observed[2].warnings > 0U);
    CHECK(send_request(alice, MSG_PLACE_BID, 8U, "R001|A001|110000"));
    CHECK(receive_type(alice, MSG_BID_RESULT, 8U, "FAIL|AUCTION_NOT_ACTIVE"));
    close(bob);
    bob = -1;
    bob = connect_client(port);
    CHECK(bob >= 0);
    observed[1].fd = bob;
    observed[1].results = 0U;
    CHECK(send_request(bob, MSG_RECONNECT, 9U, reconnect_payload));
    CHECK(receive_type(bob, MSG_RECONNECT_RESULT, 9U, "OK|P002|"));
    CHECK(receive_type(bob, MSG_AUCTION_RESULT, 0U,
                       "R001|A001|SOLD|P001|100000"));
    CHECK(send_request(bob, MSG_GET_SESSION_STATE, 10U, ""));
    CHECK(receive_type(bob, MSG_SESSION_STATE, 10U, "OK|R001"));
    CHECK(send_request(bob, MSG_GET_AUCTION_STATE, 11U, "R001"));
    CHECK(receive_type(bob, MSG_AUCTION_STATE, 11U,
                       "R001|A001|ENDED|100000|P001|0|100000|10000|1"));
    status = EXIT_SUCCESS;
cleanup:
    if (alice >= 0) close(alice);
    if (bob >= 0) close(bob);
    if (carol >= 0) close(carol);
    if (rogue >= 0) close(rogue);
    if (child > 0) { kill(child, SIGTERM); waitpid(child, NULL, 0); }
    if (output >= 0) close(output);
    if (status == EXIT_SUCCESS) puts("server auction timer integration passed");
    return status;
}

#include "auction_state.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define RESPONSE_CAPACITY 256U
#define SERVER_START_ATTEMPTS 100U

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, \
                    #condition);                                                \
            goto cleanup;                                                       \
        }                                                                       \
    } while (0)

static void wait_before_retry(void)
{
    const struct timespec delay = {0, 20L * 1000L * 1000L};

    (void)nanosleep(&delay, NULL);
}

static int server_is_running(pid_t server_process)
{
    /* Do not reap the child here: cleanup must not signal a recycled PID. */
    return kill(server_process, 0) == 0;
}

static int connect_to_server(uint16_t port, pid_t server_process)
{
    struct sockaddr_in address;
    unsigned int attempt;

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        return -1;
    }

    for (attempt = 0; attempt < SERVER_START_ATTEMPTS; attempt++) {
        int socket_fd;

        if (!server_is_running(server_process)) {
            return -1;
        }
        socket_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (socket_fd < 0) {
            return -1;
        }
        if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) ==
            0) {
            return socket_fd;
        }
        {
            int connect_error = errno;

            (void)close(socket_fd);
            if (connect_error != ECONNREFUSED && connect_error != EINTR) {
                return -1;
            }
        }
        wait_before_retry();
    }
    return -1;
}

static int send_request(int socket_fd, uint8_t type, uint32_t request_id,
                        const char *payload)
{
    struct message_header header;
    size_t payload_length = payload == NULL ? 0U : strlen(payload);

    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.request_id = request_id;
    header.payload_length = (uint32_t)payload_length;
    header.reserved = 0;
    return send_message(socket_fd, &header, payload) == PROTOCOL_OK;
}

static int receive_response(int socket_fd, uint32_t request_id,
                            uint8_t expected_type,
                            char response[RESPONSE_CAPACITY])
{
    struct message message;

    if (recv_message(socket_fd, &message) != PROTOCOL_OK ||
        message.header.request_id != request_id ||
        message.header.type != expected_type ||
        message.header.payload_length >= RESPONSE_CAPACITY) {
        return 0;
    }
    memcpy(response, message.payload, message.header.payload_length);
    response[message.header.payload_length] = '\0';
    return 1;
}

static int parse_session(const char *response,
                         char participant_id[PARTICIPANT_ID_CAPACITY],
                         char token[SESSION_TOKEN_CAPACITY])
{
    char value[RESPONSE_CAPACITY];
    char *participant_id_start;
    char *token_start;

    if (snprintf(value, sizeof(value), "%s", response) >= (int)sizeof(value)) {
        return 0;
    }
    if (strncmp(value, "OK|", 3U) != 0) {
        return 0;
    }
    participant_id_start = value + 3U;
    token_start = strchr(participant_id_start, '|');
    if (token_start == NULL || token_start[1] == '\0' ||
        strchr(token_start + 1, '|') != NULL) {
        return 0;
    }
    *token_start = '\0';
    if (strlen(participant_id_start) >= PARTICIPANT_ID_CAPACITY ||
        strlen(token_start + 1) >= SESSION_TOKEN_CAPACITY) {
        return 0;
    }
    (void)snprintf(participant_id, PARTICIPANT_ID_CAPACITY, "%s",
                   participant_id_start);
    (void)snprintf(token, SESSION_TOKEN_CAPACITY, "%s", token_start + 1);
    return 1;
}

static pid_t start_server(int *output_fd)
{
    int output_pipe[2];
    pid_t process_id;

    if (pipe(output_pipe) != 0) {
        return -1;
    }
    process_id = fork();

    if (process_id == 0) {
        (void)close(output_pipe[0]);
        if (dup2(output_pipe[1], STDOUT_FILENO) < 0) {
            _exit(EXIT_FAILURE);
        }
        (void)close(output_pipe[1]);
        execl("./server", "./server", "0", (char *)NULL);
        _exit(EXIT_FAILURE);
    }
    (void)close(output_pipe[1]);
    if (process_id < 0) {
        (void)close(output_pipe[0]);
        return -1;
    }
    *output_fd = output_pipe[0];
    return process_id;
}

static int read_server_port(int output_fd, uint16_t *port)
{
    char line[128];
    size_t used = 0;
    unsigned int parsed_port;
    struct pollfd readiness = {output_fd, POLLIN, 0};

    while (used + 1U < sizeof(line)) {
        int ready = poll(&readiness, 1, 5000);
        ssize_t received;

        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0 || !(readiness.revents & (POLLIN | POLLHUP))) {
            return 0;
        }
        received = read(output_fd, line + used, 1);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received != 1) {
            return 0;
        }
        if (line[used++] == '\n') {
            break;
        }
    }
    line[used] = '\0';
    if (used == 0 || line[used - 1U] != '\n' ||
        sscanf(line, "Server dang lang nghe tai port %u...", &parsed_port) != 1 ||
        parsed_port == 0U || parsed_port > UINT16_MAX) {
        return 0;
    }
    *port = (uint16_t)parsed_port;
    return 1;
}

int main(void)
{
    char response[RESPONSE_CAPACITY];
    char initial_session[RESPONSE_CAPACITY];
    char participant_id[PARTICIPANT_ID_CAPACITY];
    char token[SESSION_TOKEN_CAPACITY];
    char reconnect_payload[PARTICIPANT_ID_CAPACITY + SESSION_TOKEN_CAPACITY];
    int socket_fd = -1;
    int server_output = -1;
    int exit_status = EXIT_FAILURE;
    uint16_t test_port = 0;
    pid_t server_process = start_server(&server_output);

    CHECK(server_process > 0);
    CHECK(read_server_port(server_output, &test_port));
    socket_fd = connect_to_server(test_port, server_process);
    CHECK(socket_fd >= 0);

    CHECK(send_request(socket_fd, MSG_LIST_ROOMS, 1U, NULL));
    CHECK(receive_response(socket_fd, 1U, MSG_ERROR, response));
    CHECK(strcmp(response, "NOT_CONNECTED") == 0);

    CHECK(send_request(socket_fd, MSG_CONNECT, 2U, "Alice"));
    CHECK(receive_response(socket_fd, 2U, MSG_CONNECT_RESULT, response));
    CHECK(parse_session(response, participant_id, token));
    CHECK(strcmp(participant_id, "P001") == 0);
    CHECK(strlen(token) == SESSION_TOKEN_CAPACITY - 1U);
    (void)snprintf(initial_session, sizeof(initial_session), "%s", response);
    (void)close(socket_fd);
    socket_fd = -1;

    socket_fd = connect_to_server(test_port, server_process);
    CHECK(socket_fd >= 0);
    CHECK(send_request(socket_fd, MSG_RECONNECT, 3U, "P001|bad-token"));
    CHECK(receive_response(socket_fd, 3U, MSG_ERROR, response));
    CHECK(strcmp(response, "INVALID_RECONNECT") == 0);
    (void)close(socket_fd);
    socket_fd = -1;

    socket_fd = connect_to_server(test_port, server_process);
    CHECK(socket_fd >= 0);
    CHECK(snprintf(reconnect_payload, sizeof(reconnect_payload), "%s|%s",
                   participant_id, token) < (int)sizeof(reconnect_payload));
    CHECK(send_request(socket_fd, MSG_RECONNECT, 4U, reconnect_payload));
    CHECK(receive_response(socket_fd, 4U, MSG_RECONNECT_RESULT, response));
    CHECK(parse_session(response, participant_id, token));
    CHECK(strcmp(participant_id, "P001") == 0);
    CHECK(strcmp(response, initial_session) == 0);
    (void)close(socket_fd);
    socket_fd = -1;

    socket_fd = connect_to_server(test_port, server_process);
    CHECK(socket_fd >= 0);
    CHECK(send_request(socket_fd, MSG_RECONNECT, 5U, reconnect_payload));
    /* Discard the response, then retry with the last token the client kept. */
    CHECK(shutdown(socket_fd, SHUT_WR) == 0);
    CHECK(receive_response(socket_fd, 5U, MSG_RECONNECT_RESULT, response));
    (void)close(socket_fd);
    socket_fd = -1;

    socket_fd = connect_to_server(test_port, server_process);
    CHECK(socket_fd >= 0);
    CHECK(snprintf(reconnect_payload, sizeof(reconnect_payload), "%s|%s",
                   participant_id, token) < (int)sizeof(reconnect_payload));
    CHECK(send_request(socket_fd, MSG_RECONNECT, 6U, reconnect_payload));
    CHECK(receive_response(socket_fd, 6U, MSG_RECONNECT_RESULT, response));

    CHECK(send_request(socket_fd, MSG_LIST_ROOMS, 7U, NULL));
    CHECK(receive_response(socket_fd, 7U, MSG_ROOM_LIST, response));
    CHECK(strstr(response, "R001|Demo Room\n") != NULL);
    CHECK(strstr(response, "R002|Practice Room\n") != NULL);

    CHECK(send_request(socket_fd, MSG_CREATE_ROOM, 8U, "Sunday Auction"));
    CHECK(receive_response(socket_fd, 8U, MSG_CREATE_ROOM_RESULT, response));
    CHECK(strcmp(response, "OK|R003|Sunday Auction") == 0);

    CHECK(send_request(socket_fd, MSG_LIST_ROOMS, 9U, NULL));
    CHECK(receive_response(socket_fd, 9U, MSG_ROOM_LIST, response));
    CHECK(strstr(response, "R003|Sunday Auction\n") != NULL);

    exit_status = EXIT_SUCCESS;

cleanup:
    if (socket_fd >= 0) {
        (void)close(socket_fd);
    }
    if (server_process > 0) {
        (void)kill(server_process, SIGTERM);
        (void)waitpid(server_process, NULL, 0);
    }
    if (server_output >= 0) {
        (void)close(server_output);
    }
    if (exit_status == EXIT_SUCCESS) {
        puts("server integration test passed");
    }
    return exit_status;
}

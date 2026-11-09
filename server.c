#include "auction_state.h"
#include "logger.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define MAX_CLIENTS 64U
#define INPUT_BUFFER_SIZE (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD) * 2U

static struct logger server_logger;

static int send_payload(int socket_fd, uint8_t type, uint32_t request_id,
                        const void *payload, size_t payload_length)
{
    struct message_header header;

    if (payload_length > PROTOCOL_MAX_PAYLOAD) {
        return PROTOCOL_ERROR;
    }
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.request_id = request_id;
    header.payload_length = (uint32_t)payload_length;
    header.reserved = 0;
    if (send_message(socket_fd, &header, payload) != PROTOCOL_OK) {
        logger_event(&server_logger, "ERROR", "send_message failed");
        return PROTOCOL_ERROR;
    }
    logger_message(&server_logger, "SEND", &header, payload);
    return PROTOCOL_OK;
}

static int send_text(int socket_fd, uint8_t type, uint32_t request_id,
                     const char *text)
{
    return send_payload(socket_fd, type, request_id, text, strlen(text));
}

static int send_error(int socket_fd, uint32_t request_id, const char *code)
{
    return send_text(socket_fd, MSG_ERROR, request_id, code);
}

static int payload_to_text(const struct message *message, char *destination,
                           size_t destination_capacity, int allow_empty,
                           int allow_pipe)
{
    uint32_t index;

    if (message == NULL || destination == NULL || destination_capacity == 0 ||
        message->header.payload_length >= destination_capacity ||
        (!allow_empty && message->header.payload_length == 0)) {
        return -1;
    }

    for (index = 0; index < message->header.payload_length; index++) {
        uint8_t byte = message->payload[index];

        if (byte < 0x20U || byte > 0x7eU ||
            (!allow_pipe && byte == (uint8_t)'|')) {
            return -1;
        }
    }

    memcpy(destination, message->payload, message->header.payload_length);
    destination[message->header.payload_length] = '\0';
    return 0;
}

static int parse_reconnect(const struct message *message,
                           char participant_id[PARTICIPANT_ID_CAPACITY],
                           char token[SESSION_TOKEN_CAPACITY])
{
    char value[PARTICIPANT_ID_CAPACITY + SESSION_TOKEN_CAPACITY];
    char *separator;
    size_t participant_id_length;
    size_t token_length;

    if (payload_to_text(message, value, sizeof(value), 0, 1) != 0) {
        return -1;
    }
    separator = strchr(value, '|');
    if (separator == NULL || separator == value || separator[1] == '\0' ||
        strchr(separator + 1, '|') != NULL) {
        return -1;
    }

    *separator = '\0';
    participant_id_length = strlen(value);
    token_length = strlen(separator + 1);
    if (participant_id_length >= PARTICIPANT_ID_CAPACITY ||
        token_length >= SESSION_TOKEN_CAPACITY) {
        return -1;
    }
    memcpy(participant_id, value, participant_id_length + 1U);
    memcpy(token, separator + 1, token_length + 1U);
    return 0;
}

static int send_participant_result(int socket_fd, uint8_t type,
                                   uint32_t request_id,
                                   const struct participant *participant)
{
    char payload[PROTOCOL_MAX_PAYLOAD];
    int written = snprintf(payload, sizeof(payload), "OK|%s|%s",
                           participant->id, participant->token);

    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    return send_text(socket_fd, type, request_id, payload);
}

static int send_room_list(int socket_fd, uint32_t request_id,
                          const struct auction_state *state)
{
    char payload[PROTOCOL_MAX_PAYLOAD];
    size_t used = 0;
    size_t index;

    for (index = 0; index < state->room_count; index++) {
        const struct auction_room *room = auction_room_at(state, index);
        int written;

        if (room == NULL) {
            return PROTOCOL_ERROR;
        }
        written = snprintf(payload + used, sizeof(payload) - used, "%s|%s\n",
                           room->id, room->name);
        if (written < 0 || (size_t)written >= sizeof(payload) - used) {
            return PROTOCOL_ERROR;
        }
        used += (size_t)written;
    }

    return send_payload(socket_fd, MSG_ROOM_LIST, request_id, payload, used);
}

static int handle_connect(int socket_fd, const struct message *message,
                          struct auction_state *state,
                          struct participant **current_participant)
{
    char name[PARTICIPANT_NAME_CAPACITY];
    struct participant *participant;
    enum state_result result;

    if (*current_participant != NULL) {
        return send_error(socket_fd, message->header.request_id,
                          "ALREADY_CONNECTED");
    }
    if (payload_to_text(message, name, sizeof(name), 1, 0) != 0) {
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_PARTICIPANT_NAME");
    }

    result = participant_connect(state, name, &participant);
    if (result == STATE_CAPACITY_REACHED) {
        return send_error(socket_fd, message->header.request_id,
                          "PARTICIPANT_LIMIT_REACHED");
    }
    if (result == STATE_INTERNAL_ERROR) {
        return send_error(socket_fd, message->header.request_id,
                          "INTERNAL_ERROR");
    }
    if (result != STATE_OK) {
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_PARTICIPANT_NAME");
    }

    *current_participant = participant;
    return send_participant_result(socket_fd, MSG_CONNECT_RESULT,
                                   message->header.request_id, participant);
}

static int handle_reconnect(int socket_fd, const struct message *message,
                            struct auction_state *state,
                            struct participant **current_participant)
{
    char participant_id[PARTICIPANT_ID_CAPACITY];
    char token[SESSION_TOKEN_CAPACITY];
    struct participant *participant;
    enum state_result result;

    if (*current_participant != NULL) {
        return send_error(socket_fd, message->header.request_id,
                          "ALREADY_CONNECTED");
    }
    if (parse_reconnect(message, participant_id, token) != 0) {
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_RECONNECT");
    }

    result = participant_reconnect(state, participant_id, token, &participant);
    switch (result) {
    case STATE_OK:
        *current_participant = participant;
        return send_participant_result(socket_fd, MSG_RECONNECT_RESULT,
                                       message->header.request_id, participant);
    case STATE_NOT_FOUND:
    case STATE_TOKEN_MISMATCH:
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_RECONNECT");
    case STATE_ALREADY_CONNECTED:
        return send_error(socket_fd, message->header.request_id,
                          "ALREADY_CONNECTED");
    case STATE_INTERNAL_ERROR:
        return send_error(socket_fd, message->header.request_id,
                          "INTERNAL_ERROR");
    default:
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_RECONNECT");
    }
}

static int handle_create_room(int socket_fd, const struct message *message,
                              struct auction_state *state,
                              const struct participant *current_participant)
{
    char name[ROOM_NAME_CAPACITY];
    char payload[PROTOCOL_MAX_PAYLOAD];
    struct auction_room *room;
    enum state_result result;
    int written;

    if (current_participant == NULL) {
        return send_error(socket_fd, message->header.request_id,
                          "NOT_CONNECTED");
    }
    if (payload_to_text(message, name, sizeof(name), 0, 0) != 0) {
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_ROOM_NAME");
    }

    result = auction_room_create(state, current_participant, name, &room);
    if (result == STATE_CAPACITY_REACHED) {
        return send_error(socket_fd, message->header.request_id,
                          "ROOM_LIMIT_REACHED");
    }
    if (result == STATE_DUPLICATE_NAME) {
        return send_error(socket_fd, message->header.request_id,
                          "ROOM_NAME_EXISTS");
    }
    if (result != STATE_OK) {
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_ROOM_NAME");
    }

    written = snprintf(payload, sizeof(payload), "OK|%s|%s", room->id,
                       room->name);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    return send_text(socket_fd, MSG_CREATE_ROOM_RESULT,
                     message->header.request_id, payload);
}

struct client_connection {
    int socket_fd;
    size_t input_length;
    uint8_t input[INPUT_BUFFER_SIZE];
    struct participant *participant;
};

static int process_message(struct client_connection *client,
                           const struct message *message,
                           struct auction_state *state)
{
    int response_result;

    logger_message(&server_logger, "RECV", &message->header, message->payload);
    switch (message->header.type) {
    case MSG_CONNECT:
        response_result = handle_connect(client->socket_fd, message, state,
                                         &client->participant);
        break;
    case MSG_RECONNECT:
        response_result = handle_reconnect(client->socket_fd, message, state,
                                           &client->participant);
        break;
    case MSG_LIST_ROOMS:
        if (client->participant == NULL) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "NOT_CONNECTED");
        } else if (message->header.payload_length != 0U) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "UNEXPECTED_PAYLOAD");
        } else {
            response_result = send_room_list(client->socket_fd,
                                             message->header.request_id, state);
        }
        break;
    case MSG_CREATE_ROOM:
        response_result = handle_create_room(client->socket_fd, message, state,
                                             client->participant);
        break;
    default:
        response_result = send_error(client->socket_fd,
                                     message->header.request_id,
                                     "UNKNOWN_MESSAGE_TYPE");
        break;
    }

    return response_result;
}

static int process_input(struct client_connection *client,
                         struct auction_state *state)
{
    while (client->input_length >= PROTOCOL_HEADER_SIZE) {
        struct message message = {0};
        struct message_header header;
        size_t frame_length;

        if (decode_header(client->input, &header) != PROTOCOL_OK) {
            logger_event(&server_logger, "ERROR",
                         "invalid frame header; closing client");
            return -1;
        }
        frame_length = PROTOCOL_HEADER_SIZE + header.payload_length;
        if (client->input_length < frame_length) {
            return 0;
        }
        message.header = header;
        if (header.payload_length > 0) {
            memcpy(message.payload, client->input + PROTOCOL_HEADER_SIZE,
                   header.payload_length);
        }
        client->input_length -= frame_length;
        if (client->input_length > 0) {
            memmove(client->input, client->input + frame_length,
                    client->input_length);
        }
        if (process_message(client, &message, state) != PROTOCOL_OK) {
            return -1;
        }
    }
    return 0;
}

static int read_client(struct client_connection *client,
                        struct auction_state *state)
{
    for (;;) {
        ssize_t received;

        if (client->input_length == sizeof(client->input)) {
            logger_event(&server_logger, "ERROR",
                         "client input buffer exhausted");
            return -1;
        }
        received = recv(client->socket_fd,
                        client->input + client->input_length,
                        sizeof(client->input) - client->input_length,
                        MSG_DONTWAIT);
        if (received > 0) {
            client->input_length += (size_t)received;
            if (process_input(client, state) != 0) {
                return -1;
            }
            continue;
        }
        if (received == 0) {
            if (client->input_length != 0) {
                logger_event(&server_logger, "ERROR",
                             "client disconnected during frame");
            }
            return 1;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        logger_event(&server_logger, "ERROR", "client recv failed");
        return -1;
    }
}

static void close_client(struct client_connection *client)
{
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
    }
    participant_disconnect(client->participant);
    client->socket_fd = -1;
    client->input_length = 0;
    client->participant = NULL;
}

static int parse_port(const char *value, uint16_t *port)
{
    char *end;
    unsigned long parsed_port;

    errno = 0;
    parsed_port = strtoul(value, &end, 10);
    if (errno != 0 || value == end || *end != '\0' ||
        parsed_port > UINT16_MAX) {
        return -1;
    }
    *port = (uint16_t)parsed_port;
    return 0;
}

int main(int argc, char *argv[])
{
    int server_fd;
    int option = 1;
    struct pollfd poll_fds[MAX_CLIENTS + 1U];
    struct client_connection clients[MAX_CLIENTS] = {0};
    size_t client_count = 0;
    uint16_t server_port = SERVER_PORT;
    struct auction_state state;
    struct sockaddr_in server_address;

    if (argc == 2) {
        if (parse_port(argv[1], &server_port) != 0) {
            fprintf(stderr, "Su dung: %s [port]\n", argv[0]);
            return EXIT_FAILURE;
        }
    } else if (argc != 1) {
        fprintf(stderr, "Su dung: %s [port]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (logger_open(&server_logger, "auction.log", "SERVER") != 0) {
        perror("auction.log");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    auction_state_init(&state);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        logger_close(&server_logger);
        return EXIT_FAILURE;
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option,
                   sizeof(option)) < 0) {
        perror("setsockopt");
        close(server_fd);
        logger_close(&server_logger);
        return EXIT_FAILURE;
    }
    if (fcntl(server_fd, F_SETFL, O_NONBLOCK) < 0) {
        perror("fcntl");
        close(server_fd);
        logger_close(&server_logger);
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(server_port);
    if (bind(server_fd, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0 ||
        listen(server_fd, 5) < 0) {
        perror("bind/listen");
        close(server_fd);
        logger_close(&server_logger);
        return EXIT_FAILURE;
    }

    {
        socklen_t address_length = sizeof(server_address);

        if (getsockname(server_fd, (struct sockaddr *)&server_address,
                        &address_length) < 0) {
            perror("getsockname");
            close(server_fd);
            logger_close(&server_logger);
            return EXIT_FAILURE;
        }
        server_port = ntohs(server_address.sin_port);
    }

    printf("Server dang lang nghe tai port %u...\n", (unsigned int)server_port);
    fflush(stdout);
    for (;;) {
        size_t index;
        int ready;

        poll_fds[0].fd = server_fd;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;
        for (index = 0; index < client_count; index++) {
            poll_fds[index + 1U].fd = clients[index].socket_fd;
            poll_fds[index + 1U].events = POLLIN;
            poll_fds[index + 1U].revents = 0;
        }

        ready = poll(poll_fds, client_count + 1U, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            for (;;) {
                int client_fd = accept(server_fd, NULL, NULL);

                if (client_fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    perror("accept");
                    break;
                }
                if (client_count == MAX_CLIENTS) {
                    logger_event(&server_logger, "WARN",
                                 "client limit reached; connection rejected");
                    close(client_fd);
                    continue;
                }
                clients[client_count].socket_fd = client_fd;
                clients[client_count].input_length = 0;
                clients[client_count].participant = NULL;
                client_count++;
                printf("Client da ket noi (%zu/%u).\n", client_count,
                       MAX_CLIENTS);
                fflush(stdout);
            }
        }

        for (index = client_count; index > 0; index--) {
            size_t client_index = index - 1U;
            short events = poll_fds[client_index + 1U].revents;

            if (events & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) {
                int result = read_client(&clients[client_index], &state);

                if (result != 0) {
                    close_client(&clients[client_index]);
                    clients[client_index] = clients[client_count - 1U];
                    client_count--;
                }
            }
        }
    }

    while (client_count > 0) {
        close_client(&clients[client_count - 1U]);
        client_count--;
    }
    close(server_fd);
    logger_close(&server_logger);
    return EXIT_FAILURE;
}

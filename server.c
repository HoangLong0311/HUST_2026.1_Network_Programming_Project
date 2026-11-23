#include "auction_state.h"
#include "auction.h"
#include "logger.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 64U
#define INPUT_BUFFER_SIZE (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD) * 2U
#define OUTPUT_BUFFER_SIZE (PROTOCOL_HEADER_SIZE + PROTOCOL_MAX_PAYLOAD) * 8U
#define OUTPUT_STALL_TIMEOUT_MS UINT64_C(15000)
#define INPUT_STALL_TIMEOUT_MS UINT64_C(15000)
#define AUTH_TIMEOUT_MS UINT64_C(30000)

struct client_connection {
    int socket_fd;
    size_t input_length;
    uint64_t input_started_ms;
    uint64_t connected_ms;
    uint8_t input[INPUT_BUFFER_SIZE];
    size_t output_length;
    size_t output_offset;
    uint64_t output_progress_ms;
    uint8_t output[OUTPUT_BUFFER_SIZE];
    int output_failed;
    int input_closed;
    struct participant *participant;
};

static struct logger server_logger;
static struct client_connection *active_clients;
static size_t *active_client_count;
static uint64_t auction_duration_ms = AUCTION_DEFAULT_DURATION_MS;

static int monotonic_ms(uint64_t *value)
{
    struct timespec current;

    if (value == NULL || clock_gettime(CLOCK_MONOTONIC, &current) != 0 ||
        current.tv_sec < 0 ||
        (uint64_t)current.tv_sec > (UINT64_MAX - 999U) / 1000U) {
        return -1;
    }
    *value = (uint64_t)current.tv_sec * 1000U +
             (uint64_t)current.tv_nsec / 1000000U;
    return 0;
}

static int send_payload(int socket_fd, uint8_t type, uint32_t request_id,
                        const void *payload, size_t payload_length)
{
    struct message_header header;
    struct client_connection *client = NULL;
    uint8_t encoded[PROTOCOL_HEADER_SIZE];
    size_t index;
    size_t pending;

    if (payload_length > PROTOCOL_MAX_PAYLOAD) {
        return PROTOCOL_ERROR;
    }
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.request_id = request_id;
    header.payload_length = (uint32_t)payload_length;
    header.reserved = 0;
    if (encode_header(&header, encoded) != PROTOCOL_OK ||
        (payload_length != 0U && payload == NULL)) {
        return PROTOCOL_ERROR;
    }
    for (index = 0; index < *active_client_count; index++) {
        if (active_clients[index].socket_fd == socket_fd) {
            client = &active_clients[index];
            break;
        }
    }
    if (client == NULL || client->output_failed) {
        return PROTOCOL_ERROR;
    }
    pending = client->output_length - client->output_offset;
    if (PROTOCOL_HEADER_SIZE + payload_length > sizeof(client->output) - pending) {
        client->output_failed = 1;
        logger_event(&server_logger, "WARN", "client output queue full");
        return PROTOCOL_ERROR;
    }
    /* Track when a newly non-empty queue began waiting for socket progress. */
    if (pending == 0U && monotonic_ms(&client->output_progress_ms) != 0) {
        client->output_failed = 1;
        return PROTOCOL_ERROR;
    }
    if (client->output_offset != 0U) {
        memmove(client->output, client->output + client->output_offset, pending);
        client->output_offset = 0;
        client->output_length = pending;
    }
    memcpy(client->output + client->output_length, encoded, sizeof(encoded));
    client->output_length += sizeof(encoded);
    if (payload_length != 0U) {
        memcpy(client->output + client->output_length, payload, payload_length);
        client->output_length += payload_length;
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

static int send_join_failure(int socket_fd, uint32_t request_id,
                             const struct participant *participant,
                             const char *room_id, const char *reason)
{
    char payload[128];
    char event[160];
    int written;

    written = snprintf(payload, sizeof(payload), "FAIL|%s", reason);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    if (participant == NULL) {
        (void)snprintf(event, sizeof(event),
                       "[JOIN FAILED] UNKNOWN %s %s",
                       reason, room_id == NULL ? "-" : room_id);
    } else {
        (void)snprintf(event, sizeof(event),
                        "[JOIN FAILED] %s %s %s",
                        participant->id, reason,
                        room_id == NULL ? "-" : room_id);
    }
    logger_event(&server_logger, "INFO", event);
    return send_text(socket_fd, MSG_JOIN_RESULT, request_id, payload);
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

static int send_room_state(int socket_fd, uint32_t request_id,
                           const struct auction_room *room)
{
    char payload[PROTOCOL_MAX_PAYLOAD];
    size_t used;
    size_t index;
    int written;

    if (room == NULL) {
        return PROTOCOL_ERROR;
    }
    written = snprintf(payload, sizeof(payload), "%s|%s|%s|",
                       room->id, room->name,
                       room->ended ? "ENDED" : "OPEN");
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    used = (size_t)written;
    for (index = 0; index < room->participant_count; index++) {
        written = snprintf(payload + used, sizeof(payload) - used,
                           "%s%s", index == 0U ? "" : ",",
                           room->participant_ids[index]);
        if (written < 0 || (size_t)written >= sizeof(payload) - used) {
            return PROTOCOL_ERROR;
        }
        used += (size_t)written;
    }
    written = snprintf(payload + used, sizeof(payload) - used,
                       "|%zu", room->participant_count);
    if (written < 0 || (size_t)written >= sizeof(payload) - used) {
        return PROTOCOL_ERROR;
    }
    used += (size_t)written;
    return send_payload(socket_fd, MSG_ROOM_STATE, request_id, payload, used);
}

static int send_session_state(int socket_fd, uint32_t request_id,
                              const struct participant *participant)
{
    char payload[ROOM_ID_CAPACITY + 3U];
    int written;

    if (participant == NULL) {
        return send_error(socket_fd, request_id, "NOT_CONNECTED");
    }
    written = snprintf(payload, sizeof(payload), "OK|%s",
                       participant->current_room_id);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    return send_payload(socket_fd, MSG_SESSION_STATE, request_id,
                        payload, (size_t)written);
}

static const char *auction_status_text(const struct auction_room *room)
{
    if (room->auction_status == AUCTION_ENDED || room->ended) {
        return "ENDED";
    }
    if (room->auction_status == AUCTION_RUNNING && room->timer_active) {
        return "RUNNING";
    }
    return "WAITING";
}

static int send_auction_state(int socket_fd, uint32_t request_id,
                              const struct auction_room *room,
                              uint64_t now)
{
    char payload[256];
    const char *leader;
    uint64_t remaining;
    int written;

    if (room == NULL) {
        return PROTOCOL_ERROR;
    }
    leader = room->has_bid ? room->highest_bidder_id : NO_ROOM;
    remaining = room->timer_active
                    ? auction_room_remaining_ms(room, now)
                    : 0U;
    written = snprintf(payload, sizeof(payload),
                       "%s|%s|%s|%" PRId64 "|%s|%" PRIu64
                       "|%" PRId64 "|%" PRId64 "|%zu",
                       room->id, room->auction_id,
                       auction_status_text(room), room->current_price,
                       leader, remaining, room->starting_price,
                       room->min_increment, room->bid_count);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    return send_payload(socket_fd, MSG_AUCTION_STATE, request_id,
                        payload, (size_t)written);
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
                            struct participant **current_participant,
                            struct client_connection *clients,
                            size_t client_count)
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
    if (result == STATE_ALREADY_CONNECTED && participant != NULL) {
        size_t index;
        char event[160];

        /* A valid bearer token may replace a stale/half-open socket. Detach the
           old connection first so its later close cannot mark the new session
           disconnected. */
        for (index = 0; index < client_count; index++) {
            if (clients[index].socket_fd != socket_fd &&
                clients[index].participant == participant) {
                clients[index].participant = NULL;
                clients[index].output_failed = 1;
            }
        }
        participant_disconnect(participant);
        result = participant_reconnect(state, participant_id, token,
                                       &participant);
        (void)snprintf(event, sizeof(event),
                       "[RECONNECT TAKEOVER] %s replaced previous socket",
                       participant_id);
        logger_event(&server_logger, "INFO", event);
    }
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
    uint64_t now;

    if (current_participant == NULL) {
        return send_error(socket_fd, message->header.request_id,
                          "NOT_CONNECTED");
    }
    if (payload_to_text(message, name, sizeof(name), 0, 0) != 0) {
        return send_error(socket_fd, message->header.request_id,
                          "INVALID_ROOM_NAME");
    }

    if (monotonic_ms(&now) != 0 || now > UINT64_MAX - auction_duration_ms) {
        return send_error(socket_fd, message->header.request_id,
                          "INTERNAL_ERROR");
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

    if (!auction_room_timer_start(room, now, auction_duration_ms)) {
        return PROTOCOL_ERROR;
    }

    written = snprintf(payload, sizeof(payload), "OK|%s|%s", room->id,
                       room->name);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        return PROTOCOL_ERROR;
    }
    return send_text(socket_fd, MSG_CREATE_ROOM_RESULT,
                     message->header.request_id, payload);
}

static int handle_join_room(int socket_fd, const struct message *message,
                            struct auction_state *state,
                            struct participant *current_participant)
{
    char room_id[ROOM_ID_CAPACITY];
    struct auction_room *room;
    enum state_result result;

    if (current_participant == NULL) {
        return send_text(socket_fd, MSG_JOIN_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    }
    if (payload_to_text(message, room_id, sizeof(room_id), 0, 0) != 0 ||
        strlen(room_id) < 4U) {
        return send_text(socket_fd, MSG_JOIN_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    }

    result = auction_room_join(state, current_participant, room_id);
    switch (result) {
    case STATE_NOT_FOUND:
        return send_join_failure(socket_fd, message->header.request_id,
                                 current_participant, room_id,
                                 "ROOM_NOT_FOUND");
    case STATE_ALREADY_CONNECTED:
        return send_join_failure(socket_fd, message->header.request_id,
                                 current_participant, room_id,
                                 "ALREADY_IN_ROOM");
    case STATE_ROOM_ENDED:
        return send_join_failure(socket_fd, message->header.request_id,
                                 current_participant, room_id,
                                 "ROOM_ENDED");
    case STATE_ROOM_FULL:
        return send_join_failure(socket_fd, message->header.request_id,
                                 current_participant, room_id,
                                 "ROOM_FULL");
    case STATE_INVALID_ARGUMENT:
        return send_text(socket_fd, MSG_JOIN_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    case STATE_INTERNAL_ERROR:
        return send_text(socket_fd, MSG_JOIN_RESULT,
                         message->header.request_id, "FAIL|SERVER_ERROR");
    default:
        break;
    }

    if (result != STATE_OK) {
        return send_text(socket_fd, MSG_JOIN_RESULT,
                         message->header.request_id, "FAIL|SERVER_ERROR");
    }
    room = auction_room_find(state, room_id);
    if (room == NULL) {
        return PROTOCOL_ERROR;
    }
    {
        char joined_payload[ROOM_ID_CAPACITY + 3U];
        int written = snprintf(joined_payload, sizeof(joined_payload),
                               "OK|%s", room_id);
        if (written < 0 || (size_t)written >= sizeof(joined_payload)) {
            return PROTOCOL_ERROR;
        }
        if (send_text(socket_fd, MSG_JOIN_RESULT, message->header.request_id,
                      joined_payload) != PROTOCOL_OK) {
            return PROTOCOL_ERROR;
        }
    }
    {
        char event[128];
        (void)snprintf(event, sizeof(event), "[JOIN] %s joined %s",
                       current_participant->id, room_id);
        logger_event(&server_logger, "INFO", event);
    }
    return send_room_state(socket_fd, message->header.request_id, room);
}

static int handle_leave_room(int socket_fd, const struct message *message,
                             struct auction_state *state,
                             struct participant *current_participant)
{
    char room_id[ROOM_ID_CAPACITY];
    enum state_result result;

    if (current_participant == NULL) {
        return send_text(socket_fd, MSG_LEAVE_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    }
    if (payload_to_text(message, room_id, sizeof(room_id), 0, 0) != 0 ||
        strlen(room_id) < 4U) {
        return send_text(socket_fd, MSG_LEAVE_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    }

    result = auction_room_leave(state, current_participant, room_id);
    switch (result) {
    case STATE_NOT_IN_ROOM:
        {
            char event[128];
            (void)snprintf(event, sizeof(event),
                           "[LEAVE FAILED] %s NOT_IN_ROOM",
                           current_participant->id);
            logger_event(&server_logger, "INFO", event);
            return send_text(socket_fd, MSG_LEAVE_RESULT,
                             message->header.request_id,
                             "FAIL|NOT_IN_ROOM");
        }
    case STATE_ROOM_MISMATCH:
        {
            char event[128];
            (void)snprintf(event, sizeof(event),
                           "[LEAVE FAILED] %s ROOM_MISMATCH %s",
                           current_participant->id, room_id);
            logger_event(&server_logger, "INFO", event);
            return send_text(socket_fd, MSG_LEAVE_RESULT,
                             message->header.request_id,
                             "FAIL|ROOM_MISMATCH");
        }
    case STATE_INVALID_ARGUMENT:
        return send_text(socket_fd, MSG_LEAVE_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    case STATE_INTERNAL_ERROR:
        return send_text(socket_fd, MSG_LEAVE_RESULT,
                         message->header.request_id, "FAIL|SERVER_ERROR");
    default:
        break;
    }
    if (result != STATE_OK) {
        return send_text(socket_fd, MSG_LEAVE_RESULT,
                         message->header.request_id, "FAIL|SERVER_ERROR");
    }

    {
        char left_payload[ROOM_ID_CAPACITY + 3U];
        const struct auction_room *room;
        int written = snprintf(left_payload, sizeof(left_payload),
                               "OK|%s", room_id);
        if (written < 0 || (size_t)written >= sizeof(left_payload)) {
            return PROTOCOL_ERROR;
        }
        if (send_text(socket_fd, MSG_LEAVE_RESULT, message->header.request_id,
                      left_payload) != PROTOCOL_OK) {
            return PROTOCOL_ERROR;
        }
        {
            char event[128];
            (void)snprintf(event, sizeof(event), "[LEAVE] %s left %s",
                           current_participant->id, room_id);
            logger_event(&server_logger, "INFO", event);
        }
        room = auction_room_find_const(state, room_id);
        if (room != NULL) {
            return send_room_state(socket_fd, message->header.request_id, room);
        }
    }
    return PROTOCOL_OK;
}

static int participant_in_room(const struct auction_room *room,
                               const char *participant_id)
{
    size_t index;

    if (room == NULL || participant_id == NULL) {
        return 0;
    }
    for (index = 0; index < room->participant_count; index++) {
        if (strcmp(room->participant_ids[index], participant_id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void broadcast_room_event(const struct auction_room *room, uint8_t type,
                                 const char *payload,
                                 struct client_connection *clients,
                                 size_t client_count)
{
    size_t index;

    for (index = 0; index < client_count; index++) {
        struct client_connection *client = &clients[index];
        if (client->participant != NULL && client->participant->connected &&
            !client->input_closed &&
            strcmp(client->participant->current_room_id, room->id) == 0 &&
            participant_in_room(room, client->participant->id)) {
            (void)send_text(client->socket_fd, type, 0, payload);
        }
    }
}

static void process_room_timer(struct auction_room *room, uint64_t now,
                               struct client_connection *clients,
                               size_t client_count)
{
    char payload[128];
    uint64_t remaining;

    if (!room->timer_active) {
        return;
    }
    if (auction_room_expire(room, now)) {
        if (room->has_bid) {
            (void)snprintf(payload, sizeof(payload), "%s|%s|SOLD|%s|%" PRId64,
                           room->id, room->auction_id,
                           room->highest_bidder_id, room->current_price);
        } else {
            (void)snprintf(payload, sizeof(payload), "%s|%s|NO_BID",
                           room->id, room->auction_id);
        }
        broadcast_room_event(room, MSG_AUCTION_RESULT, payload,
                             clients, client_count);
        return;
    }
    remaining = auction_room_remaining_ms(room, now);
    if (now >= room->next_update_ms) {
        (void)snprintf(payload, sizeof(payload), "%s|%s|%" PRIu64,
                       room->id, room->auction_id, remaining);
        broadcast_room_event(room, MSG_TIME_UPDATE, payload, clients, client_count);
        room->next_update_ms = now > UINT64_MAX - 1000U ? UINT64_MAX : now + 1000U;
    }
    if (!room->warning_sent && remaining <= 10000U) {
        (void)snprintf(payload, sizeof(payload), "%s|%s|%" PRIu64,
                       room->id, room->auction_id, (remaining + 999U) / 1000U);
        broadcast_room_event(room, MSG_TIME_WARNING, payload, clients, client_count);
        room->warning_sent = true;
    }
}

static void process_timers(struct auction_state *state, uint64_t now,
                           struct client_connection *clients,
                           size_t client_count)
{
    size_t index;
    for (index = 0; index < state->room_count; index++) {
        process_room_timer(&state->rooms[index], now, clients, client_count);
    }
}

static void send_timer_snapshot(struct client_connection *client,
                                struct auction_state *state, uint64_t now)
{
    struct auction_room *room;
    char payload[128];
    uint64_t remaining;

    if (client->participant == NULL ||
        (room = auction_room_find(state, client->participant->current_room_id)) == NULL ||
        !participant_in_room(room, client->participant->id)) {
        return;
    }
    if (room->ended) {
        if (room->has_bid) {
            (void)snprintf(payload, sizeof(payload), "%s|%s|SOLD|%s|%" PRId64,
                           room->id, room->auction_id, room->highest_bidder_id,
                           room->current_price);
        } else {
            (void)snprintf(payload, sizeof(payload), "%s|%s|NO_BID",
                           room->id, room->auction_id);
        }
        (void)send_text(client->socket_fd, MSG_AUCTION_RESULT, 0, payload);
    } else if (room->timer_active) {
        remaining = auction_room_remaining_ms(room, now);
        (void)snprintf(payload, sizeof(payload), "%s|%s|%" PRIu64,
                       room->id, room->auction_id, remaining);
        (void)send_text(client->socket_fd, MSG_TIME_UPDATE, 0, payload);
        if (remaining <= 10000U) {
            (void)snprintf(payload, sizeof(payload), "%s|%s|%" PRIu64,
                           room->id, room->auction_id, (remaining + 999U) / 1000U);
            (void)send_text(client->socket_fd, MSG_TIME_WARNING, 0, payload);
        }
    }
}

static void broadcast_bid_update(struct auction_room *room,
                                 const struct auction_state *state,
                                 struct client_connection *clients,
                                 size_t client_count)
{
    char payload[PROTOCOL_MAX_PAYLOAD];
    int written;
    size_t index;

    written = snprintf(payload, sizeof(payload), "%s|%s|%" PRId64 "|%s",
                       room->id, room->auction_id, room->current_price,
                       room->highest_bidder_id);
    if (written < 0 || (size_t)written >= sizeof(payload)) {
        logger_event(&server_logger, "ERROR",
                     "[BID UPDATE] failed to format broadcast");
        return;
    }
    for (index = 0; index < client_count; index++) {
        struct client_connection *client = &clients[index];

        if (client->socket_fd < 0 || client->participant == NULL ||
            !client->participant->connected ||
            strcmp(client->participant->current_room_id, room->id) != 0 ||
            !participant_in_room(room, client->participant->id)) {
            continue;
        }
        if (send_payload(client->socket_fd, MSG_BID_UPDATE, 0,
                         payload, (size_t)written) != PROTOCOL_OK) {
            logger_event(&server_logger, "ERROR",
                         "[BID UPDATE] send failed");
        }
    }

    (void)state;
}

static int handle_place_bid(int socket_fd, const struct message *message,
                            struct auction_state *state,
                            struct participant *participant,
                            struct client_connection *clients,
                            size_t client_count)
{
    char room_id[ROOM_ID_CAPACITY];
    char auction_id[AUCTION_ID_CAPACITY];
    char amount_text[64];
    char *parts[3];
    char *separator;
    char text[PROTOCOL_MAX_PAYLOAD];
    struct auction_room *room;
    int64_t amount;
    int64_t minimum;
    enum auction_bid_result bid_result;
    char *end;
    char *digit;
    uint64_t now;

    if (participant == NULL || !participant->connected) {
        logger_event(&server_logger, "INFO",
                     "[BID REJECTED] INVALID_PARTICIPANT");
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_PARTICIPANT");
    }
    if (payload_to_text(message, text, sizeof(text), 0, 1) != 0) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_AMOUNT");
    }

    parts[0] = text;
    separator = strchr(parts[0], '|');
    if (separator == NULL) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_AMOUNT");
    }
    *separator = '\0';
    parts[1] = separator + 1;
    separator = strchr(parts[1], '|');
    if (separator == NULL) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_AMOUNT");
    }
    *separator = '\0';
    parts[2] = separator + 1;
    if (strchr(parts[2], '|') != NULL || parts[0][0] == '\0' ||
        parts[1][0] == '\0' || parts[2][0] == '\0' ||
        strlen(parts[0]) >= sizeof(room_id) ||
        strlen(parts[1]) >= sizeof(auction_id) ||
        strlen(parts[2]) >= sizeof(amount_text)) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_AMOUNT");
    }
    for (digit = parts[2]; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return send_text(socket_fd, MSG_BID_RESULT,
                             message->header.request_id,
                             "FAIL|INVALID_AMOUNT");
        }
    }
    memcpy(room_id, parts[0], strlen(parts[0]) + 1U);
    memcpy(auction_id, parts[1], strlen(parts[1]) + 1U);
    memcpy(amount_text, parts[2], strlen(parts[2]) + 1U);

    room = auction_room_find(state, room_id);
    if (room == NULL) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|ROOM_NOT_FOUND");
    }
    if (strcmp(participant->current_room_id, NO_ROOM) == 0 ||
        participant->current_room_id[0] == '\0') {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|NOT_IN_ROOM");
    }
    if (strcmp(participant->current_room_id, room_id) != 0) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|ROOM_MISMATCH");
    }
    if (!participant_in_room(room, participant->id)) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|NOT_IN_ROOM");
    }
    if (strcmp(room->auction_id, auction_id) != 0) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|AUCTION_NOT_FOUND");
    }

    errno = 0;
    amount = strtoll(amount_text, &end, 10);
    if (errno == ERANGE || end == amount_text || *end != '\0' ||
        amount_text[0] == '-' || amount_text[0] == '+' ||
        amount <= 0) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_AMOUNT");
    }

    if (monotonic_ms(&now) != 0) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id, "FAIL|SERVER_ERROR");
    }
    process_room_timer(room, now, clients, client_count);
    bid_result = auction_place_bid(room, participant->id, amount, &minimum);
    if (bid_result == AUCTION_BID_PRICE_TOO_LOW) {
        char payload[128];
        char event[160];
        (void)snprintf(payload, sizeof(payload),
                       "FAIL|PRICE_TOO_LOW|%" PRId64, minimum);
        (void)snprintf(event, sizeof(event),
                       "[BID REJECTED] %s PRICE_TOO_LOW minimum=%" PRId64,
                       participant->id, minimum);
        logger_event(&server_logger, "INFO", event);
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id, payload);
    }
    if (bid_result == AUCTION_BID_INVALID_AMOUNT) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|INVALID_AMOUNT");
    }
    if (bid_result == AUCTION_BID_NOT_ACTIVE) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|AUCTION_NOT_ACTIVE");
    }
    if (bid_result == AUCTION_BID_HISTORY_FULL) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|SERVER_ERROR");
    }
    if (bid_result != AUCTION_BID_OK) {
        return send_text(socket_fd, MSG_BID_RESULT,
                         message->header.request_id,
                         "FAIL|SERVER_ERROR");
    }

    {
        char payload[128];
        char event[160];
        int response_result;
        (void)snprintf(payload, sizeof(payload), "OK|%s|%" PRId64,
                       room_id, amount);
        response_result = send_text(socket_fd, MSG_BID_RESULT,
                                    message->header.request_id, payload);
        (void)snprintf(event, sizeof(event),
                       "[BID] %s bid %" PRId64 " in %s accepted",
                       participant->id, amount, room_id);
        logger_event(&server_logger, "INFO", event);
        broadcast_bid_update(room, state, clients, client_count);
        (void)snprintf(event, sizeof(event),
                       "[BID UPDATE] %s new_price=%" PRId64 " leader=%s",
                       room_id, amount, participant->id);
        logger_event(&server_logger, "INFO", event);
        if (response_result != PROTOCOL_OK) {
            return PROTOCOL_ERROR;
        }
    }
    return PROTOCOL_OK;
}

static int handle_place_bid(int socket_fd, const struct message *message,
                            struct auction_state *state,
                            struct participant *participant,
                            struct client_connection *clients,
                            size_t client_count);

static int process_message(struct client_connection *client,
                           const struct message *message,
                           struct auction_state *state,
                           struct client_connection *clients,
                           size_t client_count)
{
    int response_result;
    uint64_t now;

    if (monotonic_ms(&now) != 0) {
        return PROTOCOL_ERROR;
    }
    process_timers(state, now, clients, client_count);

    logger_message(&server_logger, "RECV", &message->header, message->payload);
    switch (message->header.type) {
    case MSG_CONNECT:
        response_result = handle_connect(client->socket_fd, message, state,
                                         &client->participant);
        break;
    case MSG_RECONNECT:
        response_result = handle_reconnect(client->socket_fd, message, state,
                                           &client->participant,
                                           clients, client_count);
        if (response_result == PROTOCOL_OK && client->participant != NULL) {
            send_timer_snapshot(client, state, now);
        }
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
    case MSG_JOIN_ROOM:
        response_result = handle_join_room(client->socket_fd, message, state,
                                           client->participant);
        if (response_result == PROTOCOL_OK && client->participant != NULL &&
            message->header.payload_length == strlen(client->participant->current_room_id) &&
            memcmp(message->payload, client->participant->current_room_id,
                   message->header.payload_length) == 0) {
            send_timer_snapshot(client, state, now);
        }
        break;
    case MSG_LEAVE_ROOM:
        response_result = handle_leave_room(client->socket_fd, message, state,
                                            client->participant);
        break;
    case MSG_GET_ROOM_STATE: {
        char room_id[ROOM_ID_CAPACITY];
        const struct auction_room *room;

        if (client->participant == NULL) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "INVALID_PARTICIPANT");
        } else if (payload_to_text(message, room_id, sizeof(room_id),
                                   0, 0) != 0) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "INVALID_ROOM_ID");
        } else {
            room = auction_room_find_const(state, room_id);
            if (room == NULL) {
                response_result = send_error(client->socket_fd,
                                             message->header.request_id,
                                             "ROOM_NOT_FOUND");
            } else {
                response_result = send_room_state(client->socket_fd,
                                                  message->header.request_id,
                                                  room);
            }
        }
        break;
    }
    case MSG_GET_SESSION_STATE:
        if (client->participant == NULL) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "NOT_CONNECTED");
        } else if (message->header.payload_length != 0U) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "UNEXPECTED_PAYLOAD");
        } else {
            response_result = send_session_state(client->socket_fd,
                                                 message->header.request_id,
                                                 client->participant);
        }
        break;
    case MSG_GET_AUCTION_STATE: {
        char room_id[ROOM_ID_CAPACITY];
        const struct auction_room *room;

        if (client->participant == NULL) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "NOT_CONNECTED");
        } else if (payload_to_text(message, room_id, sizeof(room_id),
                                   0, 0) != 0) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "INVALID_ROOM_ID");
        } else if ((room = auction_room_find_const(state, room_id)) == NULL) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "ROOM_NOT_FOUND");
        } else if (strcmp(client->participant->current_room_id,
                          room_id) != 0) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         strcmp(client->participant->current_room_id,
                                                NO_ROOM) == 0
                                             ? "NOT_IN_ROOM"
                                             : "ROOM_MISMATCH");
        } else if (!participant_in_room(room, client->participant->id)) {
            response_result = send_error(client->socket_fd,
                                         message->header.request_id,
                                         "NOT_IN_ROOM");
        } else {
            response_result = send_auction_state(client->socket_fd,
                                                 message->header.request_id,
                                                 room, now);
        }
        break;
    }
    case MSG_PLACE_BID:
        response_result = handle_place_bid(client->socket_fd, message, state,
                                           client->participant,
                                           clients, client_count);
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
                         struct auction_state *state,
                         struct client_connection *clients,
                         size_t client_count)
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
        if (process_message(client, &message, state,
                            clients, client_count) != PROTOCOL_OK) {
            return -1;
        }
    }
    return 0;
}

static int read_client(struct client_connection *client,
                        struct auction_state *state,
                        struct client_connection *clients,
                        size_t client_count)
{
    unsigned int reads = 0;
    while (reads++ < 16U) {
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
            int was_empty = client->input_length == 0U;

            client->input_length += (size_t)received;
            if (was_empty && monotonic_ms(&client->input_started_ms) != 0) {
                return -1;
            }
            if (process_input(client, state, clients, client_count) != 0) {
                return -1;
            }
            if (client->input_length == 0U) {
                client->input_started_ms = 0U;
            }
            continue;
        }
        if (received == 0) {
            if (client->input_length != 0) {
                logger_event(&server_logger, "ERROR",
                             "client disconnected during frame");
                return -1;
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
    return 0;
}

static int write_client(struct client_connection *client)
{
    while (client->output_offset < client->output_length) {
        ssize_t sent = send(client->socket_fd,
                            client->output + client->output_offset,
                            client->output_length - client->output_offset,
                            MSG_DONTWAIT);
        if (sent > 0) {
            client->output_offset += (size_t)sent;
            if (client->output_offset < client->output_length &&
                monotonic_ms(&client->output_progress_ms) != 0) {
                return -1;
            }
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return -1;
        }
    }
    client->output_offset = 0;
    client->output_progress_ms = 0;
    client->output_length = 0;
    return 0;
}

static void close_client(struct client_connection *client)
{
    if (client->participant != NULL) {
        char event[160];
        (void)snprintf(event, sizeof(event),
                       "[DISCONNECT] %s disconnected, membership preserved %s",
                       client->participant->id,
                       client->participant->current_room_id);
        logger_event(&server_logger, "INFO", event);
    }
    if (client->socket_fd >= 0) {
        close(client->socket_fd);
    }
    participant_disconnect(client->participant);
    client->socket_fd = -1;
    client->input_length = 0;
    client->input_started_ms = 0;
    client->connected_ms = 0;
    client->output_length = 0;
    client->output_offset = 0;
    client->output_failed = 0;
    client->input_closed = 0;
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

static int parse_duration(const char *value, uint64_t *duration)
{
    const char *digit;
    char *end;
    unsigned long long parsed;

    if (value == NULL || *value == '\0') {
        return -1;
    }
    for (digit = value; *digit != '\0'; digit++) {
        if (*digit < '0' || *digit > '9') {
            return -1;
        }
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno == ERANGE || *end != '\0' ||
        parsed < AUCTION_MIN_DURATION_MS || parsed > AUCTION_MAX_DURATION_MS) {
        return -1;
    }
    *duration = (uint64_t)parsed;
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

    if (argc > 3 || (argc >= 2 && parse_port(argv[1], &server_port) != 0) ||
        (argc == 3 && parse_duration(argv[2], &auction_duration_ms) != 0)) {
        fprintf(stderr, "Su dung: %s [port [duration_ms]]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (logger_open(&server_logger, "auction.log", "SERVER") != 0) {
        perror("auction.log");
        return EXIT_FAILURE;
    }
    signal(SIGPIPE, SIG_IGN);
    auction_state_init(&state);
    active_clients = clients;
    active_client_count = &client_count;

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

    {
        uint64_t now;
        size_t index;
        if (monotonic_ms(&now) != 0) {
            perror("clock_gettime");
            close(server_fd);
            logger_close(&server_logger);
            return EXIT_FAILURE;
        }
        for (index = 0; index < state.room_count; index++) {
            if (!auction_room_timer_start(&state.rooms[index], now,
                                          auction_duration_ms)) {
                fprintf(stderr, "Cannot start auction timer\n");
                close(server_fd);
                logger_close(&server_logger);
                return EXIT_FAILURE;
            }
        }
    }
    printf("Server dang lang nghe tai port %u...\n", (unsigned int)server_port);
    fflush(stdout);
    for (;;) {
        size_t index;
        int ready;
        uint64_t now;

        if (monotonic_ms(&now) != 0) {
            perror("clock_gettime");
            break;
        }
        process_timers(&state, now, clients, client_count);

        poll_fds[0].fd = server_fd;
        poll_fds[0].events = POLLIN;
        poll_fds[0].revents = 0;
        for (index = 0; index < client_count; index++) {
            poll_fds[index + 1U].fd = clients[index].socket_fd;
            poll_fds[index + 1U].events = clients[index].input_closed ? 0 : POLLIN;
            if (clients[index].output_offset < clients[index].output_length) {
                poll_fds[index + 1U].events |= POLLOUT;
            }
            poll_fds[index + 1U].revents = 0;
        }

        ready = poll(poll_fds, client_count + 1U, 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("poll");
            break;
        }

        for (index = client_count; index > 0; index--) {
            size_t client_index = index - 1U;
            struct client_connection *client = &clients[client_index];
            short events = poll_fds[client_index + 1U].revents;
            int drop = client->output_failed || (events & (POLLERR | POLLNVAL));

            if (!drop && client->participant == NULL &&
                now >= client->connected_ms &&
                now - client->connected_ms >= AUTH_TIMEOUT_MS) {
                logger_event(&server_logger, "WARN",
                             "unauthenticated client timed out; closing");
                drop = 1;
            }
            if (!drop && client->input_length != 0U &&
                now >= client->input_started_ms &&
                now - client->input_started_ms >= INPUT_STALL_TIMEOUT_MS) {
                logger_event(&server_logger, "WARN",
                             "incomplete client frame timed out; closing");
                drop = 1;
            }

            if (!drop && client->output_offset < client->output_length &&
                monotonic_ms(&now) == 0 && now >= client->output_progress_ms &&
                now - client->output_progress_ms >= OUTPUT_STALL_TIMEOUT_MS) {
                logger_event(&server_logger, "WARN", "client output stalled; closing");
                drop = 1;
            }

            if (!drop && !client->input_closed && (events & (POLLIN | POLLHUP))) {
                int result = read_client(client, &state, clients, client_count);
                if (result < 0) {
                    drop = 1;
                } else if (result > 0) {
                    client->input_closed = 1;
                }
            }
            if (!drop && client->output_offset < client->output_length &&
                (events & (POLLOUT | POLLHUP))) {
                drop = write_client(client) != 0;
            }
            if (client->output_failed ||
                (client->input_closed && client->output_offset == client->output_length)) {
                drop = 1;
            }
            if (drop) {
                close_client(client);
                clients[client_index] = clients[client_count - 1U];
                client_count--;
            }
        }

        if (poll_fds[0].revents & POLLIN) {
            unsigned int accepted;
            /* Bound the work per cycle so a connection flood cannot starve timers. */
            for (accepted = 0; accepted < 16U; accepted++) {
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
                memset(&clients[client_count], 0, sizeof(clients[client_count]));
                clients[client_count].socket_fd = client_fd;
                if (monotonic_ms(&clients[client_count].connected_ms) != 0) {
                    close(client_fd);
                    continue;
                }
                client_count++;
                printf("Client da ket noi (%zu/%u).\n", client_count,
                       MAX_CLIENTS);
                fflush(stdout);
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

#include "auction_state.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int is_safe_text(const char *text, size_t maximum_length)
{
    const unsigned char *character = (const unsigned char *)text;
    size_t length;

    if (text == NULL) {
        return 0;
    }

    length = strlen(text);
    if (length == 0 || length >= maximum_length) {
        return 0;
    }

    while (*character != '\0') {
        if (*character < 0x20U || *character > 0x7eU ||
            *character == (unsigned char)'|') {
            return 0;
        }
        character++;
    }
    return 1;
}

static int read_random_bytes(uint8_t *destination, size_t length)
{
    int random_fd;
    size_t received = 0;

    random_fd = open("/dev/urandom", O_RDONLY);
    if (random_fd < 0) {
        return -1;
    }
    while (received < length) {
        ssize_t result = read(random_fd, destination + received, length - received);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(random_fd);
            return -1;
        }
        if (result == 0) {
            (void)close(random_fd);
            return -1;
        }
        received += (size_t)result;
    }
    (void)close(random_fd);
    return 0;
}

static int make_token(char token[SESSION_TOKEN_CAPACITY])
{
    static const char hex_digits[] = "0123456789abcdef";
    uint8_t bytes[16];
    size_t index;

    if (SESSION_TOKEN_CAPACITY != (sizeof(bytes) * 2U) + 1U ||
        read_random_bytes(bytes, sizeof(bytes)) != 0) {
        return -1;
    }
    for (index = 0; index < sizeof(bytes); index++) {
        token[index * 2U] = hex_digits[bytes[index] >> 4];
        token[(index * 2U) + 1U] = hex_digits[bytes[index] & 0x0fU];
    }
    token[sizeof(bytes) * 2U] = '\0';
    return 0;
}

static struct participant *find_participant(struct auction_state *state,
                                            const char *participant_id)
{
    size_t index;

    for (index = 0; index < state->participant_count; index++) {
        if (strcmp(state->participants[index].id, participant_id) == 0) {
            return &state->participants[index];
        }
    }
    return NULL;
}

static int room_name_exists(const struct auction_state *state, const char *name)
{
    size_t index;

    for (index = 0; index < state->room_count; index++) {
        if (strcmp(state->rooms[index].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static int is_connected_participant(const struct auction_state *state,
                                    const struct participant *participant)
{
    size_t index;

    for (index = 0; index < state->participant_count; index++) {
        if (&state->participants[index] == participant) {
            return participant->connected;
        }
    }
    return 0;
}

static void add_default_room(struct auction_state *state, const char *name)
{
    struct auction_room *room = &state->rooms[state->room_count++];

    (void)snprintf(room->id, sizeof(room->id), "R%03" PRIu32,
                   state->next_room_number++);
    (void)snprintf(room->name, sizeof(room->name), "%s", name);
    (void)snprintf(room->owner_id, sizeof(room->owner_id), "%s", "SYSTEM");
}

void auction_state_init(struct auction_state *state)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->next_participant_number = 1;
    state->next_room_number = 1;

    add_default_room(state, "Demo Room");
    add_default_room(state, "Practice Room");
}

enum state_result participant_connect(struct auction_state *state,
                                      const char *name,
                                      struct participant **participant)
{
    struct participant *new_participant;

    if (state == NULL || participant == NULL ||
        (name != NULL && name[0] != '\0' &&
         !is_safe_text(name, PARTICIPANT_NAME_CAPACITY))) {
        return STATE_INVALID_ARGUMENT;
    }
    if (state->participant_count == MAX_PARTICIPANTS) {
        return STATE_CAPACITY_REACHED;
    }

    new_participant = &state->participants[state->participant_count];
    (void)snprintf(new_participant->id, sizeof(new_participant->id),
                   "P%03" PRIu32, state->next_participant_number);
    if (name == NULL || name[0] == '\0') {
        (void)snprintf(new_participant->name, sizeof(new_participant->name),
                       "Participant %" PRIu32,
                       state->next_participant_number);
    } else {
        (void)snprintf(new_participant->name, sizeof(new_participant->name),
                       "%s", name);
    }
    if (make_token(new_participant->token) != 0) {
        memset(new_participant, 0, sizeof(*new_participant));
        return STATE_INTERNAL_ERROR;
    }
    new_participant->connected = true;
    state->participant_count++;
    state->next_participant_number++;
    *participant = new_participant;
    return STATE_OK;
}

enum state_result participant_reconnect(struct auction_state *state,
                                        const char *participant_id,
                                        const char *token,
                                        struct participant **participant)
{
    struct participant *existing;

    if (state == NULL || participant_id == NULL || token == NULL ||
        participant == NULL || !is_safe_text(participant_id,
                                             PARTICIPANT_ID_CAPACITY) ||
        !is_safe_text(token, SESSION_TOKEN_CAPACITY)) {
        return STATE_INVALID_ARGUMENT;
    }

    existing = find_participant(state, participant_id);
    if (existing == NULL) {
        return STATE_NOT_FOUND;
    }
    if (strcmp(existing->token, token) != 0) {
        return STATE_TOKEN_MISMATCH;
    }
    if (existing->connected) {
        return STATE_ALREADY_CONNECTED;
    }
    existing->connected = true;
    *participant = existing;
    return STATE_OK;
}

void participant_disconnect(struct participant *participant)
{
    if (participant != NULL) {
        participant->connected = false;
    }
}

enum state_result auction_room_create(struct auction_state *state,
                                      const struct participant *owner,
                                      const char *name,
                                      struct auction_room **room)
{
    struct auction_room *new_room;

    if (state == NULL || owner == NULL || room == NULL ||
        !is_connected_participant(state, owner) ||
        !is_safe_text(name, ROOM_NAME_CAPACITY)) {
        return STATE_INVALID_ARGUMENT;
    }
    if (state->room_count == MAX_AUCTION_ROOMS) {
        return STATE_CAPACITY_REACHED;
    }
    if (room_name_exists(state, name)) {
        return STATE_DUPLICATE_NAME;
    }

    new_room = &state->rooms[state->room_count++];
    (void)snprintf(new_room->id, sizeof(new_room->id), "R%03" PRIu32,
                   state->next_room_number++);
    (void)snprintf(new_room->name, sizeof(new_room->name), "%s", name);
    (void)snprintf(new_room->owner_id, sizeof(new_room->owner_id), "%s",
                   owner->id);
    *room = new_room;
    return STATE_OK;
}

const struct auction_room *auction_room_at(const struct auction_state *state,
                                           size_t index)
{
    if (state == NULL || index >= state->room_count) {
        return NULL;
    }
    return &state->rooms[index];
}

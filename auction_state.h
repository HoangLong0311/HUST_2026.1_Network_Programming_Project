#ifndef AUCTION_STATE_H
#define AUCTION_STATE_H

#include "common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_PARTICIPANTS 64U
#define MAX_AUCTION_ROOMS 32U
#define MAX_PARTICIPANTS_PER_ROOM 32U
#define NO_ROOM "-1"

#define PARTICIPANT_ID_CAPACITY 8U
#define SESSION_TOKEN_CAPACITY 33U
#define PARTICIPANT_NAME_CAPACITY 49U
#define ROOM_ID_CAPACITY 8U
#define ROOM_NAME_CAPACITY 65U

struct participant {
    char id[PARTICIPANT_ID_CAPACITY];
    char token[SESSION_TOKEN_CAPACITY];
    char name[PARTICIPANT_NAME_CAPACITY];
    bool connected;
    char current_room_id[ROOM_ID_CAPACITY];
};

struct auction_room {
    char id[ROOM_ID_CAPACITY];
    char name[ROOM_NAME_CAPACITY];
    char owner_id[PARTICIPANT_ID_CAPACITY];
    char participant_ids[MAX_PARTICIPANTS_PER_ROOM][PARTICIPANT_ID_CAPACITY];
    size_t participant_count;
    bool ended;
};

struct auction_state {
    struct participant participants[MAX_PARTICIPANTS];
    struct auction_room rooms[MAX_AUCTION_ROOMS];
    size_t participant_count;
    size_t room_count;
    uint32_t next_participant_number;
    uint32_t next_room_number;
};

enum state_result {
    STATE_OK = 0,
    STATE_INVALID_ARGUMENT,
    STATE_CAPACITY_REACHED,
    STATE_NOT_FOUND,
    STATE_TOKEN_MISMATCH,
    STATE_ALREADY_CONNECTED,
    STATE_DUPLICATE_NAME,
    STATE_NOT_IN_ROOM,
    STATE_ROOM_MISMATCH,
    STATE_ROOM_ENDED,
    STATE_ROOM_FULL,
    STATE_INTERNAL_ERROR
};

void auction_state_init(struct auction_state *state);

enum state_result participant_connect(struct auction_state *state,
                                      const char *name,
                                      struct participant **participant);
enum state_result participant_reconnect(struct auction_state *state,
                                        const char *participant_id,
                                        const char *token,
                                        struct participant **participant);
void participant_disconnect(struct participant *participant);

enum state_result auction_room_create(struct auction_state *state,
                                      const struct participant *owner,
                                      const char *name,
                                      struct auction_room **room);

const struct auction_room *auction_room_at(const struct auction_state *state,
                                           size_t index);

struct auction_room *auction_room_find(struct auction_state *state,
                                       const char *room_id);
const struct auction_room *auction_room_find_const(
    const struct auction_state *state, const char *room_id);
enum state_result auction_room_join(struct auction_state *state,
                                    struct participant *participant,
                                    const char *room_id);
enum state_result auction_room_leave(struct auction_state *state,
                                     struct participant *participant,
                                     const char *room_id);

#endif

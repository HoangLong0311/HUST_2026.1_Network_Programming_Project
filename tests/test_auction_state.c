#include "auction_state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_connect_disconnect_and_reconnect(void)
{
    struct auction_state state;
    struct participant *alice;
    struct participant *bob;
    struct participant *reconnected;
    char alice_id[PARTICIPANT_ID_CAPACITY];
    char alice_token[SESSION_TOKEN_CAPACITY];

    auction_state_init(&state);

    assert(participant_connect(&state, "Alice|Mallory", &alice) ==
           STATE_INVALID_ARGUMENT);
    assert(participant_connect(&state, "Alice", &alice) == STATE_OK);
    assert(strcmp(alice->id, "P001") == 0);
    assert(strcmp(alice->name, "Alice") == 0);
    assert(alice->connected);
    assert(strlen(alice->token) == SESSION_TOKEN_CAPACITY - 1U);

    assert(participant_connect(&state, "Bob", &bob) == STATE_OK);
    assert(strcmp(bob->id, "P002") == 0);
    assert(strcmp(alice->token, bob->token) != 0);

    (void)snprintf(alice_id, sizeof(alice_id), "%s", alice->id);
    (void)snprintf(alice_token, sizeof(alice_token), "%s", alice->token);

    assert(participant_reconnect(&state, alice_id, alice_token, &reconnected) ==
           STATE_ALREADY_CONNECTED);

    participant_disconnect(alice);
    assert(!alice->connected);
    assert(participant_reconnect(&state, "P999", alice_token, &reconnected) ==
           STATE_NOT_FOUND);
    assert(participant_reconnect(&state, alice_id, "bad-token", &reconnected) ==
           STATE_TOKEN_MISMATCH);
    assert(participant_reconnect(&state, alice_id, alice_token, &reconnected) ==
           STATE_OK);
    assert(reconnected == alice);
    assert(reconnected->connected);
    assert(strcmp(reconnected->name, "Alice") == 0);
    assert(strcmp(reconnected->token, alice_token) == 0);
}

static void test_multiple_rooms_are_managed_in_memory(void)
{
    struct auction_state state;
    struct participant *owner;
    struct auction_room *room;
    const struct auction_room *first_room;

    auction_state_init(&state);
    assert(state.room_count == 2U);
    first_room = auction_room_at(&state, 0U);
    assert(first_room != NULL);
    assert(strcmp(first_room->id, "R001") == 0);

    assert(participant_connect(&state, "Room owner", &owner) == STATE_OK);
    assert(auction_room_create(&state, owner, "Sunday Auction", &room) ==
           STATE_OK);
    assert(state.room_count == 3U);
    assert(strcmp(room->id, "R003") == 0);
    assert(strcmp(room->name, "Sunday Auction") == 0);
    assert(strcmp(room->owner_id, owner->id) == 0);

    assert(auction_room_create(&state, owner, "Sunday Auction", &room) ==
           STATE_DUPLICATE_NAME);
    assert(auction_room_create(&state, owner, "", &room) ==
           STATE_INVALID_ARGUMENT);

    participant_disconnect(owner);
    assert(auction_room_create(&state, owner, "Offline owner room", &room) ==
           STATE_INVALID_ARGUMENT);
}

int main(void)
{
    test_connect_disconnect_and_reconnect();
    test_multiple_rooms_are_managed_in_memory();
    puts("auction state tests passed");
    return 0;
}

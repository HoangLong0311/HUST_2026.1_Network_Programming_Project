#include "auction_state.h"
#include "auction.h"

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

static void test_join_leave_membership(void)
{
    struct auction_state state;
    struct participant *alice;
    struct participant *bob;
    struct participant *carol;
    struct auction_room *room1;
    struct auction_room *room2;

    auction_state_init(&state);
    assert(participant_connect(&state, "Alice", &alice) == STATE_OK);
    assert(participant_connect(&state, "Bob", &bob) == STATE_OK);
    room1 = auction_room_find(&state, "R001");
    room2 = auction_room_find(&state, "R002");
    assert(room1 != NULL && room2 != NULL);

    assert(strcmp(alice->current_room_id, NO_ROOM) == 0);
    assert(auction_room_join(&state, alice, "R001") == STATE_OK);
    assert(room1->participant_count == 1U);
    assert(strcmp(room1->participant_ids[0], "P001") == 0);
    assert(strcmp(alice->current_room_id, "R001") == 0);

    assert(auction_room_join(&state, alice, "R002") ==
           STATE_ALREADY_CONNECTED);
    assert(strcmp(alice->current_room_id, "R001") == 0);
    assert(auction_room_join(&state, bob, "R001") == STATE_OK);
    assert(room1->participant_count == 2U);

    assert(auction_room_join(&state, alice, "R999") == STATE_NOT_FOUND);
    assert(participant_connect(&state, "Carol", &carol) == STATE_OK);
    room2->ended = true;
    assert(auction_room_join(&state, carol, "R002") == STATE_ROOM_ENDED);

    assert(auction_room_leave(&state, alice, "R002") ==
           STATE_ROOM_MISMATCH);
    assert(strcmp(alice->current_room_id, "R001") == 0);
    assert(auction_room_leave(&state, alice, "R001") == STATE_OK);
    assert(room1->participant_count == 1U);
    assert(strcmp(alice->current_room_id, NO_ROOM) == 0);
    assert(auction_room_leave(&state, alice, "R001") == STATE_NOT_IN_ROOM);

    participant_disconnect(bob);
    assert(room1->participant_count == 1U);
    assert(strcmp(bob->current_room_id, "R001") == 0);

    room1->participant_count = MAX_PARTICIPANTS_PER_ROOM;
    assert(auction_room_join(&state, alice, "R001") == STATE_ROOM_FULL);
}

static void test_auction_bids(void)
{
    struct auction_state state;
    struct participant *alice;
    struct participant *bob;
    struct auction_room *room;
    int64_t minimum;

    auction_state_init(&state);
    assert(participant_connect(&state, "Alice", &alice) == STATE_OK);
    assert(participant_connect(&state, "Bob", &bob) == STATE_OK);
    room = auction_room_find(&state, "R001");
    assert(room != NULL);
    assert(auction_room_join(&state, alice, "R001") == STATE_OK);
    assert(auction_room_join(&state, bob, "R001") == STATE_OK);

    assert(auction_place_bid(room, alice->id, 100000, &minimum) ==
           AUCTION_BID_OK);
    assert(room->current_price == 100000);
    assert(strcmp(room->highest_bidder_id, "P001") == 0);
    assert(room->bid_count == 1U);

    assert(auction_place_bid(room, bob->id, 109999, &minimum) ==
           AUCTION_BID_PRICE_TOO_LOW);
    assert(minimum == 110000);
    assert(room->current_price == 100000);
    assert(room->bid_count == 1U);

    assert(auction_place_bid(room, bob->id, 160000, &minimum) ==
           AUCTION_BID_OK);
    assert(room->current_price == 160000);
    assert(strcmp(room->highest_bidder_id, "P002") == 0);
    assert(room->bid_count == 2U);

    assert(auction_place_bid(room, alice->id, 0, &minimum) ==
           AUCTION_BID_INVALID_AMOUNT);
    assert(auction_place_bid(room, alice->id, -100, &minimum) ==
           AUCTION_BID_INVALID_AMOUNT);

    room->auction_status = AUCTION_WAITING;
    assert(auction_place_bid(room, alice->id, 170000, &minimum) ==
           AUCTION_BID_NOT_ACTIVE);
}

int main(void)
{
    test_connect_disconnect_and_reconnect();
    test_multiple_rooms_are_managed_in_memory();
    test_join_leave_membership();
    test_auction_bids();
    puts("auction state tests passed");
    return 0;
}

#include "auction.h"
#include "auction_state.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_deadline_and_one_time_expiry(void)
{
    struct auction_state state;
    struct auction_room *room;
    int64_t minimum;

    auction_state_init(&state);
    room = auction_room_find(&state, "R001");
    assert(room != NULL);
    assert(room->auction_status == AUCTION_RUNNING);
    assert(auction_room_timer_start(room, 1000U, 5000U));
    assert(auction_room_remaining_ms(room, 1000U) == 5000U);
    assert(auction_room_remaining_ms(room, 5999U) == 1U);
    assert(!auction_room_expire(room, 5999U));
    assert(auction_place_bid(room, "P001", 100000, &minimum) == AUCTION_BID_OK);
    assert(auction_room_remaining_ms(room, 6000U) == 0U);
    assert(auction_room_expire(room, 6000U));
    assert(room->ended && room->auction_status == AUCTION_ENDED);
    assert(room->has_bid && room->current_price == 100000);
    assert(strcmp(room->highest_bidder_id, "P001") == 0);
    assert(!auction_room_expire(room, 6000U));
    assert(!auction_room_expire(room, 7000U));
    assert(!auction_room_timer_start(room, 7000U, 1000U));
    assert(auction_place_bid(room, "P002", 110000, &minimum) ==
           AUCTION_BID_NOT_ACTIVE);
}

static void test_independent_room_deadlines_and_no_bids(void)
{
    struct auction_state state;
    struct auction_room *first;
    struct auction_room *second;

    auction_state_init(&state);
    first = auction_room_find(&state, "R001");
    second = auction_room_find(&state, "R002");
    assert(first != NULL && second != NULL);
    assert(auction_room_timer_start(first, 100U, 1000U));
    assert(auction_room_timer_start(second, 100U, 3000U));
    assert(auction_room_expire(first, 1100U));
    assert(first->ended && !first->has_bid);
    assert(!second->ended && second->auction_status == AUCTION_RUNNING);
    assert(auction_room_remaining_ms(second, 1100U) == 2000U);
    assert(!auction_room_expire(second, 3099U));
    assert(auction_room_expire(second, 3100U));
    assert(second->ended && !second->has_bid);
}

static void test_invalid_duration_and_overflow(void)
{
    struct auction_state state;
    struct auction_room *room;

    auction_state_init(&state);
    room = auction_room_find(&state, "R001");
    assert(room != NULL);
    assert(!auction_room_timer_start(NULL, 0U, 1000U));
    assert(auction_room_remaining_ms(NULL, 0U) == 0U);
    assert(!auction_room_expire(NULL, UINT64_MAX));
    assert(!auction_room_timer_start(room, 0U, 0U));
    assert(!auction_room_timer_start(room, 0U, 999U));
    assert(!auction_room_timer_start(room, 0U, 3600001U));
    assert(!auction_room_timer_start(room, UINT64_MAX - 999U, 1000U));
    assert(!room->ended);
    assert(auction_room_timer_start(room, 0U, 3600000U));
    assert(!auction_room_timer_start(room, 10U, 1000U));
    assert(auction_room_remaining_ms(room, 10U) == 3599990U);
}

int main(void)
{
    test_deadline_and_one_time_expiry();
    test_independent_room_deadlines_and_no_bids();
    test_invalid_duration_and_overflow();
    puts("auction timer tests passed");
    return 0;
}

#include "auction.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int64_t auction_get_minimum_bid(const struct auction_room *room)
{
    int64_t minimum;

    if (room == NULL) {
        return -1;
    }
    if (!room->has_bid) {
        return room->starting_price;
    }
    if (room->current_price > INT64_MAX - room->min_increment) {
        return -1;
    }
    minimum = room->current_price + room->min_increment;
    return minimum;
}

enum auction_bid_result auction_place_bid(struct auction_room *room,
                                          const char *participant_id,
                                          int64_t amount,
                                          int64_t *minimum_bid)
{
    int64_t minimum;
    struct bid_record *record;
    time_t now;

    if (room == NULL || participant_id == NULL || minimum_bid == NULL ||
        room->auction_status != AUCTION_RUNNING || room->ended) {
        if (minimum_bid != NULL) {
            *minimum_bid = 0;
        }
        return room != NULL && (room->auction_status != AUCTION_RUNNING ||
                                room->ended)
                   ? AUCTION_BID_NOT_ACTIVE
                   : AUCTION_BID_SERVER_ERROR;
    }
    if (amount <= 0) {
        *minimum_bid = auction_get_minimum_bid(room);
        return AUCTION_BID_INVALID_AMOUNT;
    }
    minimum = auction_get_minimum_bid(room);
    if (minimum < 0) {
        *minimum_bid = 0;
        return AUCTION_BID_SERVER_ERROR;
    }
    *minimum_bid = minimum;
    if (amount < minimum) {
        return AUCTION_BID_PRICE_TOO_LOW;
    }
    if (room->bid_count >= MAX_BID_HISTORY) {
        return AUCTION_BID_HISTORY_FULL;
    }

    record = &room->bid_history[room->bid_count];
    memset(record, 0, sizeof(*record));
    (void)snprintf(record->participant_id, sizeof(record->participant_id),
                   "%s", participant_id);
    record->amount = amount;
    now = time(NULL);
    record->timestamp = now;

    room->current_price = amount;
    room->has_bid = true;
    (void)snprintf(room->highest_bidder_id,
                   sizeof(room->highest_bidder_id), "%s", participant_id);
    room->bid_count++;
    return AUCTION_BID_OK;
}

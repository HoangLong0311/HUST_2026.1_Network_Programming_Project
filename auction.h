#ifndef AUCTION_H
#define AUCTION_H

#include "auction_state.h"

#include <stdint.h>

enum auction_bid_result {
    AUCTION_BID_OK = 0,
    AUCTION_BID_HISTORY_FULL,
    AUCTION_BID_INVALID_AMOUNT,
    AUCTION_BID_NOT_ACTIVE,
    AUCTION_BID_PRICE_TOO_LOW,
    AUCTION_BID_SERVER_ERROR
};

int64_t auction_get_minimum_bid(const struct auction_room *room);
enum auction_bid_result auction_place_bid(struct auction_room *room,
                                          const char *participant_id,
                                          int64_t amount,
                                          int64_t *minimum_bid);

#endif

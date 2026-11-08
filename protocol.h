#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "common.h"

struct message_header {
    uint16_t magic;
    uint8_t version;
    uint8_t type;
    uint32_t request_id;
    uint32_t payload_length;
    uint32_t reserved;
};

struct message {
    struct message_header header;
    uint8_t payload[PROTOCOL_MAX_PAYLOAD];
};

enum protocol_result {
    PROTOCOL_ERROR = -1,
    PROTOCOL_DISCONNECT = 0,
    PROTOCOL_OK = 1
};

int send_all(int socket_fd, const void *buffer, size_t length);
int recv_exact(int socket_fd, void *buffer, size_t length);
int encode_header(const struct message_header *header,
                  uint8_t encoded[PROTOCOL_HEADER_SIZE]);
int decode_header(const uint8_t encoded[PROTOCOL_HEADER_SIZE],
                  struct message_header *header);
int send_message(int socket_fd, const struct message_header *header,
                 const void *payload);
int recv_message(int socket_fd, struct message *message);

#endif

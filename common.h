#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

#define SERVER_PORT 8080
#define PROTOCOL_MAGIC 0x4155U
#define PROTOCOL_VERSION 1U
#define PROTOCOL_HEADER_SIZE 16U
#define PROTOCOL_MAX_PAYLOAD 4096U

enum message_type {
    MSG_CONNECT = 0x01,
    MSG_CONNECT_RESULT = 0x02,
    MSG_RECONNECT = 0x03,
    MSG_RECONNECT_RESULT = 0x04,
    MSG_LIST_ROOMS = 0x05,
    MSG_ROOM_LIST = 0x06,
    MSG_CREATE_ROOM = 0x07,
    MSG_CREATE_ROOM_RESULT = 0x08,
    MSG_ERROR = 0x14
};

#endif

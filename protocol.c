#include "protocol.h"

#include <errno.h>
#include <sys/socket.h>

static void put_u32_be(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value >> 24);
    destination[1] = (uint8_t)(value >> 16);
    destination[2] = (uint8_t)(value >> 8);
    destination[3] = (uint8_t)value;
}

static uint32_t get_u32_be(const uint8_t *source)
{
    return ((uint32_t)source[0] << 24) |
           ((uint32_t)source[1] << 16) |
           ((uint32_t)source[2] << 8) |
           (uint32_t)source[3];
}

int send_all(int socket_fd, const void *buffer, size_t length)
{
    const uint8_t *bytes = buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(socket_fd, bytes + sent, length - sent, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PROTOCOL_ERROR;
        }
        if (result == 0) {
            return PROTOCOL_ERROR;
        }
        sent += (size_t)result;
    }

    return PROTOCOL_OK;
}

int recv_exact(int socket_fd, void *buffer, size_t length)
{
    uint8_t *bytes = buffer;
    size_t received = 0;

    while (received < length) {
        ssize_t result = recv(socket_fd, bytes + received,
                              length - received, 0);

        if (result == 0) {
            return received == 0 ? PROTOCOL_DISCONNECT : PROTOCOL_ERROR;
        }
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return PROTOCOL_ERROR;
        }
        received += (size_t)result;
    }

    return PROTOCOL_OK;
}

int encode_header(const struct message_header *header,
                  uint8_t encoded[PROTOCOL_HEADER_SIZE])
{
    if (header == NULL || encoded == NULL ||
        header->magic != PROTOCOL_MAGIC ||
        header->version != PROTOCOL_VERSION ||
        header->payload_length > PROTOCOL_MAX_PAYLOAD ||
        header->reserved != 0) {
        return PROTOCOL_ERROR;
    }

    encoded[0] = (uint8_t)(header->magic >> 8);
    encoded[1] = (uint8_t)header->magic;
    encoded[2] = header->version;
    encoded[3] = header->type;
    put_u32_be(encoded + 4, header->request_id);
    put_u32_be(encoded + 8, header->payload_length);
    put_u32_be(encoded + 12, header->reserved);
    return PROTOCOL_OK;
}

int decode_header(const uint8_t encoded[PROTOCOL_HEADER_SIZE],
                  struct message_header *header)
{
    if (encoded == NULL || header == NULL) {
        return PROTOCOL_ERROR;
    }

    header->magic = (uint16_t)(((uint16_t)encoded[0] << 8) | encoded[1]);
    header->version = encoded[2];
    header->type = encoded[3];
    header->request_id = get_u32_be(encoded + 4);
    header->payload_length = get_u32_be(encoded + 8);
    header->reserved = get_u32_be(encoded + 12);

    if (header->magic != PROTOCOL_MAGIC ||
        header->version != PROTOCOL_VERSION ||
        header->payload_length > PROTOCOL_MAX_PAYLOAD ||
        header->reserved != 0) {
        return PROTOCOL_ERROR;
    }
    return PROTOCOL_OK;
}

int send_message(int socket_fd, const struct message_header *header,
                 const void *payload)
{
    uint8_t encoded[PROTOCOL_HEADER_SIZE];

    if (encode_header(header, encoded) != PROTOCOL_OK ||
        (header->payload_length > 0 && payload == NULL)) {
        return PROTOCOL_ERROR;
    }
    if (send_all(socket_fd, encoded, sizeof(encoded)) != PROTOCOL_OK) {
        return PROTOCOL_ERROR;
    }
    if (header->payload_length > 0 &&
        send_all(socket_fd, payload, header->payload_length) != PROTOCOL_OK) {
        return PROTOCOL_ERROR;
    }
    return PROTOCOL_OK;
}

int recv_message(int socket_fd, struct message *message)
{
    uint8_t encoded[PROTOCOL_HEADER_SIZE];
    int result;

    if (message == NULL) {
        return PROTOCOL_ERROR;
    }
    result = recv_exact(socket_fd, encoded, sizeof(encoded));
    if (result != PROTOCOL_OK) {
        return result;
    }
    if (decode_header(encoded, &message->header) != PROTOCOL_OK) {
        return PROTOCOL_ERROR;
    }
    if (message->header.payload_length > 0 &&
        recv_exact(socket_fd, message->payload,
                   message->header.payload_length) != PROTOCOL_OK) {
        return PROTOCOL_ERROR;
    }
    return PROTOCOL_OK;
}

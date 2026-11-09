#include "logger.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *message_type_name(uint8_t type)
{
    switch (type) {
    case MSG_CONNECT: return "CONNECT";
    case MSG_CONNECT_RESULT: return "CONNECT_RESULT";
    case MSG_RECONNECT: return "RECONNECT";
    case MSG_RECONNECT_RESULT: return "RECONNECT_RESULT";
    case MSG_LIST_ROOMS: return "LIST_ROOMS";
    case MSG_ROOM_LIST: return "ROOM_LIST";
    case MSG_CREATE_ROOM: return "CREATE_ROOM";
    case MSG_CREATE_ROOM_RESULT: return "CREATE_ROOM_RESULT";
    case MSG_ERROR: return "ERROR";
    default: return "UNKNOWN";
    }
}

static void write_timestamp(FILE *file)
{
    time_t now = time(NULL);
    struct tm local_time;
    char timestamp[32];

    if (localtime_r(&now, &local_time) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z",
                 &local_time) == 0) {
        (void)fprintf(file, "time=unknown");
        return;
    }
    (void)fprintf(file, "time=%s", timestamp);
}

static int payload_is_token_message(uint8_t type)
{
    return type == MSG_RECONNECT || type == MSG_RECONNECT_RESULT ||
           type == MSG_CONNECT_RESULT;
}

static void write_escaped_payload(FILE *file, const struct message_header *header,
                                  const uint8_t *payload)
{
    uint32_t index;
    int redact_token = payload_is_token_message(header->type);
    int redact_after_separator = header->type == MSG_RECONNECT ? 1 : 2;
    int separator_count = 0;

    (void)fputc('"', file);
    for (index = 0; index < header->payload_length; index++) {
        uint8_t byte = payload[index];

        if (redact_token && separator_count >= redact_after_separator) {
            (void)fputs("<REDACTED>", file);
            break;
        }
        if (redact_token && byte == (uint8_t)'|') {
            separator_count++;
        }
        if (byte == '\\' || byte == '"') {
            (void)fprintf(file, "\\%c", byte);
        } else if (byte == '\n') {
            (void)fputs("\\n", file);
        } else if (byte == '\r') {
            (void)fputs("\\r", file);
        } else if (byte >= 0x20U && byte <= 0x7eU) {
            (void)fputc(byte, file);
        } else {
            (void)fprintf(file, "\\x%02x", byte);
        }
    }
    (void)fputc('"', file);
}

int logger_open(struct logger *logger, const char *path, const char *component)
{
    if (logger == NULL || path == NULL || component == NULL) {
        return -1;
    }
    logger->file = fopen(path, "a");
    if (logger->file == NULL) {
        return -1;
    }
    logger->component = component;
    (void)setvbuf(logger->file, NULL, _IOLBF, 0);
    return 0;
}

void logger_close(struct logger *logger)
{
    if (logger != NULL && logger->file != NULL) {
        (void)fclose(logger->file);
        logger->file = NULL;
    }
}

void logger_message(struct logger *logger, const char *direction,
                    const struct message_header *header,
                    const void *payload)
{
    if (logger == NULL || logger->file == NULL || direction == NULL ||
        header == NULL || (header->payload_length > 0 && payload == NULL)) {
        return;
    }
    write_timestamp(logger->file);
    (void)fprintf(logger->file,
                  " pid=%ld component=%s direction=%s type=%s(0x%02x)"
                  " request_id=%" PRIu32 " payload_len=%" PRIu32 " payload=",
                  (long)getpid(), logger->component, direction,
                  message_type_name(header->type), header->type,
                  header->request_id, header->payload_length);
    if (header->payload_length == 0) {
        (void)fputs("\"\"", logger->file);
    } else {
        write_escaped_payload(logger->file, header, payload);
    }
    (void)fputc('\n', logger->file);
}

void logger_event(struct logger *logger, const char *level, const char *message)
{
    if (logger == NULL || logger->file == NULL || level == NULL ||
        message == NULL) {
        return;
    }
    write_timestamp(logger->file);
    (void)fprintf(logger->file, " pid=%ld component=%s level=%s event=\"",
                  (long)getpid(), logger->component, level);
    write_escaped_payload(logger->file,
                          &(struct message_header){.payload_length =
                                                       (uint32_t)strlen(message)},
                          (const uint8_t *)message);
    (void)fputc('\n', logger->file);
}

#include "logger.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void log_payload(struct logger *logger, uint8_t type,
                        const void *payload, size_t length)
{
    struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, type, 5U, (uint32_t)length, 0U
    };
    logger_message(logger, "RECV", &header, payload);
}

int main(void)
{
    static const char malformed[] = "SECRET_MALFORMED";
    static const char reconnect[] = "P001|SECRET_RECONNECT";
    static const char connect_result[] = "OK|P001|SECRET_CONNECT";
    static const char reconnect_result[] = "OK|P001|SECRET_RESULT";
    static const char ordinary[] = "R001|A001|150000";
    static const char escaped[] = "line\nquote\"backslash\\";
    char result[8192];
    size_t size;
    unsigned int count = 0;
    char *cursor;
    struct logger logger = {0};

    logger.file = tmpfile();
    assert(logger.file != NULL);
    logger.component = "TEST";

    /* Redact entire credentials, including malformed payload without '|'. */
    log_payload(&logger, MSG_RECONNECT, malformed, sizeof(malformed) - 1U);
    log_payload(&logger, MSG_RECONNECT, reconnect, sizeof(reconnect) - 1U);
    log_payload(&logger, MSG_CONNECT_RESULT, connect_result,
                sizeof(connect_result) - 1U);
    log_payload(&logger, MSG_RECONNECT_RESULT, reconnect_result,
                sizeof(reconnect_result) - 1U);
    log_payload(&logger, MSG_PLACE_BID, ordinary, sizeof(ordinary) - 1U);
    logger_event(&logger, "INFO", escaped);

    assert(fflush(logger.file) == 0);
    rewind(logger.file);
    size = fread(result, 1U, sizeof(result) - 1U, logger.file);
    assert(!ferror(logger.file));
    result[size] = '\0';
    assert(strstr(result, "SECRET_") == NULL);
    assert(strstr(result, ordinary) != NULL);
    assert(strstr(result, "line\\nquote\\\"backslash\\\\") != NULL);
    cursor = result;
    while ((cursor = strstr(cursor, "<REDACTED>")) != NULL) {
        count++;
        cursor += strlen("<REDACTED>");
    }
    assert(count == 4U);
    logger_close(&logger);
    puts("logger tests passed");
    return 0;
}

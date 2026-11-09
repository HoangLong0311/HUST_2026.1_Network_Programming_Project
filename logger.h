#ifndef LOGGER_H
#define LOGGER_H

#include "protocol.h"

#include <stdio.h>

struct logger {
    FILE *file;
    const char *component;
};

int logger_open(struct logger *logger, const char *path, const char *component);
void logger_close(struct logger *logger);
void logger_message(struct logger *logger, const char *direction,
                    const struct message_header *header,
                    const void *payload);
void logger_event(struct logger *logger, const char *level, const char *message);

#endif

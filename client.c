#include "auction_state.h"
#include "logger.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

static struct logger client_logger;

static int is_safe_text(const char *text, size_t maximum_length, int allow_empty)
{
    const unsigned char *character = (const unsigned char *)text;
    size_t length = strlen(text);

    if ((!allow_empty && length == 0) || length >= maximum_length) {
        return 0;
    }
    while (*character != '\0') {
        if (*character < 0x20U || *character > 0x7eU ||
            *character == (unsigned char)'|') {
            return 0;
        }
        character++;
    }
    return 1;
}

static int send_request(int socket_fd, uint8_t type, uint32_t request_id,
                        const void *payload, size_t payload_length)
{
    struct message_header header;

    if (payload_length > PROTOCOL_MAX_PAYLOAD) {
        return PROTOCOL_ERROR;
    }
    header.magic = PROTOCOL_MAGIC;
    header.version = PROTOCOL_VERSION;
    header.type = type;
    header.request_id = request_id;
    header.payload_length = (uint32_t)payload_length;
    header.reserved = 0;
    if (send_message(socket_fd, &header, payload) != PROTOCOL_OK) {
        logger_event(&client_logger, "ERROR", "send_message failed");
        return PROTOCOL_ERROR;
    }
    logger_message(&client_logger, "SEND", &header, payload);
    return PROTOCOL_OK;
}

static void print_payload(const struct message *message)
{
    printf("%.*s\n", (int)message->header.payload_length,
           (const char *)message->payload);
}

static int print_notification(const struct message *message)
{
    const char *label;

    if (message->header.request_id != 0U) return 0;
    switch (message->header.type) {
    case MSG_BID_UPDATE: label = "BID_UPDATE"; break;
    case MSG_TIME_UPDATE: label = "TIME_UPDATE"; break;
    case MSG_TIME_WARNING: label = "TIME_WARNING"; break;
    case MSG_AUCTION_RESULT: label = "AUCTION_RESULT"; break;
    default: return 0;
    }
    printf("\n%s: ", label);
    print_payload(message);
    fflush(stdout);
    return 1;
}

/* Returns 0 for the expected response, 1 for a server error, and -1 for I/O. */
static int receive_response_copy(int socket_fd, uint32_t request_id,
                                 uint8_t expected_type, const char *label,
                                 char *payload, size_t payload_capacity)
{
    struct message response;

    do {
        if (recv_message(socket_fd, &response) != PROTOCOL_OK) {
            logger_event(&client_logger, "ERROR", "receive frame failed");
            fprintf(stderr, "Khong the nhan phan hoi tu server.\n");
            return -1;
        }
        logger_message(&client_logger, "RECV", &response.header, response.payload);
    } while (print_notification(&response));
    if (response.header.request_id != request_id) {
        fprintf(stderr, "request_id phan hoi khong khop.\n");
        return -1;
    }
    if (response.header.type == MSG_ERROR) {
        printf("ERROR: ");
        print_payload(&response);
        return 1;
    }
    if (response.header.type != expected_type) {
        fprintf(stderr, "Loai phan hoi khong hop le: 0x%02x\n",
                response.header.type);
        return -1;
    }

    if (payload != NULL) {
        if (payload_capacity == 0U ||
            response.header.payload_length >= payload_capacity) {
            fprintf(stderr, "Payload phan hoi qua dai.\n");
            return -1;
        }
        memcpy(payload, response.payload, response.header.payload_length);
        payload[response.header.payload_length] = '\0';
    }

    printf("%s: ", label);
    print_payload(&response);
    if ((expected_type == MSG_JOIN_RESULT ||
         expected_type == MSG_LEAVE_RESULT ||
         expected_type == MSG_BID_RESULT) &&
        response.header.payload_length >= 4U &&
        strncmp((const char *)response.payload, "FAIL", 4U) == 0) {
        return 1;
    }
    return 0;
}

static int receive_response(int socket_fd, uint32_t request_id,
                            uint8_t expected_type, const char *label)
{
    return receive_response_copy(socket_fd, request_id, expected_type, label,
                                 NULL, 0U);
}

static int exchange_request(int socket_fd, uint8_t type, uint32_t request_id,
                            const void *payload, size_t payload_length,
                            uint8_t expected_type, const char *label)
{
    if (send_request(socket_fd, type, request_id, payload, payload_length) !=
        PROTOCOL_OK) {
        fprintf(stderr, "Khong the gui yeu cau den server.\n");
        return -1;
    }
    return receive_response(socket_fd, request_id, expected_type, label);
}

static int exchange_request_copy(int socket_fd, uint8_t type,
                                 uint32_t request_id,
                                 const void *payload, size_t payload_length,
                                 uint8_t expected_type, const char *label,
                                 char *response, size_t response_capacity)
{
    if (send_request(socket_fd, type, request_id, payload, payload_length) !=
        PROTOCOL_OK) {
        fprintf(stderr, "Khong the gui yeu cau den server.\n");
        return -1;
    }
    return receive_response_copy(socket_fd, request_id, expected_type, label,
                                 response, response_capacity);
}

static int request_auction_state(int socket_fd, uint32_t request_id,
                                 const char *room_id)
{
    return exchange_request(socket_fd, MSG_GET_AUCTION_STATE, request_id,
                            room_id, strlen(room_id), MSG_AUCTION_STATE,
                            "AUCTION_STATE");
}

static int synchronize_session(int socket_fd, uint32_t *request_id,
                               char current_room_id[ROOM_ID_CAPACITY])
{
    char response[ROOM_ID_CAPACITY + 3U];
    const char *room_id;
    int result;

    (*request_id)++;
    result = exchange_request_copy(socket_fd, MSG_GET_SESSION_STATE,
                                   *request_id, NULL, 0U,
                                   MSG_SESSION_STATE, "SESSION_STATE",
                                   response, sizeof(response));
    if (result != 0) {
        return result;
    }
    if (strncmp(response, "OK|", 3U) != 0) {
        fprintf(stderr, "SESSION_STATE khong hop le.\n");
        return -1;
    }
    room_id = response + 3U;
    if (!is_safe_text(room_id, ROOM_ID_CAPACITY, 0)) {
        fprintf(stderr, "Room ID trong SESSION_STATE khong hop le.\n");
        return -1;
    }
    (void)snprintf(current_room_id, ROOM_ID_CAPACITY, "%s", room_id);
    if (strcmp(current_room_id, NO_ROOM) == 0) {
        return 0;
    }

    (*request_id)++;
    return request_auction_state(socket_fd, *request_id, current_room_id);
}

static void trim_line_ending(char *text)
{
    text[strcspn(text, "\r\n")] = '\0';
}

static int read_live_line(int socket_fd, char *text, size_t capacity)
{
    struct pollfd descriptors[2];
    descriptors[0].fd = STDIN_FILENO;
    descriptors[0].events = POLLIN;
    descriptors[1].fd = socket_fd;
    descriptors[1].events = POLLIN;

    for (;;) {
        int ready = poll(descriptors, 2, -1);
        size_t length;

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            struct message push;
            if (recv_message(socket_fd, &push) != PROTOCOL_OK) {
                return -1;
            }
            logger_message(&client_logger, "RECV",
                           &push.header, push.payload);
            if (!print_notification(&push)) {
                fprintf(stderr, "Nhan duoc message khong mong doi.\n");
                return -1;
            }
            descriptors[1].revents = 0;
        }
        if (descriptors[0].revents & (POLLIN | POLLHUP)) {
            if (fgets(text, (int)capacity, stdin) == NULL) {
                return 0;
            }
            length = strlen(text);
            if (length == capacity - 1U &&
                (length == 0U || text[length - 1U] != '\n')) {
                int character;
                while ((character = getchar()) != '\n' &&
                       character != EOF) {
                }
                printf("Input qua dai.\n");
                text[0] = '\0';
                return 1;
            }
            trim_line_ending(text);
            return 1;
        }
    }
}

static void print_usage(const char *program)
{
    fprintf(stderr, "Su dung:\n");
    fprintf(stderr, "  %s [participant-name]\n", program);
    fprintf(stderr, "  %s --reconnect P001 SESSION_TOKEN\n", program);
}

int main(int argc, char *argv[])
{
    int socket_fd;
    int request_result;
    uint32_t request_id = 1;
    char current_room_id[ROOM_ID_CAPACITY];
    uint8_t initial_type = MSG_CONNECT;
    uint8_t expected_initial_type = MSG_CONNECT_RESULT;
    const char *initial_payload = "";
    size_t initial_payload_length = 0;
    char reconnect_payload[PARTICIPANT_ID_CAPACITY + SESSION_TOKEN_CAPACITY];
    struct sockaddr_in server_address;

    (void)snprintf(current_room_id, sizeof(current_room_id), "%s", NO_ROOM);

    if (argc == 2) {
        if (strcmp(argv[1], "--reconnect") == 0 ||
            !is_safe_text(argv[1], PARTICIPANT_NAME_CAPACITY, 0)) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        initial_payload = argv[1];
        initial_payload_length = strlen(initial_payload);
    } else if (argc == 4 && strcmp(argv[1], "--reconnect") == 0) {
        int written;

        if (!is_safe_text(argv[2], PARTICIPANT_ID_CAPACITY, 0) ||
            !is_safe_text(argv[3], SESSION_TOKEN_CAPACITY, 0)) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        written = snprintf(reconnect_payload, sizeof(reconnect_payload),
                           "%s|%s", argv[2], argv[3]);
        if (written < 0 || (size_t)written >= sizeof(reconnect_payload)) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        initial_type = MSG_RECONNECT;
        expected_initial_type = MSG_RECONNECT_RESULT;
        initial_payload = reconnect_payload;
        initial_payload_length = (size_t)written;
    } else if (argc != 1) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);
    (void)setvbuf(stdin, NULL, _IONBF, 0);
    if (logger_open(&client_logger, "auction.log", "CLIENT") != 0) {
        perror("auction.log");
        return EXIT_FAILURE;
    }
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        logger_close(&client_logger);
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &server_address.sin_addr) <= 0 ||
        connect(socket_fd, (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0) {
        perror("connect");
        close(socket_fd);
        logger_close(&client_logger);
        return EXIT_FAILURE;
    }

    request_result = exchange_request(socket_fd, initial_type, request_id,
                                      initial_payload, initial_payload_length,
                                      expected_initial_type,
                                      initial_type == MSG_CONNECT
                                          ? "CONNECT_RESULT"
                                          : "RECONNECT_RESULT");
    if (request_result != 0) {
        close(socket_fd);
        logger_close(&client_logger);
        return EXIT_FAILURE;
    }
    request_result = synchronize_session(socket_fd, &request_id,
                                         current_room_id);
    if (request_result != 0) {
        close(socket_fd);
        logger_close(&client_logger);
        return EXIT_FAILURE;
    }

    for (;;) {
        char command[ROOM_NAME_CAPACITY + 32U];
        const char *room_name;

        printf("\n===== ONLINE AUCTION CLIENT =====\n");
        printf("1. List rooms\n");
        printf("2. Create room\n");
        printf("3. Join room\n");
        printf("4. Leave room\n");
        printf("5. View room state\n");
        printf("6. Place bid\n");
        printf("7. View current room\n");
        printf("8. Reconnect help\n");
        printf("9. Exit\n");
        printf("Choose: ");
        fflush(stdout);
        request_result = read_live_line(socket_fd, command, sizeof(command));
        if (request_result == 0) {
            break;
        }
        if (request_result < 0) {
            close(socket_fd);
            logger_close(&client_logger);
            return EXIT_FAILURE;
        }
        if (request_result > 1) {
            continue;
        }
        if (strcmp(command, "9") == 0 || strcmp(command, "0") == 0 ||
            strcmp(command, "exit") == 0) {
            break;
        }

        request_id++;
        if (strcmp(command, "1") == 0) {
            request_result = exchange_request(socket_fd, MSG_LIST_ROOMS,
                                              request_id, NULL, 0,
                                              MSG_ROOM_LIST, "ROOM_LIST");
        } else if (strcmp(command, "2") == 0) {
            char room_name_input[ROOM_NAME_CAPACITY];

            printf("Room name: ");
            fflush(stdout);
            if (read_live_line(socket_fd, room_name_input, sizeof(room_name_input)) <= 0) break;
            room_name = room_name_input;
            if (!is_safe_text(room_name, ROOM_NAME_CAPACITY, 0)) {
                printf("Ten phong khong hop le.\n");
                continue;
            }
            request_result = exchange_request(socket_fd, MSG_CREATE_ROOM,
                                              request_id, room_name,
                                              strlen(room_name),
                                              MSG_CREATE_ROOM_RESULT,
                                              "CREATE_ROOM_RESULT");
        } else if (strcmp(command, "3") == 0) {
            char room_id[ROOM_ID_CAPACITY];

            printf("[JOIN ROOM]\nRoom ID: ");
            fflush(stdout);
            if (read_live_line(socket_fd, room_id, sizeof(room_id)) <= 0) break;
            if (!is_safe_text(room_id, ROOM_ID_CAPACITY, 0)) {
                printf("Room ID khong hop le.\n");
                continue;
            }
            printf("Joining room %s...\n", room_id);
            request_result = exchange_request(socket_fd, MSG_JOIN_ROOM,
                                              request_id, room_id,
                                              strlen(room_id),
                                              MSG_JOIN_RESULT, "JOIN_RESULT");
            if (request_result == 0) {
                (void)snprintf(current_room_id, sizeof(current_room_id),
                               "%s", room_id);
                printf("Successfully joined room %s.\n", room_id);
                printf("Current room: %s\n", current_room_id);
                request_result = receive_response(socket_fd, request_id,
                                                  MSG_ROOM_STATE,
                                                  "ROOM_STATE");
                if (request_result == 0) {
                    request_id++;
                    request_result = request_auction_state(socket_fd,
                                                           request_id,
                                                           current_room_id);
                }
            } else if (request_result == 1) {
                printf("Join room failed.\n");
            }
        } else if (strcmp(command, "4") == 0) {
            char room_id[ROOM_ID_CAPACITY];

            if (strcmp(current_room_id, NO_ROOM) == 0) {
                printf("You are not currently in a room.\n");
                continue;
            }
            printf("[LEAVE ROOM]\nLeaving room %s...\n", current_room_id);
            (void)snprintf(room_id, sizeof(room_id), "%s", current_room_id);
            request_result = exchange_request(socket_fd, MSG_LEAVE_ROOM,
                                              request_id, room_id,
                                              strlen(room_id),
                                              MSG_LEAVE_RESULT,
                                              "LEAVE_RESULT");
            if (request_result == 0) {
                (void)snprintf(current_room_id, sizeof(current_room_id),
                               "%s", NO_ROOM);
                printf("Successfully left room %s.\n", room_id);
                printf("You are not currently in a room.\n");
                request_result = receive_response(socket_fd, request_id,
                                                  MSG_ROOM_STATE,
                                                  "ROOM_STATE");
            }
        } else if (strcmp(command, "5") == 0) {
            char room_id[ROOM_ID_CAPACITY];

            printf("Room ID: ");
            fflush(stdout);
            if (read_live_line(socket_fd, room_id, sizeof(room_id)) <= 0) {
                continue;
            }
            request_result = exchange_request(socket_fd, MSG_GET_ROOM_STATE,
                                              request_id, room_id,
                                              strlen(room_id),
                                              MSG_ROOM_STATE, "ROOM_STATE");
        } else if (strcmp(command, "7") == 0) {
            if (strcmp(current_room_id, NO_ROOM) == 0) {
                printf("You are not currently in a room.\n");
            } else {
                printf("Current room: %s\n", current_room_id);
                request_result = request_auction_state(socket_fd, request_id,
                                                       current_room_id);
                if (request_result < 0) {
                    close(socket_fd);
                    logger_close(&client_logger);
                    return EXIT_FAILURE;
                }
            }
            continue;
        } else if (strcmp(command, "8") == 0) {
            printf("Reconnect tu terminal moi:\n");
            printf("  ./client --reconnect P001 <session-token>\n");
            continue;
        } else if (strcmp(command, "6") == 0) {
            char room_id[ROOM_ID_CAPACITY];
            char auction_id[AUCTION_ID_CAPACITY];
            char amount_text[64];
            char payload[ROOM_ID_CAPACITY + AUCTION_ID_CAPACITY + 80U];
            char *end;
            int64_t amount;

            printf("[PLACE BID]\nEnter room ID: ");
            fflush(stdout);
            if (read_live_line(socket_fd, room_id, sizeof(room_id)) <= 0) {
                continue;
            }
            printf("Enter auction ID: ");
            fflush(stdout);
            if (read_live_line(socket_fd, auction_id,
                                sizeof(auction_id)) <= 0) {
                continue;
            }
            printf("Enter your bid: ");
            fflush(stdout);
            if (read_live_line(socket_fd, amount_text,
                                sizeof(amount_text)) <= 0) {
                continue;
            }

            errno = 0;
            amount = strtoll(amount_text, &end, 10);
            if (errno == ERANGE || end == amount_text || *end != '\0' ||
                amount_text[0] == '-' || amount_text[0] == '+' ||
                amount <= 0) {
                printf("INVALID_AMOUNT\n");
                continue;
            }
            if (snprintf(payload, sizeof(payload), "%s|%s|%" PRId64,
                         room_id, auction_id, amount) >=
                (int)sizeof(payload)) {
                printf("Payload khong hop le.\n");
                continue;
            }
            request_result = exchange_request(socket_fd, MSG_PLACE_BID,
                                              request_id, payload,
                                              strlen(payload),
                                              MSG_BID_RESULT, "BID_RESULT");
            if (request_result == 0) {
                printf("Bid accepted!\n");
            } else if (request_result == 1) {
                printf("Bid rejected.\n");
            }
        } else {
            printf("Lenh khong hop le.\n");
            continue;
        }

        if (request_result < 0) {
            close(socket_fd);
            logger_close(&client_logger);
            return EXIT_FAILURE;
        }
    }

    close(socket_fd);
    logger_close(&client_logger);
    return EXIT_SUCCESS;
}

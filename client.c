#include "auction_state.h"
#include "logger.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* Returns 0 for the expected response, 1 for a server error, and -1 for I/O. */
static int receive_response(int socket_fd, uint32_t request_id,
                            uint8_t expected_type, const char *label)
{
    struct message response;

    if (recv_message(socket_fd, &response) != PROTOCOL_OK) {
        logger_event(&client_logger, "ERROR", "receive frame failed");
        fprintf(stderr, "Khong the nhan phan hoi tu server.\n");
        return -1;
    }
    logger_message(&client_logger, "RECV", &response.header, response.payload);
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

    printf("%s: ", label);
    print_payload(&response);
    if ((expected_type == MSG_JOIN_RESULT ||
         expected_type == MSG_LEAVE_RESULT) &&
        response.header.payload_length >= 4U &&
        strncmp((const char *)response.payload, "FAIL", 4U) == 0) {
        return 1;
    }
    return 0;
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

static void trim_line_ending(char *text)
{
    text[strcspn(text, "\r\n")] = '\0';
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

    for (;;) {
        char command[ROOM_NAME_CAPACITY + 32U];
        const char *room_name;

        printf("\n===== ONLINE AUCTION CLIENT =====\n");
        printf("1. List rooms\n");
        printf("2. Create room\n");
        printf("3. Join room\n");
        printf("4. Leave room\n");
        printf("5. View current room\n");
        printf("6. View room state\n");
        printf("7. Reconnect help\n");
        printf("0. Exit\n");
        printf("Choose: ");
        fflush(stdout);
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }
        trim_line_ending(command);
        if (strcmp(command, "0") == 0 || strcmp(command, "exit") == 0) {
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
            if (fgets(room_name_input, sizeof(room_name_input), stdin) == NULL) {
                continue;
            }
            trim_line_ending(room_name_input);
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
            if (fgets(room_id, sizeof(room_id), stdin) == NULL) {
                continue;
            }
            trim_line_ending(room_id);
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
            if (strcmp(current_room_id, NO_ROOM) == 0) {
                printf("You are not currently in a room.\n");
            } else {
                printf("Current room: %s\n", current_room_id);
            }
            continue;
        } else if (strcmp(command, "6") == 0) {
            char room_id[ROOM_ID_CAPACITY];

            printf("Room ID: ");
            fflush(stdout);
            if (fgets(room_id, sizeof(room_id), stdin) == NULL) {
                continue;
            }
            trim_line_ending(room_id);
            request_result = exchange_request(socket_fd, MSG_GET_ROOM_STATE,
                                              request_id, room_id,
                                              strlen(room_id),
                                              MSG_ROOM_STATE, "ROOM_STATE");
        } else if (strcmp(command, "7") == 0) {
            printf("Reconnect tu terminal moi:\n");
            printf("  ./client --reconnect P001 <session-token>\n");
            continue;
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

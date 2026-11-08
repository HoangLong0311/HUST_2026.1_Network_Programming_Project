#include "auction_state.h"
#include "protocol.h"

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
    return send_message(socket_fd, &header, payload);
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
        fprintf(stderr, "Khong the nhan phan hoi tu server.\n");
        return -1;
    }
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
    uint8_t initial_type = MSG_CONNECT;
    uint8_t expected_initial_type = MSG_CONNECT_RESULT;
    const char *initial_payload = "";
    size_t initial_payload_length = 0;
    char reconnect_payload[PARTICIPANT_ID_CAPACITY + SESSION_TOKEN_CAPACITY];
    struct sockaddr_in server_address;

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
    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
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
        return EXIT_FAILURE;
    }

    for (;;) {
        char command[ROOM_NAME_CAPACITY + 16U];
        const char *room_name;

        printf("Nhap lenh (LIST_ROOMS, CREATE_ROOM <ten>, exit): ");
        fflush(stdout);
        if (fgets(command, sizeof(command), stdin) == NULL) {
            break;
        }
        trim_line_ending(command);
        if (strcmp(command, "exit") == 0) {
            break;
        }

        request_id++;
        if (strcmp(command, "LIST_ROOMS") == 0) {
            request_result = exchange_request(socket_fd, MSG_LIST_ROOMS,
                                              request_id, NULL, 0,
                                              MSG_ROOM_LIST, "ROOM_LIST");
        } else if (strncmp(command, "CREATE_ROOM ", 12U) == 0) {
            room_name = command + 12U;
            if (!is_safe_text(room_name, ROOM_NAME_CAPACITY, 0)) {
                printf("Ten phong khong hop le.\n");
                continue;
            }
            request_result = exchange_request(socket_fd, MSG_CREATE_ROOM,
                                              request_id, room_name,
                                              strlen(room_name),
                                              MSG_CREATE_ROOM_RESULT,
                                              "CREATE_ROOM_RESULT");
        } else {
            printf("Lenh khong hop le.\n");
            continue;
        }

        if (request_result < 0) {
            close(socket_fd);
            return EXIT_FAILURE;
        }
    }

    close(socket_fd);
    return EXIT_SUCCESS;
}

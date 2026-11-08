#include "protocol.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int send_request(int socket_fd, uint8_t type, uint32_t request_id)
{
    struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, type, request_id, 0, 0
    };

    return send_message(socket_fd, &header, NULL);
}

static void print_payload(const struct message *message)
{
    printf("%.*s\n", (int)message->header.payload_length,
           (const char *)message->payload);
}

int main(void)
{
    int socket_fd;
    uint32_t request_id = 1;
    struct sockaddr_in server_address;
    struct message response;

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

    if (send_request(socket_fd, MSG_CONNECT, request_id) != PROTOCOL_OK ||
        recv_message(socket_fd, &response) != PROTOCOL_OK) {
        fprintf(stderr, "Không thể hoàn tất CONNECT.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }
    if (response.header.request_id != request_id ||
        response.header.type != MSG_CONNECT_RESULT) {
        fprintf(stderr, "CONNECT_RESULT không hợp lệ.\n");
        close(socket_fd);
        return EXIT_FAILURE;
    }
    printf("CONNECT_RESULT: ");
    print_payload(&response);

    for (;;) {
        char command[128];

        printf("Nhập lệnh (LIST_ROOMS hoặc exit): ");
        fflush(stdout);
        if (fgets(command, sizeof(command), stdin) == NULL ||
            strncmp(command, "exit", 4) == 0) {
            break;
        }
        if (strncmp(command, "LIST_ROOMS", 10) != 0) {
            printf("Lệnh không hợp lệ.\n");
            continue;
        }

        request_id++;
        if (send_request(socket_fd, MSG_LIST_ROOMS, request_id) !=
                PROTOCOL_OK ||
            recv_message(socket_fd, &response) != PROTOCOL_OK) {
            fprintf(stderr, "Không thể nhận phản hồi từ server.\n");
            close(socket_fd);
            return EXIT_FAILURE;
        }
        if (response.header.request_id != request_id) {
            fprintf(stderr, "request_id phản hồi không khớp.\n");
            close(socket_fd);
            return EXIT_FAILURE;
        }
        if (response.header.type == MSG_ROOM_LIST) {
            printf("ROOM_LIST:\n");
            print_payload(&response);
        } else if (response.header.type == MSG_ERROR) {
            printf("ERROR: ");
            print_payload(&response);
        } else {
            fprintf(stderr, "Loại phản hồi không hợp lệ: 0x%02x\n",
                    response.header.type);
        }
    }

    close(socket_fd);
    return EXIT_SUCCESS;
}

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int send_text(int socket_fd, uint8_t type, uint32_t request_id,
                     const char *text)
{
    struct message_header header = {
        PROTOCOL_MAGIC, PROTOCOL_VERSION, type, request_id,
        (uint32_t)strlen(text), 0
    };

    return send_message(socket_fd, &header, text);
}

static int handle_client(int client_fd)
{
    int connected = 0;

    for (;;) {
        struct message message;
        int result = recv_message(client_fd, &message);

        if (result == PROTOCOL_DISCONNECT) {
            printf("Client đã ngắt kết nối.\n");
            return 0;
        }
        if (result != PROTOCOL_OK) {
            fprintf(stderr, "Protocol error: header hoặc payload không hợp lệ.\n");
            if (send_text(client_fd, MSG_ERROR, message.header.request_id,
                          "INVALID_MESSAGE") != PROTOCOL_OK) {
                return -1;
            }
            continue;
        }

        switch (message.header.type) {
        case MSG_CONNECT:
            if (connected) {
                send_text(client_fd, MSG_ERROR, message.header.request_id,
                          "ALREADY_CONNECTED");
                break;
            }
            connected = 1;
            if (send_text(client_fd, MSG_CONNECT_RESULT,
                          message.header.request_id, "OK|P001|TOKEN_DEMO") !=
                PROTOCOL_OK) {
                return -1;
            }
            break;
        case MSG_LIST_ROOMS:
            if (!connected) {
                if (send_text(client_fd, MSG_ERROR,
                              message.header.request_id, "NOT_CONNECTED") !=
                    PROTOCOL_OK) {
                    return -1;
                }
                break;
            }
            if (send_text(client_fd, MSG_ROOM_LIST, message.header.request_id,
                          "P001|Demo Room\nP002|Practice Room") !=
                PROTOCOL_OK) {
                return -1;
            }
            break;
        default:
            if (send_text(client_fd, MSG_ERROR, message.header.request_id,
                          "UNKNOWN_MESSAGE_TYPE") != PROTOCOL_OK) {
                return -1;
            }
            break;
        }
    }
}

int main(void)
{
    int server_fd;
    int option = 1;
    struct sockaddr_in server_address;

    signal(SIGPIPE, SIG_IGN);
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &option,
                   sizeof(option)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERVER_PORT);
    if (bind(server_fd, (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0 ||
        listen(server_fd, 1) < 0) {
        perror("bind/listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server đang lắng nghe tại port %d...\n", SERVER_PORT);
    fflush(stdout);
    for (;;) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            close(server_fd);
            return EXIT_FAILURE;
        }
        printf("Client đã kết nối.\n");
        fflush(stdout);
        handle_client(client_fd);
        close(client_fd);
    }
}

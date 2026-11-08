# Giai đoạn 2 - TCP Protocol V1

## Biên dịch

```bash
cd /home/mayb/Desktop/LTM/BTL
make
```

Các lệnh `make server`, `make client` và `make clean` cũng được hỗ trợ.

## Chạy

Mở hai terminal:

```bash
./server
./client
```

Client tự gửi `CONNECT` với request ID đầu tiên. Sau khi nhận
`CONNECT_RESULT` là `OK|P001|TOKEN_DEMO`, nhập `LIST_ROOMS` để gửi request
và nhận danh sách phòng. Nhập `exit` để đóng kết nối.

## Protocol V1

Mỗi message có header 16 byte, theo thứ tự byte:

`magic (2) | version (1) | type (1) | request_id (4) | payload_length (4) | reserved (4)`

Các số nhiều byte được encode big-endian. Payload không có NUL terminator và
có kích thước từ 0 đến 4096 byte. `protocol.c` xử lý partial `send`/`recv`,
`EINTR`, disconnect, kiểm tra magic/version/reserved và giới hạn payload.
Server chạy tuần tự, chưa có multithreading hoặc logic đấu giá.

## Kiểm thử protocol

`recv_message()` đọc chính xác 16 byte header rồi đọc chính xác payload nên
không phụ thuộc vào ranh giới của TCP. Header sai magic/version, reserved khác
0 hoặc payload lớn hơn 4096 bị từ chối. Response luôn echo `request_id` của
request tương ứng.

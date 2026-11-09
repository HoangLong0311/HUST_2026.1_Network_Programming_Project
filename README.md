# Online Auction - G3 & G4

Đây là chương trình TCP socket bằng C trên Linux, biên dịch bằng GCC. Bản
hiện tại mở rộng Protocol V1 với:

- **G3:** participant, `CONNECT`, session token và `RECONNECT`.
- **G4:** danh sách phòng đấu giá trong bộ nhớ và `CREATE_ROOM`.

Chưa triển khai `JOIN`/`LEAVE`, đặt giá, timer, lưu dữ liệu lâu dài hoặc
multithreading. Server dùng `poll()` để phục vụ nhiều client trong một tiến
trình; mỗi request vẫn được xử lý tuần tự trong event loop.

## Cấu trúc mã nguồn

- `common.h`: hằng số protocol, port và message type.
- `protocol.h`, `protocol.c`: đóng gói/giải mã frame TCP và xử lý partial
  I/O.
- `auction_state.h`, `auction_state.c`: quản lý participant, token và phòng
  trong bộ nhớ.
- `server.c`: socket server, xác thực request và điều phối state.
- `client.c`: client tương tác dòng lệnh, hỗ trợ connect/reconnect và các lệnh
  phòng.
- `logger.h`, `logger.c`: log append-only cho message gửi/nhận và sự kiện lỗi.
- `Makefile`: build chương trình và test.
- `tests/`: test protocol, state và server integration.

## Biên dịch và kiểm thử

Yêu cầu Linux hoặc WSL có `gcc` và `make`:

```bash
make
make test
```

Các target riêng:

```bash
make server
make client
make clean
```

`make test` biên dịch và chạy:

1. test encode/decode và round-trip frame;
2. test tạo participant, disconnect/reconnect, token và phòng;
3. test server thật, quyền truy cập trước `CONNECT`, request ID, reconnect,
   tạo phòng và liệt kê phòng.

## Chạy chương trình

Terminal 1:

```bash
./server
```

Server mặc định lắng nghe TCP port `8080`. Có thể truyền port khác để chạy
integration test độc lập:

```bash
./server 25000
```

Terminal 2:

```bash
./client Alice
```

Client kết nối tới `127.0.0.1:8080`, gửi `CONNECT` và in kết quả:

```text
CONNECT_RESULT: OK|P001|<session-token>
```

Lưu `participant ID` và `session-token` để reconnect sau khi client đóng
socket:

```bash
./client --reconnect P001 <session-token>
```

Trong phiên đã kết nối, các lệnh được hỗ trợ:

```text
LIST_ROOMS
CREATE_ROOM Sunday Auction
exit
```

Server khởi tạo sẵn `R001|Demo Room` và `R002|Practice Room`. Mỗi phòng mới
được cấp ID tăng dần; tên phòng trùng bị từ chối. Participant, token và phòng
chỉ tồn tại trong bộ nhớ và mất khi server khởi động lại.

## Log giao tiếp

Server và client cùng ghi append vào file `auction.log` tại thư mục hiện tại.
Mỗi dòng log gồm:

- timestamp ISO-8601 và process ID;
- `component` (`SERVER` hoặc `CLIENT`);
- `direction` (`SEND` hoặc `RECV`);
- message type và mã hex;
- `request_id`;
- độ dài payload;
- payload dạng text đã escape (`\n`, `\r`, `\xNN`, dấu nháy).

Ví dụ:

```text
time=2026-11-09T15:20:00+0700 pid=1234 component=CLIENT direction=SEND type=LIST_ROOMS(0x05) request_id=2 payload_len=0 payload=""
```

File được mở ở chế độ append và flush sau mỗi dòng để giữ log khi chương
trình đang chạy. Payload của `RECONNECT` và `RECONNECT_RESULT` được che token
thành `<REDACTED>`; không nên ghi credential đầy đủ vào log. Lỗi gửi/nhận và
frame không hợp lệ cũng được ghi với `level=ERROR`.

`auction.log` là dữ liệu runtime, không phải source code; có thể xóa trước một
lần chạy mới nếu muốn log riêng:

```bash
rm -f auction.log
```

## Protocol V1

Mỗi message gồm header 16 byte và payload từ 0 đến 4096 byte:

```text
magic (2) | version (1) | type (1) | request_id (4) |
payload_length (4) | reserved (4)
```

- `magic = 0x4155` (`AU`);
- `version = 1`;
- các số nhiều byte dùng big-endian;
- `reserved` luôn bằng `0`;
- payload không có NUL terminator;
- không gửi trực tiếp `struct C` qua socket.

`send_all()` và `recv_exact()` xử lý partial send/receive, `EINTR` và
disconnect. `decode_header()` từ chối magic, version, reserved hoặc
`payload_length` không hợp lệ. Nếu frame hỏng hoặc payload bị ngắt giữa chừng,
server ghi lỗi và đóng kết nối; server không cố trả `ERROR` bằng một header
không đáng tin cậy.

## Message type và payload

| Message | Mã | Request payload | Response |
|---|---:|---|---|
| `CONNECT` | `0x01` | Tên participant hoặc rỗng | `CONNECT_RESULT` |
| `CONNECT_RESULT` | `0x02` | — | `OK|<participant-id>|<token>` |
| `RECONNECT` | `0x03` | `<participant-id>\|<token>` | `RECONNECT_RESULT` |
| `RECONNECT_RESULT` | `0x04` | — | `OK|<participant-id>|<token>` |
| `LIST_ROOMS` | `0x05` | Rỗng | `ROOM_LIST` |
| `ROOM_LIST` | `0x06` | — | Mỗi dòng `<room-id>|<room-name>` |
| `CREATE_ROOM` | `0x07` | Tên phòng | `CREATE_ROOM_RESULT` |
| `CREATE_ROOM_RESULT` | `0x08` | — | `OK|<room-id>|<room-name>` |
| `ERROR` | `0x14` | — | Mã lỗi dạng text |

Mọi response ứng với request đều echo đúng `request_id`. Message tự gửi
không có trong phiên bản hiện tại; nếu có thì dùng `request_id = 0`.

Các mã lỗi chính gồm:

- `NOT_CONNECTED`
- `ALREADY_CONNECTED`
- `INVALID_PARTICIPANT_NAME`
- `INVALID_RECONNECT`
- `PARTICIPANT_LIMIT_REACHED`
- `ROOM_LIMIT_REACHED`
- `ROOM_NAME_EXISTS`
- `INVALID_ROOM_NAME`
- `UNEXPECTED_PAYLOAD`
- `UNKNOWN_MESSAGE_TYPE`
- `INTERNAL_ERROR`

Tên participant và tên phòng phải là ASCII in được, không chứa `|`, CR hoặc
LF, và phải nằm trong giới hạn buffer được định nghĩa ở `auction_state.h`.

## Giới hạn và lưu ý bảo mật

- Tối đa 64 participant và 32 phòng trong một tiến trình.
- Participant offline không bị xóa để hỗ trợ reconnect; khi đạt giới hạn
  participant mới, server từ chối participant mới cho tới khi restart.
- Token 32 ký tự hex được tạo từ `/dev/urandom`, chỉ giữ trong RAM và không có
  thời hạn.
- Log có thể chứa tên participant, tên phòng và payload ứng dụng; cần bảo vệ
  quyền đọc file log. Token reconnect được redacted nhưng log vẫn có thể chứa
  dữ liệu hoạt động.
- Đây là TCP raw, không có TLS. Token là credential của phiên, chỉ phù hợp
  cho bài thực hành local, không phải cơ chế xác thực production.
- Server dùng `poll()` và buffer riêng cho từng client, nên một client gửi dở
  frame không chặn các client khác. Một lần `recv()` có thể chứa nhiều frame;
  server tách và xử lý lần lượt từng frame hoàn chỉnh.
- Giới hạn hiện tại là 64 client đồng thời; kết nối vượt giới hạn bị từ chối.
- Chưa có multithreading. Payload phản hồi tối đa 4096 byte được gửi đồng bộ;
  ứng dụng tải lớn hơn cần thêm output queue và theo dõi `POLLOUT`.

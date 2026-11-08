# Online Auction — G3 & G4

Triển khai này mở rộng TCP Protocol V1 của G2 với:

- **G3:** participant, `CONNECT`, session token và `RECONNECT`.
- **G4:** danh sách nhiều phòng đấu giá trong bộ nhớ và tạo phòng mới.

Phần này chưa làm `JOIN`/`LEAVE`, đặt giá, timer hoặc phục vụ đồng thời nhiều
client; các nội dung đó thuộc G5–G9.

## Biên dịch và kiểm thử

Project dùng POSIX sockets, vì vậy hãy chạy trên Linux hoặc WSL có `gcc` và
`make` (ví dụ: `sudo apt install build-essential`).

```bash
make
make test
```

Các lệnh `make server`, `make client` và `make clean` cũng được hỗ trợ.

## Chạy

Mở hai terminal trong thư mục project:

```bash
./server
./client Alice
```

Client sẽ in kết quả dạng sau. Lưu lại participant ID và token để reconnect.

```text
CONNECT_RESULT: OK|P001|<session-token>
```

Sau khi client đầu tiên thoát, server vẫn giữ participant trong bộ nhớ. Dùng
lệnh sau để nối lại phiên đó:

```bash
./client --reconnect P001 <session-token>
```

`RECONNECT_RESULT` trả lại cùng token. Giữ token này để có thể kết nối lại
ngay cả khi phản hồi bị mất do mạng gián đoạn.

Trong client đã kết nối, các lệnh hiện có là:

```text
LIST_ROOMS
CREATE_ROOM Sunday Auction
exit
```

Server tạo sẵn hai phòng `R001` và `R002`; mỗi `CREATE_ROOM` hợp lệ tạo phòng
mới với ID tăng dần. Dữ liệu participant, token và phòng sẽ mất khi server
khởi động lại.

`./server` dùng port `8080` mặc định. Server cũng nhận một đối số port tùy chọn
cho integration test độc lập (ví dụ `./server 25000`); client CLI mặc định vẫn
kết nối port `8080`.

## Protocol V1

Mỗi message có header 16 byte, theo thứ tự byte:

```text
magic (2) | version (1) | type (1) | request_id (4) |
payload_length (4) | reserved (4)
```

Các số nhiều byte dùng big-endian. Payload không có NUL terminator, tối đa
4096 byte. `protocol.c` xử lý partial `send`/`recv`, `EINTR`, disconnect và
kiểm tra `magic`, `version`, `reserved`, giới hạn payload.

| Type | Mã | Request payload | Response payload |
|---|---:|---|---|
| `CONNECT` | `0x01` | rỗng hoặc tên participant | `CONNECT_RESULT`: `OK|<participant-id>|<token>` |
| `RECONNECT` | `0x03` | `<participant-id>|<token>` | `RECONNECT_RESULT`: `OK|<participant-id>|<token>` |
| `LIST_ROOMS` | `0x05` | rỗng | `ROOM_LIST`: mỗi dòng là `<room-id>|<room-name>` |
| `CREATE_ROOM` | `0x07` | tên phòng | `CREATE_ROOM_RESULT`: `OK|<room-id>|<room-name>` |
| `ERROR` | `0x14` | — | mã lỗi dạng text, echo `request_id` |

Tên participant/tên phòng phải là ASCII in được, dài trong giới hạn của source
code, và không được chứa `|`, CR hoặc LF. Session token là credential của phiên:
không chia sẻ token cho người khác. Đây là raw TCP không có TLS và client nhận
token qua command line khi reconnect, nên chỉ dùng cho thực hành cục bộ; không
phải cơ chế xác thực production.

## Thiết kế G3/G4

- `auction_state.c` quản lý tối đa 64 participant và 32 phòng.
- Participant không bị xoá khi ngắt kết nối để có thể reconnect. Sau 64 lần
  `CONNECT` tạo participant mới, server từ chối participant mới cho đến khi
  khởi động lại; kết nối lại participant cũ vẫn được hỗ trợ.
- `CONNECT` cấp ID theo dạng `P001`, token 32 ký tự hex và đánh dấu participant
  đang kết nối.
- Khi socket đóng, participant được chuyển offline nhưng không bị xoá;
  `RECONNECT` hợp lệ dùng lại đúng participant đó.
- Mỗi phòng có ID `R001`..., tên và participant tạo phòng. Tên phòng trùng bị
  từ chối để danh sách không mơ hồ.
- Server vẫn tuần tự theo phạm vi trước G9; state được giữ qua các kết nối nối
  tiếp trong cùng một tiến trình server. Một client gửi dở header/payload có thể
  giữ server chờ; G9 sẽ thay bằng cơ chế phục vụ nhiều client đồng thời.
- Token chỉ tồn tại trong bộ nhớ và được tạo từ `/dev/urandom`. Token chưa có
  thời hạn và không được thay mới, nên chỉ phù hợp cho bài thực hành local.

## Kiểm thử

`make test` chạy:

- `tests/test_protocol.c`: header, encode/decode và round-trip frame.
- `tests/test_auction_state.c`: CONNECT, disconnect/reconnect, token sai và
  nhiều phòng/ID phòng.
- `tests/test_server_integration.c`: server thật, phân quyền trước CONNECT,
  RECONNECT hợp lệ/không hợp lệ, echo `request_id`, tạo và liệt kê phòng.

# Online Auction - G3 đến G8

Đây là chương trình TCP socket bằng C trên Linux, biên dịch bằng GCC. Bản
hiện tại mở rộng Protocol V1 với:

- **G3:** participant, `CONNECT`, session token và `RECONNECT`.
- **G4:** danh sách phòng đấu giá trong bộ nhớ và `CREATE_ROOM`.
- **G5:** JOIN_ROOM, LEAVE_ROOM, membership hai chiều và room state.
- **G6:** PLACE_BID, BID_RESULT, BID_UPDATE và bid history trong RAM.
- **G7:** timer phiên đấu giá, cảnh báo thời gian và kết quả.
- **G8:** kiểm thử framing TCP, timer, luồng đấu giá và xử lý lỗi.

Server dùng `poll()` để phục vụ nhiều client trong một tiến trình; request và
timer được xử lý tuần tự trong event loop. Dữ liệu chưa được lưu lâu dài.

## Cấu trúc mã nguồn

- `common.h`: hằng số protocol, port và message type.
- `protocol.h`, `protocol.c`: đóng gói/giải mã frame TCP và xử lý partial
  I/O.
- `auction_state.h`, `auction_state.c`: quản lý participant, token và phòng
  trong bộ nhớ, bao gồm membership và `current_room_id`.
- `auction.h`, `auction.c`: business logic đặt bid, giá tối thiểu, auction
  state và bid history.
- `server.c`: socket server, xác thực request và điều phối state.
- `client.c`: client tương tác dòng lệnh, hỗ trợ connect/reconnect và các lệnh
  phòng.
- `logger.h`, `logger.c`: log append-only cho message gửi/nhận và sự kiện lỗi.
- `Makefile`: build chương trình và test.
- `tests/`: test protocol, state, timer, logger và server integration.

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

`make test` kiểm tra encode/decode, TCP frame bị chia/gộp, ngắt giữa frame,
participant, JOIN/LEAVE, đặt giá, đồng bộ session/auction, reconnect takeover,
timer, kết quả và che token trong log. Test integration khởi chạy server trên
port được hệ điều hành cấp và kết nối nhiều client thật; kết quả G7 gồm trường
hợp có người thắng, `NO_BID`, bid sau hạn và reconnect sau khi kết thúc.

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

Thời gian đấu giá mặc định của mỗi phòng là 60000 ms, tính từ khi server tạo
phòng. Để thử nhanh, truyền thêm thời gian (1000–3600000 ms) sau port:

```bash
./server 8080 15000
```

Hai phòng mặc định bắt đầu đếm giờ khi server khởi động; phòng mới bắt đầu khi
được tạo. Hết giờ, server kết thúc phiên một lần, giữ người dẫn đầu và giá hợp
lệ của G6, rồi thông báo kết quả cho các thành viên đang kết nối. Nếu chưa có
bid, kết quả là `NO_BID`. Phòng đã kết thúc không nhận thêm bid hoặc thành viên
mới; hiện chưa có lệnh mở lại phiên. Trạng thái chỉ tồn tại trong RAM.

Server gửi `TIME_UPDATE` khoảng mỗi giây và cảnh báo khi còn không quá 10 giây.
Thành viên vừa JOIN hoặc RECONNECT được gửi thời gian hiện tại; nếu phiên đã kết
thúc, participant reconnect vào membership cũ được gửi lại kết quả. Sau CONNECT
hoặc RECONNECT, client gọi `GET_SESSION_STATE`; nếu đang ở phòng, client gọi tiếp
`GET_AUCTION_STATE` để khôi phục phòng, giá, người dẫn đầu, trạng thái và thời
gian còn lại. Vì vậy người JOIN muộn không phải chờ bid tiếp theo mới biết giá.

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

Hoặc dùng menu tương tác:

```text
===== ONLINE AUCTION CLIENT =====
1. List rooms
2. Create room
3. Join room
4. Leave room
5. View room state
6. Place bid
7. View current room
8. Reconnect help
9. Exit
```

Khi join thành công, server trả `JOIN_RESULT` rồi `ROOM_STATE`. Client chỉ
cập nhật room local sau khi nhận `OK|<room-id>`; server là nguồn dữ liệu
authoritative.

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
trình đang chạy. Toàn bộ payload của `CONNECT_RESULT`, `RECONNECT` và
`RECONNECT_RESULT` được che thành `<REDACTED>`, kể cả payload sai định dạng.
Lỗi gửi/nhận và
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
| `JOIN_ROOM` | `0x09` | `<room-id>` | `JOIN_RESULT` |
| `JOIN_RESULT` | `0x0A` | — | `OK|<room-id>` hoặc `FAIL|<reason>` |
| `LEAVE_ROOM` | `0x0B` | `<room-id>` | `LEAVE_RESULT` |
| `LEAVE_RESULT` | `0x0C` | — | `OK|<room-id>` hoặc `FAIL|<reason>` |
| `GET_ROOM_STATE` | `0x0D` | `<room-id>` | `ROOM_STATE` |
| `ROOM_STATE` | `0x0E` | — | `<room-id>\|<name>\|<OPEN/ENDED>\|<participant-list>\|<count>` |
| `PLACE_BID` | `0x15` | `<room-id>\|<auction-id>\|<amount>` | `BID_RESULT` |
| `BID_RESULT` | `0x16` | — | `OK\|<room-id>\|<amount>` hoặc `FAIL\|<reason>[\|<minimum>]` |
| `BID_UPDATE` | `0x17` | — | `<room-id>\|<auction-id>\|<new-price>\|<leader-id>`; push `request_id=0` |
| `GET_SESSION_STATE` | `0x18` | Rỗng | `SESSION_STATE` |
| `SESSION_STATE` | `0x19` | — | `OK\|<room-id>`; `<room-id>` là `-1` nếu chưa JOIN |
| `GET_AUCTION_STATE` | `0x1A` | `<room-id>` | `AUCTION_STATE`; chỉ member của phòng |
| `AUCTION_STATE` | `0x1B` | — | `<room-id>\|<auction-id>\|<status>\|<price>\|<leader>\|<remaining-ms>\|<starting-price>\|<min-increment>\|<bid-count>` |
| `TIME_UPDATE` | `0x10` | — | `<room-id>\|<auction-id>\|<remaining-ms>`; push `request_id=0` |
| `TIME_WARNING` | `0x11` | — | `<room-id>\|<auction-id>\|<seconds>`; push `request_id=0` |
| `AUCTION_RESULT` | `0x13` | — | `<room-id>\|<auction-id>\|SOLD\|<winner-id>\|<price>` hoặc `<room-id>\|<auction-id>\|NO_BID`; push `request_id=0` |
| `ERROR` | `0x14` | — | Mã lỗi dạng text |

Mọi response ứng với request đều echo đúng `request_id`; thông báo server chủ
động gửi dùng `request_id = 0`.

0x07/0x08 đã được giữ cho G4 `CREATE_ROOM/CREATE_ROOM_RESULT`. Vì vậy G5 dùng
0x0D/0x0E mới cho `GET_ROOM_STATE/ROOM_STATE`, không làm thay đổi semantic G4
và không dùng lại mã hiện có. JOIN/LEAVE dùng đúng 0x09–0x0C. G6 dùng mã mới
0x15/0x16/0x17 để tránh xung đột 0x0D/0x0E của room-state. Các message đồng bộ
0x18–0x1B là phần mở rộng tương thích ngược; mã cũ không bị đổi.

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
- `ROOM_NOT_FOUND`
- `ALREADY_IN_ROOM`
- `ROOM_ENDED`
- `ROOM_FULL`
- `NOT_IN_ROOM`
- `ROOM_MISMATCH`
- `INVALID_ROOM_ID`
- `INVALID_AMOUNT`
- `AUCTION_NOT_FOUND`
- `AUCTION_NOT_ACTIVE`
- `PRICE_TOO_LOW`
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
- Chưa có multithreading. Mỗi client có hàng đợi gửi giới hạn và `POLLOUT`;
  khi hàng đợi đầy hoặc không tiến triển trong 15 giây, server ngắt client chậm.
- Frame nhận dở quá 15 giây và socket chưa xác thực quá 30 giây bị đóng để tránh
  chiếm slot. Kết nối đã xác thực và đang idle không bị timeout.

## G5 - JOIN_ROOM / LEAVE_ROOM

- Một participant chỉ tham gia tối đa một room; `participant.current_room_id`
  dùng `NO_ROOM` là `-1`.
- `JOIN_ROOM` xác định participant theo socket/session, không tin room hoặc
  participant ID do payload của client khai báo.
- Room lưu `participant_ids[]`, `participant_count` và trạng thái `ended`.
- JOIN thành công cập nhật đồng thời room membership và
  `participant.current_room_id`, rồi gửi `JOIN_RESULT` và `ROOM_STATE`.
- LEAVE chỉ xóa membership; participant không bị xóa khỏi Participant Manager,
  bid history chưa tồn tại ở G5 và room không tự kết thúc.
- Disconnect không phải leave: membership và `current_room_id` được giữ. Khi
  reconnect, socket mới được gắn vào participant cũ. Token hợp lệ có thể thay
  thế socket cũ còn half-open; server tách socket cũ trước khi kích hoạt socket
  mới để không làm mất trạng thái `connected`.
- Room có tối đa `32` participant (`MAX_PARTICIPANTS_PER_ROOM`).

## G6 - PLACE_BID / BID_UPDATE

- Mỗi room có một auction `A001` mặc định ở trạng thái `RUNNING`.
- Giá khởi điểm là `100000`, bước giá tối thiểu là `10000`.
- Bid phải là số nguyên dương `int64_t`, không nhận ký tự, dấu âm hoặc overflow.
- Giá tối thiểu là starting price khi chưa có bid; sau đó là
  `current_price + min_increment`.
- Bid thành công cập nhật state trước, ghi `bid_history`, gửi `BID_RESULT`
  cho bidder, rồi broadcast `BID_UPDATE` tới tất cả member trong cùng room.
- BID_UPDATE được gửi cho cả bidder và các member khác trong room. Client
  dùng `request_id = 0`; BID_RESULT echo request ID của PLACE_BID.
- Chỉ client đang connected, đang join room và nằm trong danh sách member mới
  nhận update. Client ở phòng khác hoặc client đã disconnect không nhận.
- Mỗi auction lưu tối đa `128` bid record. Khi đầy, server trả `SERVER_ERROR`
  thay vì thay đổi giá nếu chưa ghi được history.
- Disconnect không reset giá hoặc highest bidder; G7 giữ kết quả của bid hợp lệ
  khi timer kết thúc.
- `GET_AUCTION_STATE` chỉ cho member đúng phòng và trả snapshot authoritative,
  dùng cho JOIN muộn và RECONNECT; client phòng khác nhận `ROOM_MISMATCH` hoặc
  `NOT_IN_ROOM`.

## G7 - timer và kết thúc đấu giá

- Mỗi room có deadline monotonic độc lập; hết hạn được kiểm tra trong event loop
  và ngay trước khi xử lý bid, nên bid đến sau deadline luôn bị từ chối.
- Server push `TIME_UPDATE`, gửi một `TIME_WARNING` khi còn tối đa 10 giây và
  chỉ phát một `AUCTION_RESULT` (`SOLD` hoặc `NO_BID`).
- Room đã `ENDED` giữ nguyên kết quả trong RAM; member reconnect được phát lại
  kết quả và có thể lấy snapshot với thời gian còn lại bằng `0`.

## G8 - kiểm thử và xử lý lỗi

- Sáu nhóm test bao phủ protocol framing, state G3–G6, timer, multi-client TCP,
  reconnect/session snapshot, late JOIN, access control và redaction của logger.
- Server dùng input/output buffer riêng, giới hạn công việc mỗi vòng `poll()`,
  xử lý partial/coalesced frame và cách ly client gửi header sai hoặc bị treo.
- Ngoài build cảnh báo chuẩn, mã đã được kiểm tra bằng GCC `-fanalyzer` và toàn
  bộ test chạy với AddressSanitizer + UndefinedBehaviorSanitizer.

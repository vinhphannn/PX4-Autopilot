# Kế hoạch nghiên cứu RX ESP + LoRa 433 MHz theo ELRS

## Trạng thái

Ý tưởng và kế hoạch nghiên cứu. Chưa mua phần cứng và chưa thay đổi firmware
trong repository PX4 này.

> **Cập nhật 2026-09-25 — quyết định quan trọng:** 433 MHz không còn là hướng
> phần cứng/target được ExpressLRS Configurator hỗ trợ và kiểm thử chính thức.
> Upstream hiện tập trung vào 2.4 GHz, 900 MHz và dual-band. Mã nguồn còn dấu
> các regulatory-domain 433 cũ, nhưng không được hiểu là target được bảo trì.
> Vì vậy dự án này phải được xem là **local fork nghiên cứu**: tự build, tự
> kiểm thử RF và tự duy trì khi ELRS upstream thay đổi; không phải chọn một
> target có sẵn rồi flash.

Mục tiêu là làm một RX ELRS **rời**, có thể thay thế dễ dàng, giao tiếp với FC
Aegis qua CRSF. Firmware ELRS thử nghiệm đã có trong Git cá nhân; khi bắt đầu
sẽ lấy đúng commit đang chạy được làm điểm xuất phát, audit lại cấu hình 433
và tạo target riêng thay vì viết lại giao thức từ đầu.

## Phạm vi và nguyên tắc

- RX không gộp vào FC v1 và không đặt sát ESC.
- Bản đầu dùng `ESP32-C3 + RA-02 (SX1278, 433 MHz)` đã quen thuộc.
- Dùng CRSF hai chiều, không ưu tiên SBUS.
- Không coi link 433 là đường truyền MAVLink/QGC băng thông cao. Nó phù hợp
  telemetry nhẹ: link statistics, pin, GPS, flight mode và cảnh báo.
- Không dùng prototype làm link duy nhất cho chuyến bay đầu tiên, hay cho chuyến
  bay ngoài tầm nhìn.
- Phải chọn băng tần, công suất phát và cấu hình vùng theo quy định tại nơi
  vận hành trước khi bay ngoài trời.

## Kiến trúc mục tiêu

```text
              USB-C / test pads
                     │
          ┌──────────┴──────────┐
          │      ESP32-C3       │  Wi-Fi + firmware ELRS
          │                     │
          │ UART (CRSF, 3.3 V)  ├──── TX/RX/GND ──── FC Aegis
          │ SPI + DIO           │
          └──────────┬──────────┘
                     │
               RA-02 / SX1278
                     │
              antenna 433 MHz

  FC 5 V ── bảo vệ nguồn ── LDO 3.3 V sạch ── ESP32-C3 + RA-02
```

`RA-02` đã là module RF hoàn chỉnh cho bước đầu: gồm SX1278, chuẩn tần số và
mạch RF cơ bản. Carrier PCB không tự thêm crystal cho SX1278. ESP32-C3 điều
khiển radio qua SPI; code được nạp vào ESP32-C3, không nạp vào SX1278.

## Giao tiếp với FC

| RX | FC | Ghi chú |
|---|---|---|
| `5V` | 5 V đã ổn áp | RX tự hạ xuống 3.3 V; không nhận VBAT trực tiếp. |
| `GND` | GND | Bắt buộc chung mass. |
| `TX` | UART RX của FC | CRSF, mức logic 3.3 V. |
| `RX` | UART TX của FC | Telemetry/cấu hình CRSF trả về RX. |

TX/RX phải nối chéo. Chọn một UART còn trống trên Aegis và cấu hình PX4 là
`CRSF`, không dùng đồng thời UART đang dành cho SBUS (`/dev/ttyS5`) nếu chưa
đổi rõ ràng cấu hình khởi động.

## Nạp firmware và debug

Ưu tiên ESP32-C3 vì có USB Serial/JTAG tích hợp:

- Đưa `GPIO19 = USB D+`, `GPIO18 = USB D-`, `5V` và `GND` ra USB-C hoặc test
  pads. Một cáp USB có thể nạp firmware, xem console và dùng JTAG debug.
- Giữ UART CRSF riêng cho FC khi RX hoạt động; không in log debug lên UART này
  trong lúc bay vì mọi byte lạ có thể làm hỏng gói CRSF.
- Bố trí test pads: `GND`, `3V3`, `EN/RESET`, `BOOT`, `ESP_TX`, `ESP_RX`,
  `D+`, `D-`. Các pads này cho phép cứu firmware khi USB bị cấu hình sai.
- Không cấp nguồn đồng thời từ USB và FC nếu chưa có mạch power-path chống
  cấp ngược.

## Bind và cấu hình ELRS

1. TX và RX phải dùng cùng firmware fork, cùng băng tần 433 MHz, cùng FHSS
   plan và cùng major version ELRS.
2. Dùng một `Binding Phrase` riêng, đủ dài, nạp vào cả TX và RX. Đây là cách
   bind chính; bật nguồn là tự kết nối.
3. Ghi lại: phiên bản firmware, target, vùng RF, packet rate, telemetry ratio,
   công suất và binding phrase trong nhật ký build của dự án.
4. Chỉ dùng bind thủ công ba lần power-cycle khi cần khôi phục; không dùng nó
   thay cho binding phrase trong cấu hình vận hành thường xuyên.

## Công việc firmware cần sở hữu

Đây là phần khiến 433 MHz trở thành dự án phát triển, không phải chỉ là PCB:

1. Tạo hardware target riêng cho ESP32-C3 + RA-02: chân SPI, các chân DIO,
   reset/LED, UART CRSF, nguồn và phương thức flash.
2. Khôi phục hoặc duy trì regulatory-domain/FHSS plan 433 trong source fork.
   Không dùng nguyên xi domain của quốc gia khác tại Việt Nam.
3. Build local thay vì phụ thuộc Configurator; lưu file cấu hình, toolchain,
   commit hash và binary phát hành của cả TX/RX.
4. Test bind phrase, packet mode, telemetry, dynamic power và failsafe qua
   power-cycle. Mỗi lần rebase/nâng ELRS upstream đều lặp lại bộ test này.
5. Khi upstream đổi cấu trúc config hoặc bỏ API SX127x, fork phải tự merge và
   sửa. Đây là chi phí duy trì dài hạn cần chấp nhận trước khi đặt tên nó là
   “RX dùng để bay”.

RA-02 là phần cứng 433 MHz thật nên không gặp lỗi "lấy board 868/915 rồi ép về
433"; các board đó thường có RF front-end filter và đường RF không thiết kế cho
433. Tuy nhiên, lợi thế phần cứng này không thay thế việc xác nhận emission,
sensitivity và tính hợp pháp của link.

## Cấu hình radio giả định để kiểm chứng

| Mục tiêu | Packet rate | Switch mode | Telemetry | Nhận xét |
|---|---:|---|---|---|
| Bay quad thông thường | 200 Hz | Hybrid/Wide | 1:16 | Mục tiêu test: stick có chu kỳ 5 ms; ưu tiên điều khiển. |
| Cần telemetry nhanh hơn | 200 Hz | Hybrid/Wide | 1:8 | Đủ tốt cho trạng thái pin/GPS, đổi lại ít dư địa hơn. |
| Cần 16 kênh analog 10-bit | 100 Hz Full | 16ch Rate/2 | 1:16 | Mọi kênh cập nhật 50 Hz; không phải preset ưu tiên cho quad. |

Các giá trị trên là mục tiêu dựa trên mode SX127x sub-GHz cũ của ELRS. Chỉ được
coi là khả dụng sau khi target fork build, TX/RX bind và kiểm thử over-the-air
xác nhận. Không thể có 16 kênh full-resolution cùng 200 Hz với RA-02. Với
quad, 200 Hz Hybrid/Wide thường hữu ích hơn: stick và arm phản hồi nhanh, các
AUX cho mode/RTL/kill vẫn đủ dùng.

Telemetry ở 200 Hz không tương đương đường truyền serial 200 Hz. Ví dụ tỷ lệ
`1:16` chỉ có khoảng 12.5 lượt telemetry/giây; phù hợp dữ liệu bay nhẹ, không
phù hợp parameter download, mission upload lớn hay video. UART CRSF nhanh hơn
RF không làm RF có thêm băng thông.

## Quyết định PCB và layout

### Bản prototype

- Dùng PCB 2 lớp với RA-02 module được chấp nhận.
- Đổ mass liên tục trên mặt còn lại, không cắt ground plane dưới module và
  đường antenna.
- Không đặt buck, đường DShot, ESC, dây VBAT hay GPS antenna cạnh antenna
  433 MHz.
- Antenna rời và đầu nối cơ khí chắc chắn; thử antenna đúng dải 433 MHz,
  không phát khi chưa gắn antenna.

### Bản hoàn thiện

- Nếu chuyển sang radio 2.4 GHz rời, thêm PA/LNA, diversity hoặc RF matching
  tự thiết kế: chuyển sang 4 lớp.
- Tách nguồn RF/ESP bằng LDO sạch và decoupling sát chân module.
- Có bảo vệ đầu vào 5 V, chống cấp ngược và giới hạn xung; không đưa VBAT vào
  RX.
- Có LED trạng thái và nút/Pad BOOT/RESET thật sự truy cập được sau khi lắp
  lên khung.

## Danh sách phần cần chuẩn bị

- ESP32-C3 module.
- RA-02 SX1278 433 MHz và antenna/đầu nối đúng băng tần.
- LDO 3.3 V, tụ decoupling theo datasheet module, mạch lọc/bảo vệ đầu vào 5 V.
- USB-C và điện trở CC (nếu dùng cổng USB-C), hoặc tối thiểu test pads USB.
- Header/test pads cho CRSF, BOOT, RESET, UART cứu hộ và đo 3.3 V/GND.
- Vỏ/heat-shrink và gá antenna để dây antenna không bị cánh quạt cắt.

## Lộ trình kiểm chứng

0. **Firmware fork:** hoàn thành target RX/TX 433, lưu commit và binary; bind
   được trên bàn trước khi vẽ PCB hoàn thiện.
1. **Bring-up nguồn:** kiểm tra cực tính, 3.3 V, dòng nghỉ và không nóng bất
   thường; chưa nối FC/ESC.
2. **Flash/debug:** nạp lại được bằng USB; console/JTAG hoạt động; khôi phục
   được từ BOOT/RESET pads.
3. **Radio:** bind phrase, LQ/RSSI, telemetry link statistics, power-cycle
   nhiều lần và test failsafe.
4. **CRSF với PX4:** PX4 nhận đủ stick, arm/mode/kill đúng kênh; mất link phải
   kích hoạt đúng hành vi an toàn. Thử với prop đã tháo.
5. **Nhiễu:** cho FC, DShot và motor chạy trên bàn an toàn; theo dõi LQ/RSSI,
   reset RX và lỗi UART. Không cầm drone khi motor/prop đang chạy.
6. **Tầm xa mặt đất:** kiểm tra line-of-sight, antenna orientation và telemetry
   trước; ghi lại packet rate, công suất, LQ thấp nhất và failsafe. Chỉ bay sau
   khi kết quả lặp lại ổn định.

## Tiêu chí sẵn sàng để bay thử

- Không reset/brownout khi cắm-rút nguồn hoặc khi DShot/motor hoạt động.
- Failsafe được kiểm chứng trên mặt đất ở cả mất TX và mất nguồn RX.
- CRSF không có byte debug xen vào luồng điều khiển.
- Antenna được cố định và không nằm trong vùng cánh quạt/ESC.
- Có một RX thương mại hoặc link đã kiểm chứng làm phương án dự phòng.
- Đã ghi rõ đây là firmware fork tự duy trì, không có giả định hỗ trợ từ
  ExpressLRS Configurator/upstream.

## Tài liệu tham khảo

- [ExpressLRS: DIY Receiver](https://www.expresslrs.org/hardware/special-targets/diy-rx/)
- [Thảo luận ELRS về 433 MHz và giới hạn front-end 868/915](https://github.com/ExpressLRS/ExpressLRS/discussions/2879)
- [ExpressLRS: packet rate và telemetry ratio](https://www.expresslrs.org/quick-start/transmitters/lua-howto/)
- [ExpressLRS: switch modes và 16 kênh](https://www.expresslrs.org/software/switch-config/)
- [ExpressLRS: telemetry bandwidth](https://www.expresslrs.org/info/telem-bandwidth/)
- [Espressif: USB Serial/JTAG trên ESP32-C3](https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/usb-serial-jtag-console.html)

# Aegis FC V1 — ghi chép bring-up và định hướng thiết kế V2

Tài liệu này lưu lại cấu hình đã được xác nhận trên phần cứng Aegis FC V1,
những giới hạn thực tế phát hiện trong quá trình bring-up, và các yêu cầu nên
được đưa vào thiết kế V2. Các lệnh, tham số PX4 và số liệu đo được giữ nguyên
để có thể đối chiếu khi debug hoặc thử nghiệm lại.

## Cấu hình nền đã chấp nhận cho V1

BMI088 hoạt động ổn định trên SPI2 với cấu hình sau:

- Xung SPI: **1 MHz**
- Thứ tự khởi động: **gyro trước, accelerometer sau**
- Đường dữ liệu: **thăm dò FIFO bằng SPI2 DMA**; ngắt DRDY đang tắt
- Hướng cảm biến: `ROTATION_YAW_180` (`bmi088 ... -R 4`)

Cấu hình này làm khung tọa độ mặc định của cảm biến khớp với hướng IMU đã lắp
trên mạch. Không đồng thời đặt yaw rotation cho board/cảm biến trong
QGroundControl. Sau khi chuyển từ cấu hình cũ `-R 6`, cần xóa và thực hiện lại
calibration accelerometer và level.

### Số liệu quan sát ở 1 MHz

Nhiều lần cold boot, cắm lại USB và soak test kéo dài đều chạy bình thường.
Thử nghiệm DMA cuối dùng bộ đệm SPI2 DMA 2048 byte cho mỗi chiều:

- FIFO thô accelerometer: **1600 Hz** (`2` mẫu mỗi lần truyền, `dt = 625 us`)
- FIFO gyroscope: khoảng **1998 Hz**
- control allocator và rate control: **666.7 Hz**; DShot: khoảng **662 Hz**
- không có lỗi register hoặc transfer
- bộ đếm FIFO overflow/reset lúc khởi động giữ nguyên trong soak DMA sáu phút;
  một lần gyro FIFO rỗng không gây data gap hay reset
- CPU idle: khoảng **62.5%**; work queue SPI2: khoảng **12.2%** CPU

Phiên bản PIO trước đó dùng khoảng 61% CPU trong `wq:SPI2`, CPU idle chỉ khoảng
16%. DMA giảm khoảng 80% tải CPU của SPI2 mà vẫn giữ tần số lấy mẫu và điều
khiển nền. Đây là baseline hiện tại cho flight control.

## Các thử nghiệm biên độ xung SPI

| Xung SPI | Kết quả | Quyết định |
| --- | --- | --- |
| 1 MHz | Đúng tần số đầu ra; ổn định khi soak và cắm lại USB | Chấp nhận |
| 2 MHz | Driver còn sống, nhưng chỉ còn khoảng 659 Hz accel / 1340 Hz gyro; mỗi FIFO có thêm overflow/reset | Loại |
| 8 MHz | BMI088 biến mất, QGroundControl báo thiếu accel/gyro | Loại |
| 10 MHz | Probe trước đó không đọc chip ID một cách tin cậy | Loại |

`bad transfer: 0` chỉ xác nhận giao dịch SPI từ phía MCU đã hoàn tất; nó không
chứng minh các byte FIFO trả về là hợp lệ. Việc giảm tần số vĩnh viễn ở 2 MHz
và mất cảm biến ở 8 MHz vì vậy được coi là vấn đề về biên độ/tính toàn vẹn tín
hiệu SPI, không phải là một cải thiện hiệu năng.

## Tối ưu phần mềm đã loại

Giảm polling xuống 400 Hz accel / 333 Hz gyro và đọc batch FIFO lớn hơn chỉ hạ
tải SPI2 từ khoảng 61% xuống 52%, nhưng cũng kéo rate control và DShot từ
khoảng 667 Hz xuống khoảng 333 Hz. Hướng này bị loại. Giải pháp được chấp nhận
là polling ở cadence bình thường cùng DMA tại 1 MHz.

## Những điểm đã làm V1 hoạt động

1. Polling tránh cấu hình ngắt DRDY chưa được hỗ trợ/ổn định trên board này.
2. Khởi động gyro trước accelerometer tạo trình tự bring-up SPI lặp lại được.
3. Chip-select của accelerometer nằm ở PC15. Firmware tắt LSE một cách chủ
   đích để PC15 có thể dùng làm GPIO.
4. SPI2 DMA tại 1 MHz sử dụng mapping DMA1/DMAMUX SPI2 RX/TX của board và đã
   được kiểm chứng cùng DShot trên phần cứng.
5. Driver có phần xử lý FIFO/recovery BMI088 dành riêng cho quá trình bring-up.
   Giữ phần này cho đến khi có bản gọn hơn đã được regression-test độc lập.

Chẩn đoán brown-out ban đầu chỉ là một **giả thuyết**, chưa phải nguyên nhân đã
được chứng minh. Hoạt động ổn định lâu dài tại 1 MHz cho thấy giả thuyết đó
không đủ để giải thích chính; sự phụ thuộc theo tần số đo được là bằng chứng
mạnh hơn về biên độ điện của SPI hạn chế và/hoặc trạng thái chip-select khi
reset.

## Checklist phần cứng cho V2

- Không đưa chân CS BMI088 vào PC15/OSC32_OUT. Dùng GPIO thông thường để không
  phải tắt LSE.
- Tạo trạng thái reset/power-up xác định cho **cả hai** net CS BMI088. Chừa
  footprint điện trở kéo gần cảm biến và chọn chiều/giá trị theo datasheet
  BMI088 cùng phép đo oscilloscope; không để CS floating khi MCU reset.
- Giữ trace SPI2 ngắn và có mặt phẳng mass liên tục. Chừa footprint điện trở
  series tại nguồn cho SCK và MOSI, sau đó chọn giá trị bằng đo đạc.
- Đưa cả hai net DRDY về chân MCU có EXTI rõ ràng, không xung đột, có test pad.
  Chỉ bật DRDY sau khi kiểm tra mapping điện và timing interrupt.
- Thêm test pad cho SPI, CS và một điểm mass gần đó để có thể kiểm tra
  SCK/MOSI/MISO/CS bằng oscilloscope hoặc logic analyzer.
- Khi phát triển board, luôn thể hiện rõ bộ đệm/cache DMA. DMA giảm tải CPU và
  jitter, nhưng không thể tăng ODR vật lý của BMI088.

### Routing actuator, DShot và bDSHOT

V1 xác nhận tám output vẫn chạy DShot thường, nhưng số jack vật lý bị xen kẽ
theo timer. Mapping đo được là QGC `MAIN 1…8` sang jack vật lý
`MAIN 1, 5, 2, 6, 3, 7, 4, 8`. PX4 yêu cầu channel dùng chung một timer phải
đứng liền nhau trong `timer_config.cpp`, nên không thể sửa sự xen kẽ đó một
cách sạch bằng cách chỉ đảo thứ tự firmware.

- Route bốn jack motor chính `MAIN 1…4` vào bốn channel liên tiếp của **một
  timer DMA-capable** (ví dụ TIM1 CH1…CH4), và để đúng thứ tự physical
  `MAIN 1, MAIN 2, MAIN 3, MAIN 4`.
- Bố trí net, connector pinout, silkscreen, `timer_config.cpp` và QGC actuator
  test cùng một thứ tự. Khi kích `MAIN n` trong QGC, chính jack in `MAIN n`
  phải phát tín hiệu.
- Chừa bốn output motor chính trên cùng timer để DShot và bDSHOT/RPM telemetry
  có đường nâng cấp đơn giản, không buộc người dùng cắm ESC 4-in-1 vào các jack
  cách xa nhau.
- Các output phụ có thể thuộc timer khác, nhưng từng nhóm timer phải liên tiếp
  và được ghi rõ trong schematic. Không dùng group phụ xen vào giữa bốn motor
  chính.
- Fixture sản xuất phải chạy lần lượt actuator test `MAIN 1…8`, xác nhận đúng
  số jack, tần số DShot và nếu có, chiều telemetry/RPM trước khi board xuất xưởng.

## Thứ tự kiểm chứng firmware trong tương lai

Mỗi build chỉ đổi một biến và luôn giữ cấu hình 1 MHz làm điểm quay lại:

1. Xác nhận baseline DMA 1 MHz bằng cold boot lặp lại, cắm lại USB và soak lâu
   hơn.
2. Nếu phần cứng V2 vượt qua đo tín hiệu, tăng xung từng bước trong khi kiểm
   tra tần số cảm biến và bộ đếm FIFO.
3. Thử DRDY gyro trước và xác nhận độc lập trước khi xem xét DRDY accel.

Tiêu chí đạt cho mọi thí nghiệm: đúng tần số đầu ra, không tăng `bad transfer`,
`bad register`, FIFO overflow/reset, USB/QGroundControl ổn định và cold boot
lặp lại được.

## Bài học V2 từ toàn bộ quá trình bring-up FC

Các nội dung dưới đây là yêu cầu thiết kế rút ra từ board thực, không chỉ là
tính năng “có thì tốt”. Giải quyết chúng từ schematic và placement V2 sẽ rẻ và
an toàn hơn nhiều so với việc duy trì workaround firmware đặc thù V1 mãi mãi.

### Hành vi reset, nguồn và chip-select

`reboot` của MCU, reset bootloader/cập nhật firmware, gắn USB/SWD và thật sự
cắt nguồn board là các trạng thái khác nhau. Khi chỉ reset MCU, BMI088 vẫn có
nguồn, giữ trạng thái và có thể tích đầy FIFO. Vì vậy backlog FIFO lớn lúc
khởi động ở V1 là hành vi có thể dự đoán trong trạng thái này, không tự động có
nghĩa là SPI lỗi.

- Tạo cách power-cycle rail IMU đã được mô tả rõ, ví dụ load switch điều khiển
  được, hoặc kiểm chứng chính thức đường reset khi vẫn giữ nguồn cảm biến.
- Kiểm tra USB, SWD và IO MCU khác không back-power rail cảm biến khi rail
  chính đã tắt. Thêm test point cho các rail.
- Lắp điện trở kéo CS active-low về trạng thái không chọn ở bên ngoài; không
  phụ thuộc pull-up nội yếu trong lúc reset hay bootloader chạy.
- Trước khi phát hành, thử cold power-up, `reboot`, reset flash/bootloader,
  cắm lại USB và gắn SWD khi IMU đang có nguồn.

### Yêu cầu với FIFO và cơ chế recovery

FIFO BMI088 là một stream gồm frame header, control và data. Sau MCU reset nó
có thể chứa mẫu cũ hợp lệ, và có thể lớn hơn batch uORB thông thường. Một
backlog hợp lệ phải được xả, không được nhầm là hỏng rồi reset lặp vô hạn.

- Đọc FIFO count và FIFO data riêng; chỉ parse frame hoàn chỉnh; loại count
  không thể có/đầy; rồi tiếp tục xả backlog hợp lệ ở các lần poll tiếp theo.
- Giữ cơ chế startup drain V1 cho đến khi có bản thay thế đã regression-test:
  nó đọc toàn bộ 1024 byte FIFO-data và xả lần cuối vì mẫu mới vẫn có thể tới
  trong lúc truyền ở 1 MHz.
- Chỉ recovery khi bad transfer kéo dài, ID/register không hợp lệ, hoặc mất mẫu
  hợp lệ liên tục; không recovery vì một lần FIFO rỗng hay backlog lúc start.
- Giữ các counter quan sát được: bad transfer, bad register, FIFO empty,
  overflow, reset và recovery.

Ở baseline V1 đã chấp nhận, không có bad register/transfer và không có overflow
tăng sau giai đoạn start là trạng thái khỏe. Một lần reset khi khởi tạo và vài
lần accel FIFO rỗng quanh thời điểm start là lành tính.

### DRDY và DMA

Polling kết hợp SPI2 DMA là baseline an toàn của V1. Nó hạ tải work queue SPI2
từ khoảng 61% trong thử nghiệm PIO trước đó xuống khoảng 12% trong thử nghiệm
DMA, đồng thời giữ acquisition ổn định. DRDY hiện được tắt có chủ đích vì
mapping điện và hành vi EXTI chưa được chứng minh trọn vẹn.

- Route cả hai đường DRDY đến chân EXTI sạch, có test pad, cực tính và điện trở
  kéo xác định.
- Chỉ bật DRDY sau khi đo dạng xung, tần số interrupt, cadence FIFO, latency và
  error counter dưới tải FC đầy đủ. Đây là tối ưu latency/polling, không phải
  điều kiện tiên quyết để có flight-control an toàn.
- Giữ DMA buffer cấp phát thường trực, cache-safe và đủ lớn cho giao dịch FIFO
  lớn nhất. Không quay lại stack buffer hoặc buffer nhỏ hơn kích thước transfer.

### Chọn cảm biến, orientation và bố trí nhiệt

- BMI160 trên SPI3 đã được xác định chắc chắn là chip ID `0xD1` và hoạt động
  như IMU thứ hai. BMI270 chưa từng được định danh chắc chắn trước khi thay ra,
  vì vậy V2 không nên kết luận nó hỏng hay software không tương thích. Cần xác
  nhận IMU/BOM được chọn bằng fixture kiểm tra sản xuất trước khi chốt BOM.
- Ghi trục vật lý và rotation mặc định PX4 trong schematic, silkscreen, tài liệu
  lắp ráp và test sản xuất. Rotation BMI088 V1 đã chấp nhận là yaw 180 (`-R 4`);
  board mới không nên cần người dùng chữa hướng bằng QGroundControl.
- Nhiệt độ die STM32 đã hiệu chỉnh khoảng 53 C trong setup này, IMU khoảng
  49--50 C. Đây không phải vấn đề an toàn silicon, nhưng có ảnh hưởng đến bias
  và barometer. Đặt cả hai IMU và barometer xa MCU/regulator/đường dòng lớn;
  cho barometer lỗ thông áp phù hợp và có thermal-soak trong kiểm tra calibration.

### Chế độ suy giảm của SD, USB, telemetry và power monitor

V1 hiện giữ MAVLink phản hồi khi không có SD nhờ RAM dataman. Có thể upload và
chạy mission trong chế độ đó, nhưng mission chủ ý bị mất khi mất nguồn; khi có
SD, mission vẫn persist bình thường.

- MAVLink, tải parameter và setup vehicle không được phụ thuộc vào SD. Không
  được format lặp lại khi card không có hoặc bị lỗi.
- Quyết định rõ V2 có cần mission persist khi không có SD hay không. Nếu có,
  dùng non-volatile storage trên board được quản lý đúng cách, không xem RAM là
  storage.
- Thêm card-detect/power handling chắc chắn và thử các trường hợp có, không có,
  gắn/tháo card. Thông báo rõ logging không khả dụng hay mission storage chỉ
  tạm thời.
- Hiện tượng USB/MAVLink đôi lúc khựng khi cầm board chưa chứng minh do SD gây
  ra. V2 cần thử nhiều cáp/host, độ chắc connector, ESD protection và EMI khi
  DShot/motor tải trước khi đóng vấn đề.
- Đấu và kiểm tra sớm sensing điện áp/dòng pin. Thông báo `system power
  unavailable` hiện tại là đúng khi chưa có power monitor hợp lệ, không được
  nhầm với lỗi IMU hay MAVLink.

## Checklist nghiệm thu V2 khi bring-up

| Ưu tiên | Phải chứng minh trước khi phát hành | Điều kiện đạt |
| --- | --- | --- |
| P0 | Cold boot IMU và mọi đường reset chỉ-MCU | Cả hai IMU publish không cần can thiệp shell, không recovery/reset lặp. |
| P0 | SPI khi DShot, MAVLink và SD logging chạy đồng thời | Không bad transfer/register hay FIFO overflow sau startup; biên độ waveform đã xác nhận. |
| P0 | Hành vi CS/rail qua USB, SWD và chuyển nguồn chính | Cảm biến không thể bị chọn hoặc back-power ngoài ý muốn. |
| P1 | SD có, không có, gắn và tháo | MAVLink/parameter vẫn phản hồi; persistence đúng như đã công bố. |
| P1 | Thermal soak ít nhất 30 phút | IMU/baro ổn định, bias drift có giới hạn, ghi lại nhiệt độ die và board. |
| P1 | Bản thử DRDY | Có lợi ích đo được nhưng không kém polling/DMA về timing, counter hay độ bền reset. |
| P2 | Fixture sản xuất | Xác nhận IMU ID, rail, SPI, DRDY, SD, USB, CAN/UART, PWM và orientation. |

## Roadmap kiến trúc hệ thống V2

V2 trước hết phải là một flight controller xuất sắc, không phải một payload
computer “all-in-one” đắt tiền. Kiến trúc được khuyến nghị là **flight-core
board cộng navigation/RF pod tháo rời**, liên kết chính qua CAN-FD và cấp bởi
rail 5 V sạch, được bảo vệ.

### Flight-core board

Giữ STM32, IMU dự phòng, barometer, power monitoring, SD, PWM/DShot, giao tiếp
RC và các bus tốc độ cao đã chứng minh ổn định trên core board. Đây là phần phải
boot, bay, log và còn điều khiển được ngay cả khi phụ kiện navigation hoặc
telemetry bị thiếu.

Chừa sẵn thay vì vội populate:

- hai cổng CAN-FD cho GNSS, smart peripheral, radar, range finder hoặc node
  payload tương lai;
- ít nhất hai UART dư với nguồn 5 V/3.3 V được bảo vệ, TX/RX và mass nằm cạnh
  từng cặp tín hiệu;
- một cổng I2C ngắn, cục bộ chỉ dành cho phụ kiện tần số thấp;
- tùy chọn Ethernet hoặc giao tiếp tốc độ cao đến companion computer nếu VIO
  là mục tiêu sau này. Máy VIO nên gửi odometry/vision estimate cho PX4, không
  gửi dữ liệu camera thô vào flight controller;
- rail nguồn payload có bảo vệ, đóng/cắt được và đủ current headroom.

Cảm biến tốc độ cao ngoài board nên ưu tiên CAN hoặc UART; không làm hệ thống
tương lai phụ thuộc vào dây I2C dài, nhạy nhiễu.

### Navigation/RF pod

Đặt RTK receiver, magnetometer, đầu nối RF GNSS và tùy chọn đầu nối radio
telemetry trên pod nhỏ có thể gắn cao/xa flight core, dòng ESC, dây DShot và
dây pin. Việc này cải thiện thu GNSS, giúp compass hữu ích và cô lập một thí
nghiệm RF lỗi khỏi quá trình phát hành FC cơ bản.

- Dùng anten GNSS lắp ngoài và đầu nối coax. Với dual-antenna heading, chọn
  giải pháp receiver thực sự hỗ trợ nó, ví dụ dual-F9P moving-baseline hoặc một
  receiver heading hai anten.
- Hai anten nên giống nhau nếu có thể, nằm cùng mặt phẳng, cùng hướng và có
  điều kiện ground plane tương đương. PX4 ghi nhận khoảng cách khoảng 50 cm đã
  được dùng trên test vehicle dual-F9P; phải kiểm tra yêu cầu riêng của receiver
  đã chọn trước khi chốt hình học frame.
- Ghi rõ anten nào là primary và vị trí của nó so với trục tiến của vehicle.
  PX4 sau đó dùng baseline qua `GPS_YAW_OFFSET` và dual-antenna heading fusion.
- Vẫn giữ compass dù dự định dùng GNSS heading. GNSS heading rất giá trị do
  miễn nhiễm nhiễu từ trường, nhưng magnetometer vẫn hữu ích khi RTK/heading
  chưa có hoặc suy giảm.
- Trước mắt nên để telemetry radio là tùy chọn module/cắm rời. Tích hợp raw RF
  receiver hoặc telemetry radio trực tiếp vào V2 làm tăng đáng kể độ khó về
  anten, layout RF, chứng nhận, coexistence và debug. Một cổng serial có nguồn
  chuẩn CRSF/ELRS tạo trải nghiệm dùng sớm; tích hợp RF có thể là revision riêng
  sau này.

### Các giai đoạn phát triển hợp lý

1. **V2A — flight core:** tái tạo hành vi IMU/DMA/reset V1 đã được kiểm chứng,
   xác nhận power monitoring và tám output, đồng thời đưa header CAN/UART/payload.
2. **V2B — navigation pod:** thêm RTK position, dual-antenna heading, compass
   và telemetry dưới dạng các module có thể kiểm tra độc lập.
3. **V2C — perception:** chỉ thêm range finder, radar, LiDAR hoặc VIO companion
   khi có mission cụ thể và ngân sách phù hợp. Các giao tiếp đã chừa sẵn giúp
   không cần thiết kế lại flight core vì chúng.

Nghiên cứu nguyên tắc hệ thống kiểu DJI--cô lập cảm biến, GNSS/compass đặt xa,
miền nguồn sạch, RF module và test sản xuất--là rất hữu ích. Schematic, layout,
cấu hình firmware và tích hợp cơ khí V2 cần được tự thiết kế, kiểm chứng độc
lập dựa trên PX4 và tài liệu công khai của linh kiện đã chọn.

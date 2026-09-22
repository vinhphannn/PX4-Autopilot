# Báo cáo Phân tích lỗi BMI088 trên Aegis FC v1.0

Tui đã tìm ra bản chất cốt lõi của hiện tượng lỗi này sau khi xâu chuỗi tất cả các log từ trước đến nay. Vấn đề KHÔNG PHẢI là do code khởi tạo, mà là do **sụt áp (Brown-out) phần cứng** trên đường nguồn 3.3V cấp cho cụm cảm biến!

## Bằng chứng từ Log:
1. **Quá trình khởi động ban đầu rất tốt:**
   - Cả Accel và Gyro đều vượt qua bước `probe()` (đọc thành công ID `0x1E` và `0x0F`).
   - Cả hai đều vượt qua bước `WAIT_FOR_RESET` (Gyro đọc lại được `0x0F`, Accel bật nguồn thành công).

2. **Lỗi đồng loạt ngay sau khi bật nguồn:**
   - Ngay khi Accel được lệnh bật nguồn (Active mode) và chúng ta chờ 60 ms để nó sẵn sàng, thì ở vòng cấu hình (`Configure()`), Accel đọc ra toàn `0x00`.
   - Gyro cũng vậy, khi vào vòng cấu hình nó đọc ra toàn `0xFF`.
   - `0x00` ở Accel có nghĩa là nó đã bị reset về lại **Suspend Mode** (chế độ ngủ mặc định khi cấp nguồn).
   - `0xFF` ở Gyro có nghĩa là nó đã bị reset về lại **I2C Mode** (chế độ mặc định khi cấp nguồn, do chân CS có pull-up).

## Kết luận:
Ngay tại thời điểm Accel bật nguồn (hoặc khi BMP280 bật nguồn cùng lúc đó), dòng điện tăng đột ngột đã làm điện áp 3.3V cấp cho BMI088 bị sụt xuống dưới ngưỡng hoạt động (Power-On Reset threshold). 
Điều này khiến cả con Accel và Gyro bị reset cứng! 
Sau khi reset, Gyro trở về I2C mode (nên SPI đọc ra 0xFF), còn Accel trở về Suspend mode (nên thanh ghi cấu hình đọc ra 0x00). Vòng lặp của PX4 cố gắng cấu hình lại nhưng vì chip đã bị reset về trạng thái ban đầu nên mọi giao tiếp SPI đều thất bại.

## Cách khắc phục:
1. **Phần cứng (Khuyên dùng):** Kiểm tra lại tụ chống nhiễu (decoupling capacitor) cho nguồn 3.3V cấp cho BMI088 trên mạch. Đảm bảo có tụ 100nF và tụ lớn (1uF - 10uF) đặt sát chân nguồn của cảm biến để bù dòng khi cảm biến thức dậy.
2. **Phần mềm:** Tui sẽ viết lại driver BMI088 để biến nó thành một "con đỉa" bám dai dẳng: Nếu phát hiện cảm biến bị reset (đọc ra 0x00 hoặc 0xFF), nó sẽ tự động chạy lại quy trình đánh thức (nháy chân CS cho Gyro và gửi lệnh bật nguồn cho Accel) ngay trong vòng lặp cấu hình, cho đến khi điện áp ổn định và cấu hình thành công!

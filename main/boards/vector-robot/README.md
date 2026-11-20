# XiaoZhi Vector Robot

Bu kart profili ESP32-S3 N16R8 üzerinde XiaoZhi ses asistanını korur ve robot sürüş, servo, MPU6050, arka IR ve UART kamera özelliklerini ekler.

## Derleme

Proje kökünde ESP-IDF 6.0.1 veya üstü etkinleştirildikten sonra:

```powershell
python scripts/build.py vector-robot
```

Kart seçimi `main/Kconfig.projbuild` ve `main/CMakeLists.txt` üzerinden yapılır. OLED tipi SH1106 128x64 olarak seçilidir.

## MCP araçları

- `robot.forward`, `robot.backward`, `robot.left`, `robot.right`, `robot.stop`
- `robot.set_speed`, `robot.get_status`, `robot.set_servo`
- `sensor.get_distance`, `sensor.get_imu`
- `self.camera.take_photo` (XiaoZhi çekirdek MCP aracı; `GetCamera()` üzerinden gerçek UART kamerayı kullanır)

## Kamera UART protokolü

Ana kart UART1 kullanır: TX GPIO2, RX GPIO3, 115200 8N1. ESP32-CAM yardımcı kartı `camera-companion/` altındaki firmware'i çalıştırmalıdır. Ana kart `CAPTURE\n` gönderir; yardımcı kart 8 baytlık `VCAM` + big-endian JPEG uzunluğu başlığı ve ardından JPEG verisini gönderir. Hata durumunda `VERR` başlığı ve sıfır uzunluk döner. Ayar komutları `HMIRROR 0|1\n` ve `VFLIP 0|1\n` şeklindedir.

GPIO3, ESP32-S3 strapping pinidir. Bu hatta harici devrenin reset/boot sırasında hattı zorlamaması gerekir; kamera UART kablolaması ilk açılışta bağlantı sorununa neden olursa kartın boot davranışı ayrıca ölçülmelidir.

## Notlar

- I2S pinleri ve OLED I2C pinleri değiştirilmedi.
- MPU6050, OLED ile GPIO41/42 I2C hattını paylaşır; firmware önce 0x68'i, yanıt alamazsa 0x69'u dener.
- IR sensör çıkışı aktif LOW kabul edilir. Sensör dijital yaklaşım/engel durumu verir; santimetre cinsinden mesafe ölçmez. Geri hareket sırasında engel algılanırsa motor komutu reddedilir.
- Motor ve servo PWM kanalları farklı LEDC timer'larına ayrılmıştır.
- Motor beslemesi ile servo beslemesi ESP32-S3 3.3V hattından alınmamalıdır. TB6612 STBY başlangıçta LOW tutulur.
- Bu sürümde kart kodu ve UART protokolü kaynak seviyesinde eklenmiştir. Donanım testi ve ESP-IDF derlemesi yapılmadan "derleme doğrulandı" veya "kamera fiziksel olarak test edildi" kabul edilmemelidir.

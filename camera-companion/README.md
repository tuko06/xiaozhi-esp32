# ESP32-CAM companion firmware

Bu yardımcı firmware AI-Thinker pin dizilimli ESP32-CAM modülü içindir. Arduino IDE'de ESP32 kart paketi kurulu olmalı; kart olarak **AI Thinker ESP32-CAM** seçilip bu sketch yüklenmelidir.

## UART bağlantısı

| ESP32-S3 Vector Robot | ESP32-CAM |
|---|---|
| GPIO2 (TX) | GPIO13 (RX / Serial2) |
| GPIO3 (RX) | GPIO14 (TX / Serial2) |
| GND | GND |

UART: 115200, 8N1. Kamera modülünün beslemesi ayrı ve kararlı 5V kaynaktan sağlanmalıdır. Ortak GND zorunludur. ESP32-CAM programlama için UART0 pinlerini kullanır; çalışma UART'ı GPIO13/14'tür.

## Protokol

- Ana kart: `CAPTURE\n`
- Kamera: 8 bayt başlık `VCAM` + 32-bit big-endian JPEG uzunluğu + JPEG baytları
- Ayar: `HMIRROR 0|1\n`, `VFLIP 0|1\n`

PlatformIO ile derlemek için proje kökünde `pio run -d camera-companion`; yüklemek için `pio run -d camera-companion -t upload` kullanılır. Yalnızca AI-Thinker pin haritası doğrulanmış kartlar içindir. Farklı ESP32-CAM klonlarında pin haritası değişebilir. Ana kart GPIO3 bir strapping pinidir; ilk açılış/boot testinde UART hattının boot seviyesini bozmadığı doğrulanmalıdır.

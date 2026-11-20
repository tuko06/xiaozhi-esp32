#include "esp_camera.h"

// AI-Thinker ESP32-CAM pin map. The companion uses UART2 on GPIO13/14
// so UART0 remains available for flashing and serial diagnostics.
static constexpr int CAM_UART_RX = 13;
static constexpr int CAM_UART_TX = 14;
static bool hmirror = false;
static bool vflip = false;

static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = 5;  c.pin_d1 = 18; c.pin_d2 = 19; c.pin_d3 = 21;
  c.pin_d4 = 36; c.pin_d5 = 39; c.pin_d6 = 34; c.pin_d7 = 35;
  c.pin_xclk = 0; c.pin_pclk = 22; c.pin_vsync = 25; c.pin_href = 23;
  c.pin_sccb_sda = 26; c.pin_sccb_scl = 27;
  c.pin_pwdn = 32; c.pin_reset = -1;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

static void sendFrame() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb || fb->format != PIXFORMAT_JPEG || fb->len == 0 || fb->len > 512 * 1024) {
    if (fb) esp_camera_fb_return(fb);
    const uint8_t fail[8] = {'V','E','R','R',0,0,0,0};
    Serial2.write(fail, sizeof(fail));
    return;
  }
  const uint32_t n = fb->len;
  const uint8_t header[8] = {
    'V','C','A','M',
    static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
    static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)
  };
  Serial2.write(header, sizeof(header));
  size_t sent = 0;
  while (sent < fb->len) {
    size_t written = Serial2.write(fb->buf + sent, fb->len - sent);
    if (written == 0) break;
    sent += written;
  }
  Serial2.flush();
  esp_camera_fb_return(fb);
}

void setup() {
  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, CAM_UART_RX, CAM_UART_TX);
  if (!initCamera()) {
    Serial.println("Camera init failed");
    while (true) delay(1000);
  }
  sensor_t* s = esp_camera_sensor_get();
  if (s) {
    s->set_hmirror(s, hmirror);
    s->set_vflip(s, vflip);
  }
  Serial.println("Vector camera companion ready");
}

void loop() {
  static String command;
  while (Serial2.available()) {
    char ch = static_cast<char>(Serial2.read());
    if (ch == '\n') {
      command.trim();
      if (command == "CAPTURE") {
        sendFrame();
      } else if (command.startsWith("HMIRROR ")) {
        hmirror = command.endsWith("1");
        if (sensor_t* s = esp_camera_sensor_get()) s->set_hmirror(s, hmirror);
      } else if (command.startsWith("VFLIP ")) {
        vflip = command.endsWith("1");
        if (sensor_t* s = esp_camera_sensor_get()) s->set_vflip(s, vflip);
      }
      command = "";
    } else if (command.length() < 32) {
      command += ch;
    } else {
      command = "";
    }
  }
  delay(2);
}

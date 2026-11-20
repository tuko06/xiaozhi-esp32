#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "led/gpio_led.h"
#include "assets/lang_config.h"
#include "camera.h"
#include "system_info.h"

#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_sh1106.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define TAG "VectorRobot"

class VectorRobotBoard;

class VectorCamera final : public Camera {
public:
    VectorCamera() {
        uart_config_t cfg = {};
        cfg.baud_rate = ROBOT_CAMERA_BAUD;
        cfg.data_bits = UART_DATA_8_BITS;
        cfg.parity = UART_PARITY_DISABLE;
        cfg.stop_bits = UART_STOP_BITS_1;
        cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        cfg.source_clk = UART_SCLK_DEFAULT;
        esp_err_t err = uart_driver_install(ROBOT_CAMERA_UART, 4096, 0, 0, nullptr, 0);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Camera UART install failed: %s", esp_err_to_name(err));
            return;
        }
        ESP_ERROR_CHECK(uart_param_config(ROBOT_CAMERA_UART, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(ROBOT_CAMERA_UART, ROBOT_CAMERA_TX, ROBOT_CAMERA_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ready_ = true;
    }

    ~VectorCamera() override { if (frame_ != nullptr) heap_caps_free(frame_); }

    void SetExplainUrl(const std::string& url, const std::string& token) override {
        explain_url_ = url;
        explain_token_ = token;
    }

    bool Capture() override {
        if (!ready_) return false;
        uart_flush_input(ROBOT_CAMERA_UART);
        static constexpr char command[] = "CAPTURE\n";
        if (uart_write_bytes(ROBOT_CAMERA_UART, command, sizeof(command) - 1) < 0) return false;

        uint8_t header[8] = {};
        if (uart_read_bytes(ROBOT_CAMERA_UART, header, sizeof(header), pdMS_TO_TICKS(5000)) != sizeof(header)) {
            ESP_LOGE(TAG, "Camera companion header timeout");
            return false;
        }
        if (memcmp(header, "VCAM", 4) != 0) {
            ESP_LOGE(TAG, "Invalid camera companion frame signature");
            return false;
        }
        uint32_t length = (static_cast<uint32_t>(header[4]) << 24) |
                          (static_cast<uint32_t>(header[5]) << 16) |
                          (static_cast<uint32_t>(header[6]) << 8) | header[7];
        if (length < 4 || length > kMaxJpegBytes) {
            ESP_LOGE(TAG, "Invalid JPEG frame length: %lu", static_cast<unsigned long>(length));
            return false;
        }

        if (frame_ != nullptr) { heap_caps_free(frame_); frame_ = nullptr; frame_size_ = 0; }
        frame_ = static_cast<uint8_t*>(heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (frame_ == nullptr) {
            ESP_LOGE(TAG, "Unable to allocate PSRAM for JPEG frame (%lu bytes)", static_cast<unsigned long>(length));
            return false;
        }
        size_t offset = 0;
        while (offset < length) {
            int n = uart_read_bytes(ROBOT_CAMERA_UART, frame_ + offset,
                                    length - offset, pdMS_TO_TICKS(3000));
            if (n <= 0) {
                heap_caps_free(frame_); frame_ = nullptr; frame_size_ = 0;
                ESP_LOGE(TAG, "Camera JPEG transfer incomplete");
                return false;
            }
            offset += static_cast<size_t>(n);
        }
        frame_size_ = length;
        return frame_size_ >= 4 && frame_[0] == 0xFF && frame_[1] == 0xD8;
    }

    bool SetHMirror(bool enabled) override { return SendSetting("HMIRROR", enabled); }
    bool SetVFlip(bool enabled) override { return SendSetting("VFLIP", enabled); }

    std::expected<std::string, std::string> Explain(const std::string& question) override {
        if (frame_ == nullptr || frame_size_ == 0) return std::unexpected("Önce kamera görüntüsü alınmalı");
        if (explain_url_.empty()) return std::unexpected("Görüntü açıklama servisi URL'si ayarlanmamış");
        auto network = Board::GetInstance().GetNetwork();
        if (network == nullptr) return std::unexpected("Ağ bağlantısı kullanılamıyor");

        auto http = network->CreateHttp(3);
        const std::string boundary = "----VECTOR_ROBOT_CAMERA";
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        if (!explain_token_.empty()) http->SetHeader("Authorization", "Bearer " + explain_token_);
        http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http->SetHeader("Transfer-Encoding", "chunked");
        if (auto opened = http->Open("POST", explain_url_); !opened)
            return std::unexpected("Görüntü açıklama servisine bağlanılamadı");

        auto write = [&http](const std::string& s) -> bool {
            return static_cast<bool>(http->Write(s.data(), s.size()));
        };
        std::string q = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"question\"\r\n\r\n" +
                        question + "\r\n";
        std::string h = "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"vector.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n";
        if (!write(q) || !write(h) || !static_cast<bool>(http->Write(
                reinterpret_cast<const char*>(frame_), frame_size_)) ||
            !write("\r\n--" + boundary + "--\r\n") || !static_cast<bool>(http->Write("", 0))) {
            http->Close();
            return std::unexpected("Kamera görüntüsü yüklenemedi");
        }
        auto status = http->GetStatusCode();
        if (!status || *status != 200) {
            http->Close();
            return std::unexpected("Görüntü servisi HTTP hatası");
        }
        std::string response = http->ReadAll();
        http->Close();
        return response;
    }

private:
    static constexpr uint32_t kMaxJpegBytes = 512 * 1024;
    bool ready_ = false;
    std::string explain_url_;
    std::string explain_token_;
    uint8_t* frame_ = nullptr;
    size_t frame_size_ = 0;

    bool SendSetting(const char* key, bool enabled) {
        if (!ready_) return false;
        std::string cmd = std::string(key) + (enabled ? " 1\n" : " 0\n");
        return uart_write_bytes(ROBOT_CAMERA_UART, cmd.data(), cmd.size()) == cmd.size();
    }
};

class VectorRobotBoard final : public WifiBoard {
public:
    VectorRobotBoard()
        : boot_button_(BOOT_BUTTON_GPIO), touch_button_(TOUCH_BUTTON_GPIO),
          volume_up_button_(VOLUME_UP_BUTTON_GPIO), volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeI2cAndDisplay();
        InitializeRobotIo();
        InitializeButtons();
        InitializeTools();
        camera_ = std::make_unique<VectorCamera>();
    }

    Led* GetLed() override {
        static GpioLed led(BUILTIN_LED_GPIO, 0, LEDC_TIMER_2, LEDC_CHANNEL_5);
        return &led;
    }

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &codec;
    }

    Display* GetDisplay() override { return display_; }
    Camera* GetCamera() override { return camera_.get(); }

private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_master_dev_handle_t imu_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    std::unique_ptr<VectorCamera> camera_;
    Button boot_button_, touch_button_, volume_up_button_, volume_down_button_;
    int speed_ = 55;
    bool imu_ready_ = false;
    bool motors_ready_ = false;
    bool servos_ready_ = false;
    int left_direction_ = 0;
    int right_direction_ = 0;

    void InitializeI2cAndDisplay() {
        i2c_master_bus_config_t bus = {};
        bus.i2c_port = I2C_NUM_0;
        bus.sda_io_num = ROBOT_I2C_SDA;
        bus.scl_io_num = ROBOT_I2C_SCL;
        bus.clk_source = I2C_CLK_SRC_DEFAULT;
        bus.glitch_ignore_cnt = 7;
        bus.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus, &i2c_bus_));

        i2c_device_config_t imu_cfg = {};
        imu_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        imu_cfg.device_address = ROBOT_MPU6050_ADDRESS;
        imu_cfg.scl_speed_hz = 400000;
        if (i2c_master_bus_add_device(i2c_bus_, &imu_cfg, &imu_) == ESP_OK) {
            uint8_t wake[] = {0x6B, 0x00};
            imu_ready_ = i2c_master_transmit(imu_, wake, sizeof(wake), 100) == ESP_OK;
            if (!imu_ready_) {
                i2c_master_bus_rm_device(imu_);
                imu_ = nullptr;
                imu_cfg.device_address = (ROBOT_MPU6050_ADDRESS == 0x68) ? 0x69 : 0x68;
                if (i2c_master_bus_add_device(i2c_bus_, &imu_cfg, &imu_) == ESP_OK) {
                    imu_ready_ = i2c_master_transmit(imu_, wake, sizeof(wake), 100) == ESP_OK;
                }
            }
        }

        esp_lcd_panel_io_i2c_config_t io = {};
        io.dev_addr = 0x3C;
        io.scl_speed_hz = 400000;
        io.control_phase_bytes = 1;
        io.dc_bit_offset = 6;
        io.lcd_cmd_bits = 8;
        io.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &io, &panel_io_));

        esp_lcd_panel_dev_config_t panel_cfg = {};
        panel_cfg.reset_gpio_num = GPIO_NUM_NC;
        panel_cfg.bits_per_pixel = 1;
        esp_lcd_panel_ssd1306_config_t sh1106_cfg = {};
        sh1106_cfg.height = DISPLAY_HEIGHT;
        panel_cfg.vendor_config = &sh1106_cfg;
        ESP_ERROR_CHECK(esp_lcd_new_panel_sh1106(panel_io_, &panel_cfg, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));
        display_ = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                   DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
    }

    void InitializeRobotIo() {
        gpio_config_t io = {};
        io.pin_bit_mask = (1ULL << ROBOT_AIN1) | (1ULL << ROBOT_AIN2) |
                          (1ULL << ROBOT_BIN1) | (1ULL << ROBOT_BIN2) | (1ULL << ROBOT_STBY);
        io.mode = GPIO_MODE_OUTPUT;
        ESP_ERROR_CHECK(gpio_config(&io));
        gpio_set_level(ROBOT_STBY, 0);

        ledc_timer_config_t motor_timer = {};
        motor_timer.speed_mode = LEDC_LOW_SPEED_MODE;
        motor_timer.duty_resolution = LEDC_TIMER_10_BIT;
        motor_timer.timer_num = LEDC_TIMER_0;
        motor_timer.freq_hz = 20000;
        motor_timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&motor_timer));
        ConfigurePwm(ROBOT_PWMA, LEDC_CHANNEL_0, LEDC_TIMER_0);
        ConfigurePwm(ROBOT_PWMB, LEDC_CHANNEL_1, LEDC_TIMER_0);
        motors_ready_ = true;
        StopMotors();

        ledc_timer_config_t servo_timer = {};
        servo_timer.speed_mode = LEDC_LOW_SPEED_MODE;
        servo_timer.duty_resolution = LEDC_TIMER_13_BIT;
        servo_timer.timer_num = LEDC_TIMER_1;
        servo_timer.freq_hz = 50;
        servo_timer.clk_cfg = LEDC_AUTO_CLK;
        ESP_ERROR_CHECK(ledc_timer_config(&servo_timer));
        ConfigurePwm(SERVO_HEAD_GPIO, LEDC_CHANNEL_2, LEDC_TIMER_1);
        ConfigurePwm(SERVO_LEFT_ARM_GPIO, LEDC_CHANNEL_3, LEDC_TIMER_1);
        ConfigurePwm(SERVO_RIGHT_ARM_GPIO, LEDC_CHANNEL_4, LEDC_TIMER_1);
        servos_ready_ = true;
        SetServo(LEDC_CHANNEL_2, 90);
        SetServo(LEDC_CHANNEL_3, 90);
        SetServo(LEDC_CHANNEL_4, 90);

        gpio_config_t ir = {};
        ir.pin_bit_mask = 1ULL << ROBOT_REAR_IR_GPIO;
        ir.mode = GPIO_MODE_INPUT;
        ir.pull_up_en = GPIO_PULLUP_ENABLE;
        ir.pull_down_en = GPIO_PULLDOWN_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&ir));
    }

    void ConfigurePwm(gpio_num_t pin, ledc_channel_t channel, ledc_timer_t timer) {
        ledc_channel_config_t cfg = {};
        cfg.gpio_num = pin;
        cfg.speed_mode = LEDC_LOW_SPEED_MODE;
        cfg.channel = channel;
        cfg.intr_type = LEDC_INTR_DISABLE;
        cfg.timer_sel = timer;
        cfg.duty = 0;
        cfg.hpoint = 0;
        ESP_ERROR_CHECK(ledc_channel_config(&cfg));
    }

    void SetServo(ledc_channel_t channel, int angle) {
        angle = std::clamp(angle, 0, 180);
        const uint32_t pulse_us = 500 + (static_cast<uint32_t>(angle) * 2000 / 180);
        const uint32_t duty = pulse_us * 8191 / 20000;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channel, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channel);
    }

    void SetMotor(ledc_channel_t pwm, gpio_num_t in1, gpio_num_t in2, int value) {
        value = std::clamp(value, -100, 100);
        gpio_set_level(in1, value >= 0);
        gpio_set_level(in2, value < 0);
        const uint32_t duty = static_cast<uint32_t>(std::abs(value)) * 1023 / 100;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, pwm, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, pwm);
    }

    void Drive(int left, int right) {
        if (!motors_ready_) return;
        left_direction_ = (left > 0) - (left < 0);
        right_direction_ = (right > 0) - (right < 0);
        gpio_set_level(ROBOT_STBY, 1);
        SetMotor(LEDC_CHANNEL_0, ROBOT_AIN1, ROBOT_AIN2, left);
        SetMotor(LEDC_CHANNEL_1, ROBOT_BIN1, ROBOT_BIN2, right);
    }

    void StopMotors() {
        if (!motors_ready_) return;
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        gpio_set_level(ROBOT_STBY, 0);
        left_direction_ = 0;
        right_direction_ = 0;
    }

    bool ReadImu(int16_t& ax, int16_t& ay, int16_t& az, int16_t& gx, int16_t& gy, int16_t& gz) {
        if (!imu_ready_) return false;
        uint8_t reg = 0x3B, data[14] = {};
        if (i2c_master_transmit_receive(imu_, &reg, 1, data, sizeof(data), 100) != ESP_OK) return false;
        auto word = [&data](int i) { return static_cast<int16_t>((data[i] << 8) | data[i + 1]); };
        ax = word(0); ay = word(2); az = word(4);
        gx = word(8); gy = word(10); gz = word(12);
        return true;
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) { EnterWifiConfigMode(); return; }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([] { Application::GetInstance().StartListening(); });
        touch_button_.OnPressUp([] { Application::GetInstance().StopListening(); });
        volume_up_button_.OnClick([this] {
            auto* codec = GetAudioCodec();
            int volume = std::min(100, codec->output_volume() + 10);
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
        volume_down_button_.OnClick([this] {
            auto* codec = GetAudioCodec();
            int volume = std::max(0, codec->output_volume() - 10);
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
    }

    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool("robot.forward", "Robotu ileri sür.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult { Drive(speed_, speed_); return std::string("İleri hareket"); });
        mcp.AddTool("robot.backward", "Robotu geri sür; arka IR sensörü engel algılarsa hareket etme.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult {
                if (gpio_get_level(ROBOT_REAR_IR_GPIO) == ROBOT_IR_ACTIVE_LOW) {
                    StopMotors();
                    return std::unexpected("Arka tarafta engel algılandı; geri hareket iptal edildi");
                }
                Drive(-speed_, -speed_);
                return std::string("Geri hareket");
            });
        mcp.AddTool("robot.left", "Robotu sola döndür.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult { Drive(-speed_, speed_); return std::string("Sola dönüş"); });
        mcp.AddTool("robot.right", "Robotu sağa döndür.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult { Drive(speed_, -speed_); return std::string("Sağa dönüş"); });
        mcp.AddTool("robot.stop", "Motorları durdur.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult { StopMotors(); return std::string("Motorlar durduruldu"); });
        mcp.AddTool("robot.set_speed", "Motor hızını yüzde olarak ayarla.", PropertyList({
            Property("speed", kPropertyTypeInteger, 55, 0, 100)}),
            [this](const PropertyList& p) -> ToolResult {
                speed_ = p["speed"].value<int>();
                if (speed_ == 0) {
                    StopMotors();
                } else if (left_direction_ != 0 || right_direction_ != 0) {
                    Drive(left_direction_ * speed_, right_direction_ * speed_);
                }
                return std::string("Hız: ") + std::to_string(speed_);
            });
        mcp.AddTool("robot.get_status", "Robot motor, sensör ve IMU durumunu döndür.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult {
                int ir = gpio_get_level(ROBOT_REAR_IR_GPIO);
                int16_t ax=0, ay=0, az=0, gx=0, gy=0, gz=0;
                bool imu = ReadImu(ax, ay, az, gx, gy, gz);
                return std::string("{\"speed\":") + std::to_string(speed_) +
                    ",\"rear_ir_detected\":" + ((ir == ROBOT_IR_ACTIVE_LOW) ? "true" : "false") +
                    ",\"imu_ready\":" + (imu ? "true" : "false") +
                    ",\"accel_raw\":[" + std::to_string(ax) + "," + std::to_string(ay) + "," + std::to_string(az) +
                    "],\"gyro_raw\":[" + std::to_string(gx) + "," + std::to_string(gy) + "," + std::to_string(gz) + "]}";
            });
        mcp.AddTool("sensor.get_distance", "Arka IR yaklaşım sensörünün engel algılama durumunu oku (santimetre ölçmez).", PropertyList({}),
            [this](const PropertyList&) -> ToolResult {
                bool detected = gpio_get_level(ROBOT_REAR_IR_GPIO) == ROBOT_IR_ACTIVE_LOW;
                return detected ? std::string("Arka sensörde engel algılandı") : std::string("Arka sensörde engel algılanmadı");
            });
        mcp.AddTool("sensor.get_imu", "MPU6050 ivmeölçer ve jiroskop ham verisini oku.", PropertyList({}),
            [this](const PropertyList&) -> ToolResult {
                int16_t ax=0, ay=0, az=0, gx=0, gy=0, gz=0;
                if (!ReadImu(ax, ay, az, gx, gy, gz)) return std::unexpected("MPU6050 yanıt vermiyor");
                return std::string("{\"accel\":[") + std::to_string(ax) + "," + std::to_string(ay) + "," + std::to_string(az) +
                    "],\"gyro\":[" + std::to_string(gx) + "," + std::to_string(gy) + "," + std::to_string(gz) + "]}";
            });
        mcp.AddTool("robot.set_servo", "Bir servoyu belirtilen açıya getir.", PropertyList({
            Property("servo", kPropertyTypeString).SetMaxLength(16), Property("angle", kPropertyTypeInteger, 90, 0, 180)}),
            [this](const PropertyList& p) -> ToolResult {
                const auto& name = p["servo"].value<std::string>();
                auto angle = p["angle"].value<int>();
                if (name == "head") SetServo(LEDC_CHANNEL_2, angle);
                else if (name == "left_arm") SetServo(LEDC_CHANNEL_3, angle);
                else if (name == "right_arm") SetServo(LEDC_CHANNEL_4, angle);
                else return std::unexpected("servo: head, left_arm veya right_arm olmalı");
                return std::string(name) + " servo " + std::to_string(angle) + " derece";
            });
        // The core XiaoZhi MCP server automatically exposes self.camera.take_photo
        // when GetCamera() returns a non-null Camera implementation. That tool calls
        // Capture() and Explain() using the server-provided vision endpoint.
    }
};

DECLARE_BOARD(VectorRobotBoard);

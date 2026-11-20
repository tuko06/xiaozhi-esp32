import json
import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "main" / "boards" / "vector-robot"


class VectorRobotBoardConfigTests(unittest.TestCase):
    def test_release_variant_targets_esp32s3_and_sh1106(self):
        config = json.loads((BOARD / "config.json").read_text(encoding="utf-8"))
        self.assertEqual(config["type"], "vector-robot")
        self.assertEqual(config["target"], "esp32s3")
        self.assertEqual(len(config["builds"]), 1)
        options = config["builds"][0]["sdkconfig_append"]
        self.assertIn("CONFIG_OLED_SH1106_128X64=y", options)
        self.assertIn("CONFIG_SPIRAM=y", options)

    def test_board_is_registered_in_kconfig_and_cmake(self):
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
        cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("config BOARD_TYPE_VECTOR_ROBOT", kconfig)
        self.assertRegex(
            kconfig,
            r"choice DISPLAY_OLED_TYPE[\s\S]*?BOARD_TYPE_VECTOR_ROBOT",
        )
        self.assertIn('set(BOARD_DIR "vector-robot")', cmake)

    def test_preserved_audio_and_display_pin_map(self):
        config = (BOARD / "config.h").read_text(encoding="utf-8")
        expected = {
            "AUDIO_I2S_MIC_GPIO_WS": "4",
            "AUDIO_I2S_MIC_GPIO_SCK": "5",
            "AUDIO_I2S_MIC_GPIO_DIN": "6",
            "AUDIO_I2S_SPK_GPIO_DOUT": "7",
            "AUDIO_I2S_SPK_GPIO_BCLK": "15",
            "AUDIO_I2S_SPK_GPIO_LRCK": "16",
            "DISPLAY_SDA_PIN": "41",
            "DISPLAY_SCL_PIN": "42",
        }
        for macro, pin in expected.items():
            self.assertRegex(config, rf"#define {macro}\s+GPIO_NUM_{pin}\b")

    def test_robot_io_pins_are_declared(self):
        config = (BOARD / "config.h").read_text(encoding="utf-8")
        expected = {
            "ROBOT_AIN1": "8", "ROBOT_AIN2": "9", "ROBOT_PWMA": "10",
            "ROBOT_BIN1": "11", "ROBOT_BIN2": "12", "ROBOT_PWMB": "13",
            "ROBOT_STBY": "14", "SERVO_HEAD_GPIO": "19",
            "SERVO_LEFT_ARM_GPIO": "20", "SERVO_RIGHT_ARM_GPIO": "38",
            "ROBOT_REAR_IR_GPIO": "21",
        }
        for macro, pin in expected.items():
            self.assertRegex(config, rf"#define {macro}\s+GPIO_NUM_{pin}\b")

    def test_camera_and_mcp_interfaces_are_real(self):
        source = (BOARD / "vector_robot_board.cc").read_text(encoding="utf-8")
        companion = (ROOT / "camera-companion" / "camera-companion.ino").read_text(encoding="utf-8")
        for tool in (
            "robot.forward", "robot.backward", "robot.left", "robot.right",
            "robot.stop", "robot.set_speed", "robot.get_status",
            "robot.set_servo", "sensor.get_distance", "sensor.get_imu",
        ):
            self.assertIn(tool, source)
        self.assertIn("Camera* GetCamera() override { return camera_.get(); }", source)
        self.assertIn("gpio_set_level(ROBOT_STBY, 0);", source)
        self.assertRegex((BOARD / "config.h").read_text(encoding="utf-8"), r"#define ROBOT_IR_ACTIVE_LOW\\s+0\\b")
        self.assertIn('"CAPTURE\\n"', source)
        self.assertIn("'V','C','A','M'", companion)


if __name__ == "__main__":
    unittest.main()

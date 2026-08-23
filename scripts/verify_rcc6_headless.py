#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ini = (ROOT / "variants/heltec_rcc6/platformio.ini").read_text(encoding="utf-8")
main = (ROOT / "examples/companion_radio/main.cpp").read_text(encoding="utf-8")
board = (ROOT / "variants/heltec_rcc6/heltec_rcc6.cpp").read_text(encoding="utf-8")
service = (ROOT / "examples/companion_radio/UltimateService.cpp").read_text(encoding="utf-8")
ui = (ROOT / "examples/companion_radio/ui-new/UltimateUIScreen.cpp").read_text(encoding="utf-8")
ui_task = (ROOT / "examples/companion_radio/ui-new/UITask.cpp").read_text(encoding="utf-8")
web = (ROOT / "examples/companion_radio/webui/src/app.js").read_text(encoding="utf-8")
common_cli = (ROOT / "src/helpers/CommonCLI.cpp").read_text(encoding="utf-8")

required = {
    "heltec_rcc6_headless_companion_ble": ["BLE_PIN_CODE=123456", 'NEONPOCKET_HEADLESS_MODE=\'"ble"\''],
    "heltec_rcc6_headless_companion_usb": ["ENABLE_USB_INTERFACE", 'NEONPOCKET_HEADLESS_MODE=\'"usb"\''],
    "heltec_rcc6_headless_companion_web": ["RCC6_WEB_AP=1", 'NEONPOCKET_HEADLESS_MODE=\'"web"\''],
}

for environment, markers in required.items():
    header = f"[env:{environment}]"
    assert header in ini, f"missing {environment}"
    block = ini.split(header, 1)[1].split("\n[", 1)[0]
    for marker in markers:
        assert marker in block, f"{environment} missing {marker}"
    assert "DISPLAY_CLASS" not in block, f"{environment} enables a display"

common = ini.split("[heltec_rcc6_headless_companion]", 1)[1].split("\n[", 1)[0]
assert "FIRMWARE_VERSION='\"v1.17.1\"'" in common, \
    "headless targets must report the MeshCore 1.17.1 maintenance line"
assert "DISPLAY_CLASS" not in common
assert "NEONPOCKET_SAFE_SPIFFS_BOOTSTRAP=1" in common
assert "MAX_CONTACTS=350" in common
assert "MAX_GROUP_CHANNELS=40" in common
assert "OFFLINE_QUEUE_SIZE=256" in common
assert "NeonPocket Headless setup password" in main
assert "NeonPocketMC RCC6 Headless" in main
assert "measured <= 4500U" in board
assert "calibrated > 0 && calibrated <= 4500" in service
assert 'strcpy(battery, "--")' in ui
assert 'strcpy(line, "BATTERY --")' in ui
assert 'display.print("--")' in ui_task
assert "ultimate.batteryMv > 0" in web
assert '(key === "battery" && raw <= 0)' in web
assert common_cli.index('strcmp(command, "gps advert prefs")') < \
    common_cli.index("#if ENV_INCLUDE_GPS == 1"), \
    "saved-coordinate advert policy must not require physical GPS hardware"

print("RCC6 headless companion contract verified")

#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ini = (ROOT / "variants/heltec_rcc6/platformio.ini").read_text(encoding="utf-8")
main = (ROOT / "examples/companion_radio/main.cpp").read_text(encoding="utf-8")

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
assert "DISPLAY_CLASS" not in common
assert "NEONPOCKET_SAFE_SPIFFS_BOOTSTRAP=1" in common
assert "MAX_CONTACTS=350" in common
assert "MAX_GROUP_CHANNELS=40" in common
assert "OFFLINE_QUEUE_SIZE=256" in common
assert "NeonPocket Headless setup password" in main
assert "NeonPocketMC RCC6 Headless" in main

print("RCC6 headless companion contract verified")

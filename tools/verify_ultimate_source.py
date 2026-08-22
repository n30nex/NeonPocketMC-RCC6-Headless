#!/usr/bin/env python3
"""Fail-closed static contract checks for the Ultimate-only build surface."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def text(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


platform = text("variants/heltec_rcc6/platformio.ini")
display = text("src/helpers/ui/NV3001BDisplay.cpp")
service = text("examples/companion_radio/UltimateService.h")
service_cpp = text("examples/companion_radio/UltimateService.cpp")
web = text("examples/companion_radio/UltimateWebApi.cpp")
ui = text("examples/companion_radio/ui-new/UltimateUIScreen.cpp")
ui_task = text("examples/companion_radio/ui-new/UITask.cpp")
mesh = text("examples/companion_radio/MyMesh.cpp")

required = {
    "Ultimate BLE environment": "[env:heltec_rcc6_ultimate_companion_ble]" in platform,
    "Ultimate Web environment": "[env:heltec_rcc6_ultimate_companion_web]" in platform,
    "32 KiB gate": "NEONPOCKET_MEMORY_GATE_BYTES=32768" in platform,
    "indexed framebuffer": "NV3001B_USE_INDEXED_FRAMEBUFFER=1" in platform,
    "20 by 8 tiles": "framebuffer_tile_width = 20" in text("src/helpers/ui/NV3001BDisplay.h") and
        "framebuffer_tile_height = 8" in text("src/helpers/ui/NV3001BDisplay.h"),
    "256-byte history record": "static_assert(sizeof(UltimateHistoryRecord) == 256" in service,
    "32 event queue": "pending_capacity = 32" in service,
    "64 network nodes": "network_capacity = 64" in service,
    "signed OTA": "Ed25519::verify" in web and "Update.begin" in web,
    "location endpoint": all(value in web for value in
        ['"/api/ultimate/location"', 'extractBool(body, "advertise"',
         "advert_loc_policy = advertise ? ADVERT_LOC_SHARE : ADVERT_LOC_NONE"]),
    "six-area UI": all(name in ui for name in ["HOME", "INBOX", "NETWORK", "RADIO", "TOOLS", "POWER"]),
    "adaptive animation cadence": all(value in service_cpp for value in
        ["getRecommendedFrameMillis", "flush >= 45000", "flush >= 90000"]) and
        "return ultimate_service.getRecommendedFrameMillis()" in ui,
    "adaptive display timeout": "displayAutoOffMillis" in ui_task and
        "getDisplayTimeoutMillis" in service_cpp,
    "truthful delivery states": all(value in service for value in
        ["Queued", "OnAir", "Transmitted", "Acked", "NoAck", "Unconfirmed", "Failed"]) and
        "_ultimate_delivery_hash" in mesh and
        "markDeliveryAcked" in mesh,
    "encoded hop count decoding": all(value in mesh for value in
        ["displayHopCount(0x80) == 0", "displayHopCount(0x81) == 1",
         "displayHopCount(0xC2) == 2", "displayHopCount(0xFF) == 0xFF"]) and
        mesh.count("displayHopCount(path_len)") >= 3 and
        all(value in service_cpp for value in
            ["displayHopCount(0x80) == 0", "record.path_len = displayHopCount(record.path_len)",
             "record.path_len = displayHopCount(event.path_len)",
             "event.path_len = displayHopCount(path_len)",
             "node.path_len = displayHopCount(event.path_len)"]),
    "persistent composer": all(value in service for value in
        ["UltimateComposerState", "setPinnedTarget", "saveDraft"]) and
        all(value in ui for value in ["RESUME DRAFT", "PIN TARGET", "{battery}", "{location}", "{name}"]),
    "battery intelligence": all(value in service for value in
        ["battery_trend_mv_per_hour", "battery_runtime_minutes", "battery_calibration_mv",
         "battery_capacity_mah", "usb_host_connected", "power_profile"]) and
        "usb_serial_jtag_is_connected" in service_cpp and
        "set.batterysize " in mesh and "np battery size " in mesh,
    "triple press navigation": "handleTriplePress" in ui and "markAllRead" in service_cpp and
        "handleTripleClick" in ui_task and "3X CLEAR" in ui,
    "artifact verifier": (ROOT / "tools/verify_ultimate_artifact.py").is_file(),
    "portable recovery merger": (ROOT / "tools/merge_rcc6_image.py").is_file(),
}

failed = [name for name, passed in required.items() if not passed]
if failed:
    raise SystemExit("Ultimate source contract failed: " + ", ".join(failed))
print("Ultimate source contract verified")

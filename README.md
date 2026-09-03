<p align="center">
  <img src="https://raw.githubusercontent.com/n30nex/NeonPocketMC/main/branding/neonpocketmc-mark.png" alt="NeonPocketMC pocket mesh logo" width="140">
</p>

# NeonPocketMC-RCC6-Headless

Screenless MeshCore companion firmware for the Heltec RadioCore RCC6-L62/SX1262. Choose one transport: Bluetooth, native USB/serial, or Wi-Fi Web/TCP.

Current experimental release: [`v1.0.0-rc.5`](https://github.com/n30nex/NeonPocketMC-RCC6-Headless/releases/tag/v1.0.0-rc.5). RC5 makes the Wi-Fi Web login explicit through setup, restart, and USB-console recovery while retaining the saved-location and battery safeguards.

> **RCC6-L62 only. Do not flash RC52, RC32, T114, Heltec V3/V4, or an RCC6 with different radio hardware.**

**Guided install:** [flasher.canadaverse.org](https://flasher.canadaverse.org/)

## Choose one image

| Build | Target | Best for |
|---|---|---|
| BLE | `heltec_rcc6_headless_companion_ble` | Standard MeshCore phone apps. Default pairing PIN: **`123456`**. |
| USB/serial | `heltec_rcc6_headless_companion_usb` | A directly attached computer, Pi, kiosk, or gateway using the native USB companion protocol. |
| Wi-Fi Web/TCP | `heltec_rcc6_headless_companion_web` | Browser access plus the standard companion protocol on TCP port 5000. |

The transports are separate firmware images. BLE and Wi-Fi never run together.

## What is included

- Current RCC6 SX1262 mapping, DIO flash mode, DIO2 RF switching, DIO3 1.8 V TCXO, DC-DC mode, and boosted RX gain correction
- 350 contacts, 40 channels, and 256 queued companion frames
- Fail-closed storage mounting: existing nonblank MeshCore data is never formatted after a mount failure
- Existing MeshCore identity, contacts, channels, and preferences preserved by normal app-only updates
- No TFT initialization, framebuffer, visual UI, repeater, MQTT observer, or room-server behavior

This repository follows the current [NeonPocketMC RCC6](https://github.com/n30nex/NeonPocketMC-RCC6) source line. The TFT Ultimate Companion remains a separate product.

## BLE

Pair from a standard MeshCore companion app using **`123456`**. A previously saved nonzero MeshCore BLE PIN still overrides the default.

## Native USB/serial

This image exposes the standard binary MeshCore companion protocol over the RCC6 native USB CDC port. It is not a text command console while a companion owns the connection.

## Wi-Fi Web/TCP

On first boot the device starts a WPA-protected setup network named `MeshCore-<node-name>`.

1. Keep USB connected and open the 115200-baud console in the guided flasher.
2. Reset the RCC6.
3. Read the setup SSID, eight-character password, and `http://192.168.4.1/` URL from the console.
4. Join that network, open the URL, and enter the local 2.4 GHz Wi-Fi details.
5. Save the LAN Web login shown before restart: username **`meshcore`**, password the generated eight-letter **device key**.
6. After restart, the USB console prints the assigned LAN address and the same login. The device key is not your home Wi-Fi password.

The Web interface is authenticated on the LAN. TCP port **5000** remains enabled for standard MeshCore companion clients.

The browser-location action asks separately whether the saved coordinates should be included in MeshCore adverts. The confirmation states whether the location will be shared or kept private; it never enables sharing in the background.

> **Trusted-LAN warning:** TCP/5000 provides the full companion/admin protocol without HTTP authentication. Any client that can reach that port must be trusted.

## Flashing

Use the guided flasher or follow [docs/FLASHING.md](docs/FLASHING.md). Normal installation uses the app-only image at `0x10000` and preserves bootloader, partition table, NVS, and MeshCore storage.

Recovery images are for a board that does not already have the expected RCC6 partition layout. They rewrite bootloader, partition table, and application, reset NVS/BLE bonds/Web network settings, and leave the later SPIFFS MeshCore data partition untouched.

## Other screenless roles already available

Do not install a companion image when the device should autonomously relay or serve rooms:

- [NeonPocketMC-RCC6-Repeater](https://github.com/n30nex/NeonPocketMC-RCC6-Repeater): headless MQTT observer/repeater plus minimal and full headless Room Server builds
- [NeonPocketMC-RC52-Repeater](https://github.com/n30nex/NeonPocketMC-RC52-Repeater): headless RC52 repeater and Room Server builds

Those existing products are reused by the NeonPocket suite; this repository does not duplicate them.

## Build verification

The Pi-local release checks (mirrored by GitHub Actions when available):

1. runs upstream native tests;
2. deterministically verifies the embedded WebUI;
3. builds all three exact headless targets;
4. verifies DIO ESP application and identity-preserving recovery layouts;
5. regression-builds the TFT Ultimate BLE companion; and
6. publishes exact build artifacts with SHA-256 manifests.

## License

This is a derivative of [MeshCore](https://github.com/meshcore-dev/MeshCore). Preserve the upstream and third-party license notices when redistributing source or binaries. See [license.txt](license.txt) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

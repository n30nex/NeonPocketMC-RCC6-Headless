# RCC6 Headless companion flashing

> **RCC6-L62 only—do not flash RC52, RC32, T114, Heltec V3/V4, or other RCC6 hardware.**

## Normal app-only update

1. Confirm the connected USB device is the intended RCC6 and record its USB identity.
2. Download one app image and `SHA256SUMS.txt` from the same release.
3. Verify the SHA-256 digest.
4. Attach a suitable 868/915 MHz antenna before LoRa transmission.
5. Put the RCC6 in ROM download mode.
6. Flash the app image at **`0x10000`** without erasing the chip.
7. Reset once and verify the selected transport plus LoRa TX/RX.

App-only updates preserve bootloader, partition table, NVS, BLE bonds, Web credentials, MeshCore identity, contacts, channels, and preferences.

## Recovery image

Use the matching recovery image at **`0x0`** only when the board lacks the expected RCC6 bootloader/partition layout or the app-only image cannot boot. Do not erase the whole flash first.

The recovery file contains bootloader, partition table, and application. Its gap resets NVS/BLE bonds/Web network settings. It ends before the SPIFFS data partition, so MeshCore identity, contacts, channels, and preferences are preserved when the file is written at the documented address without a whole-chip erase.

If the device resets repeatedly, storage will not mount, or USB identity changes unexpectedly, stop and capture the 115200-baud boot log instead of repeatedly flashing it.

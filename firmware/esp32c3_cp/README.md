# ESP32-C3 ESP-Hosted CP firmware

This ESP-IDF project uses the official `espressif/esp_hosted` component pinned to **v3.0.9**. It runs the coprocessor side with SPI Full-Duplex, mode 3, checksum, MCU RPC v2 and Wi-Fi enabled. The STM32 host component now implements INIT negotiation, RPC v2 and an optional lwIP Ethernet netif. A successful firmware build alone does not establish end-to-end Wi-Fi connectivity; verify the STM32 DHCP, DNS, TCP and UDP path on hardware.

Cold-start ordering: `sdkconfig.defaults` disables the upstream pre-`app_main()` ESP-Hosted constructor. `main/app_main.c` initializes NVS and the default event loop before explicitly calling `esp_hosted_init()`. This is intended to prevent an early Wi-Fi init RPC from reaching an uninitialized NVS store (`ESP_ERR_NVS_NOT_INITIALIZED`, `0x1101`). Keep this order when changing the CP startup. The updated firmware was flashed and passed one observed cold-start end-to-end run on 2026-09-28: no `0x1101` or brownout, with two DHCP/DNS/TCP/UDP passes around one deliberate STA disconnect. Repeat power-cycle and long-duration runs before claiming stability.

The GPIO assignment in [`sdkconfig.defaults.esp32c3`](sdkconfig.defaults.esp32c3) was checked against the connected STM32H723 schematic: ESP GPIO7=MOSI, GPIO2=MISO, GPIO6=CLK, GPIO10=CS, GPIO3=handshake, GPIO4=data ready, and STM32 PC5 drives ESP EN. The module is ESP32-C3-WROOM-02-N4 with 4 MB flash. Confirm the host USB-UART COM port before flashing.

In an activated ESP-IDF shell (tested installation discovery: IDF 6.0.2 at `D:\esp\v6.0.2\esp-idf`):

```powershell
cd firmware/esp32c3_cp
idf.py set-target esp32c3
idf.py build
# After identifying the USB-UART serial port:
# idf.py -p COMx flash monitor
```

The generated `build/flasher_args.json` lists exact image offsets, and `build/stm_esp32c3_cp.bin` is the application image. The bootloader and partition table images are also necessary on a blank device. The project downloads the pinned official dependency through ESP-IDF's Component Manager; it does not copy the upstream CP repository into this project.

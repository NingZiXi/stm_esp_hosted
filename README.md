# stm_esp_hosted

STM32 裸机 ESP-Hosted SPI 主机组件。当前版本针对 ESP32-C3 上的 ESP-Hosted CP **3.0.9**、RPC v2、SPI Full-Duplex Mode 3、1600 字节 V1 帧和 STA 模式。组件不负责 HAL 外设初始化或凭据持久化。

## 组成与依赖

`stm_esp_hosted` 是独立静态库，依赖 `stm_common` 和调用者提供的 HAL SPI/GPIO/毫秒时钟；它负责 INIT 能力协商、固件版本检查、Wi-Fi RPC、STA MAC、连接/断开、链路事件和完整 Ethernet 帧收发。调用者提供 32 字节对齐、DMA 可访问的两个 1600 字节缓冲区；STM32H7 开启 D-Cache 时组件维护其缓存。当前 SPI 交换使用阻塞式 `HAL_SPI_TransmitReceive()`，在主循环中频繁轮询。

工程已定义 `lwip` 目标时额外构建 `stm_esp_hosted_lwip`，将 STA_IF 帧连接到 lwIP Ethernet netif。纯传输/控制应用只链接 `stm_esp_hosted`。适配库不内嵌或下载 lwIP 源码；应用从官方 `STABLE-2_2_1_RELEASE` 获取，并使用 `NO_SYS=1`。不需要 STM32 ETH 外设。

```cmake
add_subdirectory(Lib/stm_esp_hosted)
target_link_libraries(my_app PRIVATE stm_esp_hosted_lwip) # 或仅 stm_esp_hosted
```

## 调用顺序

1. 由 CubeMX 初始化 SPI Mode 3、CS/Reset/Handshake/Data Ready GPIO；准备两个 1600 字节 DMA 缓冲区。
2. `esp_hosted_create()`、`esp_hosted_start(handle, timeout_ms)`；INIT 协商后检查 CP 版本 3.0.9。随后 `esp_hosted_get_sta_mac()` 初始化 Wi-Fi 并读取 STA MAC。
3. 若使用 lwIP：`lwip_init()`、`esp_hosted_lwip_prepare()`、`netif_add(..., esp_hosted_lwip_netif_init, ethernet_input)`、`netif_set_default()`、`netif_set_up()`、`esp_hosted_lwip_attach()`。
4. `esp_hosted_connect()` 配置本地凭据并等待连接事件。主循环频繁执行 `esp_hosted_poll()`、`sys_check_timeouts()`。在链路上线后应用启动 DHCP；断线时停止 DHCP、清空地址、重连后重新获取地址。
5. 不用 lwIP 时可通过 `esp_hosted_set_callbacks()` 接收 STA 帧/链路事件，`esp_hosted_send()` 发送完整 Ethernet 帧。回调只在同步轮询/发送路径触发，接收数据的指针只在回调期有效。应用退出时先移除 netif，再 `esp_hosted_delete()`。

凭据放到被忽略的本地构建头文件，**不要放入组件源码、日志或公开仓库**。`esp_hosted_connect()` 不输出凭据。一次发送最长 1514 字节，过长和短于 Ethernet 头部的帧会被拒绝。当前 API 不支持并发调用或在回调内再次调用组件发送 API。

## 板级参考与验证

STM32H723 示例见 本地 `stm_h723_demo` 工程的 `main/app_main.c`。ESP32-C3 的参考固件工程位于本仓库 `firmware/esp32c3_cp/`。该板连接 SPI1 MOSI=PD7、MISO=PA6、SCLK=PG11、CS=PC4、Handshake=PA2、Data Ready=PA3、Reset/EN=PC5；ESP32-C3 对应 GPIO7/2/6/10/3/4。其他板请核对原理图并更换配置。

```sh
cmake -S tests -B build/stm_esp_hosted_tests -G Ninja
cmake --build build/stm_esp_hosted_tests
ctest --test-dir build/stm_esp_hosted_tests --output-on-failure
```

测试覆盖帧校验和边界、RPC 异常事件、INIT 超时、STA 链路和接收，以及用轻量 lwIP API 仿真器验证 `pbuf` 链收发。使用本地忽略的凭据和回显服务器时，STM32 实板已完成 DHCP、DNS、TCP/UDP 原样回显以及主动断线后重新获取地址的验证。此前冷启动偶发的 `STM_ERR_IO` 对应 CP Wi-Fi init RPC（278）返回 `0x1101`（NVS 未初始化）。CP 工程已改为先初始化 NVS 和事件循环、再显式启动 ESP-Hosted；2026-09-28 烧录后的单次冷启动观察中未再出现该错误或 brownout，并完成两轮 DHCP、DNS 和 TCP/UDP 回显（包含一次主动断线重连）。仍需多次断电重启与长时间运行来评估稳定性。模拟测试和固件构建单独不能证明联网验收。AP、BLE、OTA 和 RTOS 不在 v0.1.0 范围。

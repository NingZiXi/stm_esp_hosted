# stm_esp_hosted

STM32 裸机 ESP-Hosted SPI 主机组件。当前版本针对 ESP32-C3 上的 ESP-Hosted CP **3.0.9**、RPC v2、SPI Full-Duplex Mode 3、1600 字节 V1 帧，支持 STA、扫描、AP 与 AP+STA。组件不负责 HAL 外设初始化或凭据持久化。

## 组成与依赖

`stm_esp_hosted` 是独立静态库，依赖 `stm_common` 和调用者提供的 HAL SPI/GPIO/毫秒时钟；它负责 INIT 能力协商、固件版本检查、Wi-Fi RPC、STA/AP MAC、连接/断开、扫描、AP 客户端事件和完整 Ethernet 帧收发。调用者提供 32 字节对齐、DMA 可访问的两个 1600 字节缓冲区；STM32H7 开启 D-Cache 时组件维护其缓存。当前 SPI 交换使用阻塞式 `HAL_SPI_TransmitReceive()`，在主循环中频繁轮询。

工程已定义 `lwip` 目标时额外构建 `stm_esp_hosted_lwip`，将 STA_IF 和可选 AP_IF 帧连接到独立的 lwIP Ethernet netif，并提供一个适合小规模演示的四租约 AP DHCP 服务。纯传输/控制应用只链接 `stm_esp_hosted`。适配库不内嵌或下载 lwIP 源码；应用从官方 `STABLE-2_2_1_RELEASE` 获取，并使用 `NO_SYS=1`。不需要 STM32 ETH 外设。

```cmake
add_subdirectory(Lib/stm_esp_hosted)
target_link_libraries(my_app PRIVATE stm_esp_hosted_lwip) # 或仅 stm_esp_hosted
```

## 调用顺序

1. CubeMX 初始化 SPI Mode 3、CS/Reset/Handshake/Data Ready GPIO；准备两个 32 字节对齐且 DMA 可访问的 1600 字节缓冲区。调用 `esp_hosted_create()`、`esp_hosted_start(handle, timeout_ms)` 完成 INIT/RPC 协商和 CP 版本检查；`esp_hosted_get_version()` 可查询版本。
2. 调用 `eh_wifi_init()`、`eh_wifi_set_mode(handle, EH_WIFI_MODE_STA, timeout_ms)`、`eh_wifi_get_mac(handle, EH_WIFI_IF_STA, mac)` 和 `eh_wifi_start()`。用 `eh_wifi_set_config(handle, EH_WIFI_IF_STA, &config, timeout_ms)` 设置 STA 参数，再调用 `eh_wifi_connect()`。连接及扫描是异步过程，通过 `eh_wifi_set_event_callback()` 接收事件，并频繁调用 `esp_hosted_poll()`。`eh_wifi_connect()` 返回成功仅表示请求被接受，关联结果以事件及 `eh_wifi_get_status()` 的 `sta_connected` 为准；此接口返回的模式、启动、STA/AP 链路、扫描中状态和最近断线原因均为缓存快照，不发起 RPC。断线后可以重新配置并连接。关联后可用 `eh_wifi_sta_get_ap_info()` 查询当前 AP 的 SSID、BSSID、信道、RSSI 和认证方式；未关联时返回状态错误。
3. 若使用 lwIP：先 `lwip_init()`，再以 STA MAC 调用 `esp_hosted_lwip_prepare()`、`netif_add(..., esp_hosted_lwip_netif_init, ethernet_input)`、`netif_set_default()`、`netif_set_up()` 和 `esp_hosted_lwip_attach()`。主循环每次 `esp_hosted_poll()` 后调用 `esp_hosted_lwip_sta_update()`，适配层按链路启停 DHCP，断线时清除旧 IPv4 地址，重连后重新取址；用 `esp_hosted_lwip_sta_has_address()` 判断 DHCP 是否已提供地址。应用继续负责 `lwip_init()`、网卡添加与默认网卡选择，以及 `sys_check_timeouts()`；移除网卡前调用 `esp_hosted_lwip_sta_stop()`。
4. 可选扫描：`eh_wifi_scan_start(handle, &scan_config, timeout_ms)` 启动，收到 `EH_WIFI_EVENT_SCAN_DONE` 后检查事件中的 `scan_status`（零表示成功），成功时用 `eh_wifi_scan_get_results()` 读取有限容量的记录；超时则调用 `eh_wifi_scan_stop()`，随后可重新扫描。结果包含 SSID、BSSID、信道、RSSI 和认证模式。
5. 可选 AP：模式设为 `EH_WIFI_MODE_AP` 或 `EH_WIFI_MODE_APSTA`，用 `eh_wifi_get_mac(handle, EH_WIFI_IF_AP, ap_mac)`、`eh_wifi_set_config(handle, EH_WIFI_IF_AP, &ap_config, timeout_ms)` 配置，并调用 `eh_wifi_start()`。如需 IP 通信，使用 `esp_hosted_lwip_ap_prepare()`、`netif_add(..., esp_hosted_lwip_ap_netif_init, ethernet_input)` 和 `esp_hosted_lwip_ap_attach()` 建立 AP netif。应用自行指定 AP 地址、掩码；主循环每次 `esp_hosted_poll()` 后调用 `esp_hosted_lwip_dhcps_update()`，适配层随 AP 链路启停 DHCP 服务。移除网卡前调用 `esp_hosted_lwip_dhcps_stop()`。内置 DHCP 仅为 /24 子网分配 `.100` 至 `.103` 四个地址，适合最小演示，不作为通用 DHCP 服务。
6. 不用 lwIP 时，`esp_hosted_set_callbacks()` 接收 STA 帧与链路变化、`esp_hosted_send()` 发送 STA Ethernet 帧；AP 对应 `eh_wifi_set_ap_rx_callback()`、`eh_wifi_set_ap_link_callback()` 和 `eh_wifi_ap_send()`。回调在同步轮询/发送路径触发，接收帧指针仅在回调期间有效。使用 lwIP 时释放前停止 DHCP、移除网卡，再调用 `esp_hosted_delete()`。

凭据存放在应用的本地忽略配置中，不要写入组件源码、公开仓库或日志。STA/AP 一帧最长 1514 字节；短于 Ethernet 头部或超过上限的帧会被拒绝。当前 API 不支持并发调用，也不要在组件回调内重入发送或控制 API。AP 演示已通过单客户端 HTTP/TCP 和手机 UDP 回显；多个客户端并发和长期运行尚未覆盖。

## 板级参考与验证

STM32H723 板级示例位于配套 `stm_h723_demo` 工程的 `main/app_main.c`。ESP32-C3 的参考固件工程位于本仓库 `firmware/esp32c3_cp/`。该板连接 SPI1 MOSI=PD7、MISO=PA6、SCLK=PG11、CS=PC4、Handshake=PA2、Data Ready=PA3、Reset/EN=PC5；ESP32-C3 对应 GPIO7/2/6/10/3/4。其他板请核对原理图并更换配置。

```sh
cmake -S tests -B build/stm_esp_hosted_tests -G Ninja
cmake --build build/stm_esp_hosted_tests
ctest --test-dir build/stm_esp_hosted_tests --output-on-failure
```

测试覆盖帧校验和边界、RPC 异常与超时、扫描错误重试、STA/AP 事件、DHCP 租约分配/续租/冲突/释放，以及轻量 lwIP 仿真中的 `pbuf` 链收发。STM32 实板已验证 STA 获取 DHCP 地址、DNS、TCP/UDP 回显与主动断线重连。此前 CP Wi-Fi init RPC（278）曾因 NVS 未初始化返回 `0x1101`；CP 工程改为先初始化 NVS 和事件循环，再启动 ESP-Hosted。2026-09-28 的阶段性测试中，20 次 J-Link 复位均在 90 秒内完成全链路验证；持续联网约 47 分钟完成 49 轮回显，未达到原计划的 2 小时/120 轮，也未做断电循环。

AP+STA 实板功能测试中，首次扫描返回 3 条记录，最终固件复测返回 4 条记录，电脑关联测试 AP 后获得 `192.168.40.100/24`，网关 `192.168.40.1`，三次 UDP 数据均从 `192.168.40.1:24681` 原样回显。切换测试电脑的无线网络后，STA 测试服务器不可达，因此该切换后的回显失败不计入持续运行稳定性结论。2026-09-29，手机连接测试 AP 后，在浏览器访问 `http://192.168.40.1/` 成功显示 `STM32 AP OK`；RTT 记录客户端接入和来自 `192.168.40.100` 的两次 HTTP GET。该页面验证单客户端 AP TCP/HTTP 通路。手机向 AP 的 UDP 回显端口发送数据时，每次发送均收到一条对应回包；板端 RTT 连续记录 `AP UDP echo 47 bytes`，47 字节是手机应用实际报文长度，不代表仅发送了五字节文本。此项只验证 AP UDP 双向通信，不计为长期稳定性测试。多个客户端、AP 长期运行、BLE、OTA 和 RTOS 仍未验证或实现。原始日志与本地凭据不随组件发布。

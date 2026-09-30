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
2. 调用 `eh_wifi_init()`、`eh_wifi_set_mode(handle, EH_WIFI_MODE_STA, timeout_ms)`、`eh_wifi_get_mac(handle, EH_WIFI_IF_STA, mac)` 和 `eh_wifi_start()`。用 `eh_wifi_set_config(handle, EH_WIFI_IF_STA, &config, timeout_ms)` 设置 STA 参数，再调用 `eh_wifi_connect()`。连接及扫描是异步过程，通过 `eh_wifi_set_event_callback()` 接收事件，并频繁调用 `esp_hosted_poll()`。`eh_wifi_connect()` 返回成功仅表示请求被接受，关联结果以事件及 `eh_wifi_get_status()` 的 `sta_connected` 为准；此接口返回的模式、启动、STA/AP 链路、扫描中状态和最近断线原因均为缓存快照，不发起 RPC。`eh_wifi_get_mode()` 则使用 RPC 259 读取 CP 的实际模式。断线后可以重新配置并连接。关联后可用 `eh_wifi_sta_get_ap_info()` 查询当前 AP 的 SSID、BSSID、信道、RSSI 和认证方式；未关联时返回状态错误。
3. 若使用 lwIP：先 `lwip_init()`，再以 STA MAC 调用 `esp_hosted_lwip_prepare()`、`netif_add(..., esp_hosted_lwip_netif_init, ethernet_input)`、`netif_set_default()`、`netif_set_up()` 和 `esp_hosted_lwip_attach()`。主循环每次 `esp_hosted_poll()` 后调用 `esp_hosted_lwip_sta_update()`，适配层按链路启停 DHCP，断线时清除旧 IPv4 地址，重连后重新取址；用 `esp_hosted_lwip_sta_has_address()` 判断 DHCP 是否已提供地址。应用继续负责 `lwip_init()`、网卡添加与默认网卡选择，以及 `sys_check_timeouts()`；移除网卡前调用 `esp_hosted_lwip_sta_stop()`。
4. 可选扫描：`eh_wifi_scan_start(handle, &scan_config, timeout_ms)` 启动，收到 `EH_WIFI_EVENT_SCAN_DONE` 后检查事件中的 `scan_status`（零表示成功），成功时用 `eh_wifi_scan_get_results()` 读取有限容量的记录；超时则调用 `eh_wifi_scan_stop()`，随后可重新扫描。结果包含 SSID、BSSID、信道、RSSI 和认证模式。
5. 可选 AP：模式设为 `EH_WIFI_MODE_AP` 或 `EH_WIFI_MODE_APSTA`，用 `eh_wifi_get_mac(handle, EH_WIFI_IF_AP, ap_mac)`、`eh_wifi_set_config(handle, EH_WIFI_IF_AP, &ap_config, timeout_ms)` 配置，并调用 `eh_wifi_start()`。如需 IP 通信，使用 `esp_hosted_lwip_ap_prepare()`、`netif_add(..., esp_hosted_lwip_ap_netif_init, ethernet_input)` 和 `esp_hosted_lwip_ap_attach()` 建立 AP netif。应用自行指定 AP 地址、掩码；主循环每次 `esp_hosted_poll()` 后调用 `esp_hosted_lwip_dhcps_update()`，适配层随 AP 链路启停 DHCP 服务。移除网卡前调用 `esp_hosted_lwip_dhcps_stop()`。内置 DHCP 仅为 /24 子网分配 `.100` 至 `.103` 四个地址，适合最小演示，不作为通用 DHCP 服务。
6. 不用 lwIP 时，`esp_hosted_set_callbacks()` 接收 STA 帧与链路变化、`esp_hosted_send()` 发送 STA Ethernet 帧；AP 对应 `eh_wifi_set_ap_rx_callback()`、`eh_wifi_set_ap_link_callback()` 和 `eh_wifi_ap_send()`。回调在同步轮询/发送路径触发，接收帧指针仅在回调期间有效。使用 lwIP 时释放前停止 DHCP、移除网卡，再调用 `esp_hosted_delete()`。

## 配置读回与 STA 省电

`eh_wifi_get_config(handle, EH_WIFI_IF_STA, &info, timeout_ms)` 和 AP 接口对应的查询使用 RPC 285。结果类型 `eh_wifi_config_info_t` 只包含 SSID，以及 AP 的信道、隐藏状态、最大连接数和认证方式；它没有密码字段，也不表示其他高级配置已受支持。CP 的原始响应可能含密码，组件在解析后清除响应缓冲。查询失败、超时或报文异常时不要使用输出参数，成功才有完整结果。

STA 模式下，`eh_wifi_set_ps()` 与 `eh_wifi_get_ps()` 分别通过 RPC 270/271 操作 CP 的省电模式，支持 `EH_WIFI_PS_NONE`、`EH_WIFI_PS_MIN_MODEM` 和 `EH_WIFI_PS_MAX_MODEM`。组件不会自动更改 CP 默认值；设置调用返回成功后可再次查询确认实际值。配置读回、省电查询与本地状态快照的调用方式如下：

```c
eh_wifi_mode_t mode;
eh_wifi_config_info_t info;
eh_wifi_ps_t ps;
if (eh_wifi_get_mode(host, &mode, 5000U) == STM_OK && mode == EH_WIFI_MODE_STA &&
    eh_wifi_get_config(host, EH_WIFI_IF_STA, &info, 5000U) == STM_OK &&
    eh_wifi_set_ps(host, EH_WIFI_PS_MIN_MODEM, 5000U) == STM_OK &&
    eh_wifi_get_ps(host, &ps, 5000U) == STM_OK && ps == EH_WIFI_PS_MIN_MODEM) {
    /* info.ssid 是不含密码的配置读回；继续轮询和处理 lwIP 定时器。 */
}
```

这些同步 RPC 应在完成 `eh_wifi_init()` 和相应模式设置后调用。省电模式需要 STA 或 AP+STA 模式。查询与控制接口不应从组件回调内重入。

凭据存放在应用的本地忽略配置中，不要写入组件源码、公开仓库或日志。STA/AP 一帧最长 1514 字节；短于 Ethernet 头部或超过上限的帧会被拒绝。当前 API 不支持并发调用，也不要在组件回调内重入发送或控制 API。AP 演示已通过单客户端 HTTP/TCP 和手机 UDP 回显；多个客户端并发和长期运行尚未覆盖。

## 板级参考与验证

STM32H723 板级示例位于配套 `stm_h723_demo` 工程的 `main/app_main.c`。ESP32-C3 的参考固件工程位于本仓库 `firmware/esp32c3_cp/`。该板连接 SPI1 MOSI=PD7、MISO=PA6、SCLK=PG11、CS=PC4、Handshake=PA2、Data Ready=PA3、Reset/EN=PC5；ESP32-C3 对应 GPIO7/2/6/10/3/4。其他板请核对原理图并更换配置。

```sh
cmake -S tests -B build/stm_esp_hosted_tests -G Ninja
cmake --build build/stm_esp_hosted_tests
ctest --test-dir build/stm_esp_hosted_tests --output-on-failure
```

测试覆盖帧校验和边界、RPC 异常与超时、扫描错误重试、STA/AP 事件、模式/配置/省电模式读回的成功与异常响应、密码不外露、DHCP 租约分配/续租/冲突/释放，以及轻量 lwIP 仿真中的 `pbuf` 链收发。STM32 实板已验证 STA 获取 DHCP 地址、DNS、TCP/UDP 回显与主动断线重连。此前 CP Wi-Fi init RPC（278）曾因 NVS 未初始化返回 `0x1101`；CP 工程改为先初始化 NVS 和事件循环，再启动 ESP-Hosted。2026-09-28 的阶段性测试中，20 次 J-Link 复位均在 90 秒内完成全链路验证。2026-09-29 的纯 STA 持续运行从首次全链路通过起保持 7200 秒，121 轮 TCP/UDP 周期回显全部通过、0 失败；期间一次主动断线后约 9.9 秒恢复，并重新获得 DHCP 地址及完成 DNS 查询。整板断电重启的独立结果见下文。

AP+STA 实板功能测试中，首次扫描返回 3 条记录，最终固件复测返回 4 条记录，电脑关联测试 AP 后获得 `192.168.40.100/24`，网关 `192.168.40.1`，三次 UDP 数据均从 `192.168.40.1:24681` 原样回显。切换测试电脑的无线网络后，STA 测试服务器不可达，因此该切换后的回显失败不计入持续运行稳定性结论。2026-09-29，手机连接测试 AP 后，在浏览器访问 `http://192.168.40.1/` 成功显示 `STM32 AP OK`；RTT 记录客户端接入和来自 `192.168.40.100` 的两次 HTTP GET。该页面验证单客户端 AP TCP/HTTP 通路。手机向 AP 的 UDP 回显端口发送数据时，每次发送均收到一条对应回包；板端 RTT 连续记录 `AP UDP echo 47 bytes`，47 字节是手机应用实际报文长度，不代表仅发送了五字节文本。此项只验证 AP UDP 双向通信，不计为长期稳定性测试。多个客户端、AP 长期运行、BLE、OTA 和 RTOS 仍未验证或实现。原始日志与本地凭据不随组件发布。

2026-09-29 至 09-30 的 v0.3.0 实板回归：STA 配置与模式查询成功，`NONE`、`MIN_MODEM`、`MAX_MODEM` 各设置及读回成功；每种模式以约 60 秒间隔进行 10 轮 DHCP 地址保持、DNS、TCP/UDP 回显，合计 30/30 轮通过。主动断线后重新取址并恢复 DNS、TCP/UDP。AP+STA 模式及 AP 配置读回成功，板端 DHCP 启动；手机获取租约、访问 HTTP 页面并收到 UDP 回显，板端记录对应客户端、HTTP 和 UDP 事件。此项功能回归与后续整板断电重启分别统计。

2026-09-30 的整板断电重启验收共观察 13 次：10 次有效通过，均重新完成 CP 3.0.9 INIT、STA 关联、DHCP、DNS 及 TCP/UDP 回显；其中还观察到一次欠压启动失败、一次采集缺口无法判定、一次无法证明整板重启且网络验证失败。10 次通过并非连续 10 次无故障通过。最后一次有效循环从 ESP 串口断连到首轮全链路通过约 33.5 秒；主动断线后的 DHCP 重新取址、DNS 和 TCP/UDP 回显恢复亦通过。部分循环在 USB 重新枚举期间未采集到 CP ROM 启动文本，因此不据此推断上电瞬态均正常。原始日志和逐次记录仅留在本地构建目录，不随组件发布。

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
2. 调用 `eh_wifi_init()`、`eh_wifi_set_mode(handle, EH_WIFI_MODE_STA, timeout_ms)`、`eh_wifi_get_mac(handle, EH_WIFI_IF_STA, mac)` 和 `eh_wifi_start()`。用 `eh_wifi_set_config(handle, EH_WIFI_IF_STA, &config, timeout_ms)` 设置 STA 参数，再调用 `eh_wifi_connect()`。连接及扫描是异步过程，通过 `eh_wifi_set_event_callback()` 接收事件，并频繁调用 `esp_hosted_poll()`。`eh_wifi_connect()` 返回成功仅表示请求被接受，关联结果以事件及 `eh_wifi_get_status()` 的 `sta_connected` 为准；此接口返回的模式、启动、STA/AP 链路、扫描中状态和最近断线原因均为缓存快照，不发起 RPC。`eh_wifi_get_mode()` 则使用 RPC 259 读取 CP 的实际模式。应用可选择启用下述自动重连策略；不启用时仍可自行重新连接。关联后可用 `eh_wifi_sta_get_ap_info()` 查询当前 AP 的 SSID、BSSID、信道、RSSI 和认证方式；未关联时返回状态错误。
3. 若使用 lwIP：先 `lwip_init()`，再以 STA MAC 调用 `esp_hosted_lwip_prepare()`、`netif_add(..., esp_hosted_lwip_netif_init, ethernet_input)`、`netif_set_default()`、`netif_set_up()` 和 `esp_hosted_lwip_attach()`。主循环每次 `esp_hosted_poll()` 后调用 `esp_hosted_lwip_sta_update()`，适配层按链路启停 DHCP，断线时清除旧 IPv4 地址，重连后重新取址；用 `esp_hosted_lwip_sta_has_address()` 判断 DHCP 是否已提供地址。应用继续负责 `lwip_init()`、网卡添加与默认网卡选择，以及 `sys_check_timeouts()`；移除网卡前调用 `esp_hosted_lwip_sta_stop()`。
4. 可选扫描：`eh_wifi_scan_start(handle, &scan_config, timeout_ms)` 启动，收到 `EH_WIFI_EVENT_SCAN_DONE` 后检查事件中的 `scan_status`（零表示成功），成功时用 `eh_wifi_scan_get_results()` 读取有限容量的记录；超时则调用 `eh_wifi_scan_stop()`，随后可重新扫描。结果包含 SSID、BSSID、信道、RSSI 和认证模式。
5. 可选 AP：模式设为 `EH_WIFI_MODE_AP` 或 `EH_WIFI_MODE_APSTA`，用 `eh_wifi_get_mac(handle, EH_WIFI_IF_AP, ap_mac)`、`eh_wifi_set_config(handle, EH_WIFI_IF_AP, &ap_config, timeout_ms)` 配置，并调用 `eh_wifi_start()`。如需 IP 通信，使用 `esp_hosted_lwip_ap_prepare()`、`netif_add(..., esp_hosted_lwip_ap_netif_init, ethernet_input)` 和 `esp_hosted_lwip_ap_attach()` 建立 AP netif。应用自行指定 AP 地址、掩码；主循环每次 `esp_hosted_poll()` 后调用 `esp_hosted_lwip_dhcps_update()`，适配层随 AP 链路启停 DHCP 服务。移除网卡前调用 `esp_hosted_lwip_dhcps_stop()`。内置 DHCP 仅为 /24 子网分配 `.100` 至 `.103` 四个地址，适合最小演示，不作为通用 DHCP 服务。
6. 不用 lwIP 时，`esp_hosted_set_callbacks()` 接收 STA 帧与链路变化、`esp_hosted_send()` 发送 STA Ethernet 帧；AP 对应 `eh_wifi_set_ap_rx_callback()`、`eh_wifi_set_ap_link_callback()` 和 `eh_wifi_ap_send()`。回调在同步轮询/发送路径触发，接收帧指针仅在回调期间有效。使用 lwIP 时释放前停止 DHCP、移除网卡，再调用 `esp_hosted_delete()`。

## STA 自动重连

自动重连默认关闭。完成 `eh_wifi_start()`、设置 STA 配置后，调用 `eh_wifi_set_reconnect()` 启用策略，仍须由应用调用一次 `eh_wifi_connect()` 发起首次连接。之后主循环在 `esp_hosted_poll()` 和 `esp_hosted_lwip_sta_update()` 后调用 `eh_wifi_reconnect_update()`；其到期时会执行同步连接 RPC，因此不要在事件回调内调用。连接请求成功仅表示 CP 已接受请求，STA 关联、DHCP 和上层数据通路需分别确认。

```c
eh_wifi_reconnect_config_t retry = {
    .enabled = 1U,
    .initial_delay_ms = 1000U,
    .max_delay_ms = 30000U,
    .association_timeout_ms = 15000U,
    .rpc_timeout_ms = 5000U,
    .max_attempts = 0U, /* 0 表示不限次数。 */
};
if (eh_wifi_set_reconnect(host, &retry) == STM_OK) {
    (void)eh_wifi_connect(host, 5000U);
}
/* 主循环：esp_hosted_poll(host); esp_hosted_lwip_sta_update(&adapter);
 * eh_wifi_reconnect_update(host); sys_check_timeouts(); */
```

连接断开或关联超时后按指数退避尝试，最长等待由 `max_delay_ms` 限制。`max_attempts` 计入自动尝试次数，不计首次显式连接；达到上限后停止自动尝试，显式调用 `eh_wifi_connect()` 可重新开始。`eh_wifi_get_status()` 的 `reconnect_enabled`、`reconnect_pending` 和 `reconnect_attempts` 是本地快照；`last_disconnect_reason` 保留 CP 最近一次断线原因。`eh_wifi_disconnect()`、`eh_wifi_stop()`、退出 STA 模式及禁用策略都会取消自动重连。应用切换网络时应显式断开、更新配置并重新发起连接。退避期间仍需持续轮询和处理 lwIP 定时器。

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

## 协议与带宽配置

`eh_wifi_set_protocol()` / `eh_wifi_get_protocol()` 对应 RPC 297/298，`eh_wifi_set_bandwidth()` / `eh_wifi_get_bandwidth()` 对应 RPC 299/300。四个接口均接收 `EH_WIFI_IF_STA` 或 `EH_WIFI_IF_AP` 和非零 `timeout_ms`，要求已完成 Wi-Fi 初始化且当前模式启用指定接口，不要求 STA 关联或 AP 已启动。在 AP+STA 模式中两接口可独立配置。

协议标志为 `EH_WIFI_PROTOCOL_11B=0x01`、`EH_WIFI_PROTOCOL_11G=0x02`、`EH_WIFI_PROTOCOL_11N=0x04`，只接受 B、BG、BGN 三种组合。带宽为 `EH_WIFI_BW_HT20=1`、`EH_WIFI_BW_HT40=2`；HT40 需要 11n。组件不为带宽设置额外查询协议，不自动调整组合、降级或恢复配置。无效参数返回 `STM_ERR_INVALID_ARG`，接口未启用返回 `STM_ERR_INVALID_STATE`，CP 拒绝返回 `STM_ERR_IO`；可用 `esp_hosted_get_info()` 查看最近 RPC 的 CP 状态。查询缺字段、重复字段、类型或数值错误返回 `STM_ERR_VERIFY`，超时返回 `STM_ERR_TIMEOUT`，超过查询缓冲容量返回 `STM_ERR_OUT_OF_RANGE`。查询任何失败均保持输出不变。

切换配置前保存各接口协议与带宽，停止 Wi-Fi 并更新 STA/AP 的 DHCP 生命周期，再先设置 HT20、设置协议、设置目标带宽并读回确认，最后启动 Wi-Fi、连接 STA。以下函数在初始化且模式已选定、Wi-Fi 已停止后从主循环调用：

```c
static stm_err_t configure_radio(esp_hosted_handle_t host, eh_wifi_if_t iface,
                                 uint8_t protocol, eh_wifi_bandwidth_t bandwidth)
{
    uint8_t actual_protocol = 0U;
    eh_wifi_bandwidth_t actual_bandwidth = EH_WIFI_BW_HT20;
    stm_err_t err = eh_wifi_set_bandwidth(host, iface, EH_WIFI_BW_HT20, 5000U);
    if (err == STM_OK) { err = eh_wifi_set_protocol(host, iface, protocol, 5000U); }
    if (err == STM_OK) { err = eh_wifi_set_bandwidth(host, iface, bandwidth, 5000U); }
    if (err == STM_OK) { err = eh_wifi_get_protocol(host, iface, &actual_protocol, 5000U); }
    if (err == STM_OK) { err = eh_wifi_get_bandwidth(host, iface, &actual_bandwidth, 5000U); }
    if (err == STM_OK && (actual_protocol != protocol || actual_bandwidth != bandwidth)) {
        err = STM_ERR_VERIFY;
    }
    return err;
}
```

从 BGN/HT40 切换到 B 或 BG 前同样先设置 HT20；恢复保存的配置也采用此顺序，并重新确认 DHCP、DNS 和应用通信。事件回调只标记任务，不执行同步 RPC。带宽读回表示配置值，实际带宽由协商与环境决定；HT40 读回成功不证明实际以 40 MHz 通信。LR、5 GHz、11ax 与其他带宽尚未支持。

## 异常诊断与传输恢复

`esp_hosted_set_monitor()` 用 RPC 277 配置 CP 心跳，事件 770 刷新本地计时；默认关闭。CP 3.0.9 支持 10 至 3600 秒周期，启用时本地超时必须大于两个周期且不超过 `INT32_MAX` 毫秒。示例采用 10 秒周期、35 秒超时：

```c
esp_hosted_monitor_config_t monitor = {
    .enabled = 1U, .interval_s = 10U, .timeout_ms = 35000U,
};
stm_err_t err = esp_hosted_set_monitor(host, &monitor, 5000U);
```

`esp_hosted_get_diagnostics()` 不发 RPC，返回本地传输状态、会话代次、最近故障及 HAL 毫秒时间、最近失败 RPC/CP 状态和累计计数。SPI 失败、帧/校验失败、RPC 超时、迟到响应、心跳超时及恢复成功/失败计数在句柄生命周期内饱和累计；恢复不清零，正常成功请求不覆盖最近故障。`READY` 只表示 INIT、CP 3.0.9 核对和已启用的心跳配置完成，Wi-Fi 与 IP 仍须另行恢复。

已就绪时收到有效新 INIT 或心跳超时会使旧会话失效并进入 `FAULT`。初始化/恢复中重复 INIT 合并处理。单次 RPC 超时、CP 拒绝、坏帧或 Data Ready 空闲不单独触发复位；心跳超时仅说明 CP 通路失联。失效会取消未完成 RPC，清除 Wi-Fi 初始化、连接、扫描、自动重连执行状态和临时数据，通知 STA/AP 链路下线。UID 不因恢复而重新从零分配，旧响应不得完成新请求；`esp_hosted_reset()` 同样清除旧会话状态。

`esp_hosted_recover_begin(host, timeout_ms)` 只启动异步恢复，不在入口等待启动完成。后续 `esp_hosted_poll()` 推进 EN 拉低 10 毫秒、启动等待、INIT 能力回包、版本核对及心跳重新配置；总超时覆盖全部步骤。同步 RPC 与恢复事务互斥，恢复中返回 `STM_ERR_INVALID_STATE`，组件回调中重入返回 `STM_ERR_INVALID_CONTEXT`。同步请求的超时也覆盖整个请求。

应用从主循环启动恢复，持续处理 lwIP 定时器，并检查诊断状态。到 `READY` 后逐步重放 Wi-Fi 初始化、模式、应用凭据、协议/带宽、国家策略、省电模式，再启动 Wi-Fi、设置功率、连接和取址；组件不保存密码，不自行复位或重放这些配置。例如：

```c
/* 在主循环的恢复入口调用一次。 */
stm_err_t err = esp_hosted_recover_begin(host, 10000U);
/* 后续每轮继续 esp_hosted_poll(host)、sys_check_timeouts()。 */
esp_hosted_diagnostics_t diagnostics;
if (esp_hosted_get_diagnostics(host, &diagnostics) == STM_OK &&
    diagnostics.state == ESP_HOSTED_STATE_READY) {
    /* 由应用状态机执行上述配置重放，再验证 DHCP/DNS/通信。 */
}
```

链路失效后调用 STA/AP 生命周期更新：STA 停止 DHCP、清除旧地址及 ARP；AP 停止 DHCP、清空租约及 ARP，同时保留静态地址和网卡对象。应用还须取消旧 DNS/回显任务、关闭对应 TCP/UDP 会话，恢复后建立新探针。配套演示的每次全链路恢复限时 90 秒，传输恢复每次 10 秒，失败间隔 5 秒、最多三次，超限明确停止。

## 国家策略、共享信道与功率

| 接口 | RPC | 前置条件与结果 |
| --- | --- | --- |
| `eh_wifi_set_country_code()` | 334 | Wi-Fi 已初始化；两字符大写国家码或世界安全模式 `01`，802.11d 参数为 0/1 |
| `eh_wifi_get_country_code()` | 335 | Wi-Fi 已初始化；输出 `char[4]`，容纳两字符代码、可选环境字符和终止符 |
| `eh_wifi_get_country()` | 304 | Wi-Fi 已初始化；返回代码、2.4 GHz 起始信道/数量、最大功率和 AUTO/MANUAL 策略 |
| `eh_wifi_set_channel()` | 301 | Wi-Fi 已启动；主信道 1..14，次信道使用现有 NONE/ABOVE/BELOW 枚举 |
| `eh_wifi_set_max_tx_power()` | 275 | Wi-Fi 已启动；请求范围 8..84，单位 0.25 dBm |
| `eh_wifi_get_max_tx_power()` | 276 | Wi-Fi 已启动；读回 CP 实际配置，单位 0.25 dBm |

这些同步接口均接收非零 `timeout_ms`。国家码的最终有效性、当地可用信道及组合由 CP 判定；国家配置可能写入 CP Flash，不应在周期探针中反复设置。当前 CP 设置处理只复制两字符代码，因此设置接口不接受第三个环境字符；查询保留 CP 返回的合法环境字符。`eh_wifi_country_info_t.max_tx_power` 的单位是整 dBm，与功率接口的 0.25 dBm 区分。

`ieee80211d_enabled=0` 为固定 MANUAL 策略，1 为 AUTO。AUTO 关联路由器后可能采用其广播的有效国家信息；保存原配置前应先断开 STA，查询未关联时的国家策略，恢复时分别核对未关联配置和关联后的有效信息。

信道和最大发射功率属于共享射频，没有 STA/AP 参数。设置信道拒绝已知扫描、连接进行中和 STA 已关联状态；应用还应保证切换时无 AP 客户端，不暗中断开手机。AP+STA 关联路由器后 AP 跟随 STA 实际信道，不承诺两者独立信道，也不自动改带宽或重新连接。

功率设置可能按 CP 档位量化，读回无需与请求逐值相等。组件不自动提升功率；应用必须结合国家上限选值。RPC 276 的状态字段为字段 2，已单独处理。设置成功仅表示 CP 接受配置；查询失败保持输出不变，缺字段、重复字段、错误类型/数值或畸形响应按现有错误体系返回。

```c
/* Wi-Fi 已初始化，STA 未关联；国家策略无需周期重复设置。 */
stm_err_t err = eh_wifi_set_country_code(host, "CN", 0U, 5000U);
eh_wifi_country_info_t country;
if (err == STM_OK) { err = eh_wifi_get_country(host, &country, 5000U); }
/* Wi-Fi 启动后、无扫描/连接任务且无 AP 客户端时设置纯 AP 信道。 */
if (err == STM_OK) { err = eh_wifi_set_channel(host, 6U, EH_WIFI_SECOND_CHAN_NONE, 5000U); }
int8_t actual_power;
if (err == STM_OK && country.max_tx_power >= 11) {
    err = eh_wifi_set_max_tx_power(host, 44, 5000U); /* 11 dBm 上限请求。 */
    if (err == STM_OK) { err = eh_wifi_get_max_tx_power(host, &actual_power, 5000U); }
}
```

## 运行信息与 AP 客户端管理

`eh_wifi_sta_get_rssi()` 在 STA 已关联时查询 dBm 信号强度；`eh_wifi_get_channel()` 在 Wi-Fi 启动后查询实际主信道及 `EH_WIFI_SECOND_CHAN_NONE/ABOVE/BELOW`。AP+STA 共用射频，AP 信道可能随 STA 关联变化。

AP 已启动后，`eh_wifi_ap_get_sta_list()` 返回一次客户端快照，每条记录包含六字节 MAC 和 RSSI。`records=NULL, capacity=0` 可只查询所需数量；容量不足返回 `STM_ERR_OUT_OF_RANGE` 并更新所需数量，数组保持不变。空列表返回成功及零数量，其他失败保持数组和数量不变。客户端 IP 仍由 DHCP/lwIP 管理，列表不返回 IP 或 AID。

```c
eh_wifi_sta_record_t clients[4];
size_t count = 0U;
stm_err_t err = eh_wifi_ap_get_sta_list(host, clients, 4U, &count, 5000U);
if (err == STM_OK) {
    /* 根据应用明确选择的 MAC 查找客户端，再查询 AID。不要自动断开任意客户端。 */
    for (size_t i = 0U; i < count; ++i) {
        if (memcmp(clients[i].mac, selected_mac, 6U) == 0) {
            uint16_t aid;
            if (eh_wifi_ap_get_sta_aid(host, selected_mac, &aid, 5000U) == STM_OK) {
                (void)eh_wifi_deauth_sta(host, aid, 5000U);
            }
            break;
        }
    }
}
```

示例需包含 `<string.h>`，`selected_mac` 由应用选择。`eh_wifi_ap_get_sta_aid()` 单独查询关联 ID；`eh_wifi_deauth_sta()` 仅接受 `1..2007`，拒绝零以避免断开全部客户端。成功表示 CP 接受请求，实际离线须通过客户端事件及新的列表确认；客户端也可能马上重新接入，事件发生顺序需分别处理。除列表容量不足时更新数量外，上述查询失败均保持输出不变。同步 RPC 都带 `timeout_ms`，应在主循环执行，事件回调只标记待处理任务。

STA/AP lwIP attach 会同步组件已经缓存的链路状态，随后由事件继续更新。因此，关联或 `AP_START` 早于网卡注册时也能正确启动 DHCP。应用仍需每轮调用对应的生命周期更新及 `sys_check_timeouts()`。

## 板级参考与验证

STM32H723 板级示例位于配套 `stm_h723_demo` 工程的 `main/app_main.c`。ESP32-C3 的参考固件工程位于本仓库 `firmware/esp32c3_cp/`。该板连接 SPI1 MOSI=PD7、MISO=PA6、SCLK=PG11、CS=PC4、Handshake=PA2、Data Ready=PA3、Reset/EN=PC5；ESP32-C3 对应 GPIO7/2/6/10/3/4。其他板请核对原理图并更换配置。

```sh
cmake -S tests -B build/stm_esp_hosted_tests -G Ninja
cmake --build build/stm_esp_hosted_tests
ctest --test-dir build/stm_esp_hosted_tests --output-on-failure
```

测试覆盖帧校验和边界、RPC 异常与超时、扫描错误重试、STA/AP 事件、模式/配置/省电模式读回的成功与异常响应、密码不外露、DHCP 租约分配/续租/冲突/释放，以及轻量 lwIP 仿真中的 `pbuf` 链收发。本轮新增自动重连的首次连接、关联超时、退避和重试上限、意外断线，以及主动断开、停止和禁用后的取消测试。STM32 实板已验证 STA 获取 DHCP 地址、DNS、TCP/UDP 回显与主动断线重连。此前 CP Wi-Fi init RPC（278）曾因 NVS 未初始化返回 `0x1101`；CP 工程改为先初始化 NVS 和事件循环，再启动 ESP-Hosted。2026-09-28 的阶段性测试中，20 次 J-Link 复位均在 90 秒内完成全链路验证。2026-09-29 的纯 STA 持续运行从首次全链路通过起保持 7200 秒，121 轮 TCP/UDP 周期回显全部通过、0 失败；期间一次主动断线后约 9.9 秒恢复，并重新获得 DHCP 地址及完成 DNS 查询。整板断电重启的独立结果见下文。

AP+STA 实板功能测试中，首次扫描返回 3 条记录，最终固件复测返回 4 条记录，电脑关联测试 AP 后获得 `192.168.40.100/24`，网关 `192.168.40.1`，三次 UDP 数据均从 `192.168.40.1:24681` 原样回显。切换测试电脑的无线网络后，STA 测试服务器不可达，因此该切换后的回显失败不计入持续运行稳定性结论。2026-09-29，手机连接测试 AP 后，在浏览器访问 `http://192.168.40.1/` 成功显示 `STM32 AP OK`；RTT 记录客户端接入和来自 `192.168.40.100` 的两次 HTTP GET。该页面验证单客户端 AP TCP/HTTP 通路。手机向 AP 的 UDP 回显端口发送数据时，每次发送均收到一条对应回包；板端 RTT 连续记录 `AP UDP echo 47 bytes`，47 字节是手机应用实际报文长度，不代表仅发送了五字节文本。此项只验证 AP UDP 双向通信，不计为长期稳定性测试。多个客户端、AP 长期运行、BLE、OTA 和 RTOS 仍未验证或实现。原始日志与本地凭据不随组件发布。

2026-09-29 至 09-30 的 v0.3.0 实板回归：STA 配置与模式查询成功，`NONE`、`MIN_MODEM`、`MAX_MODEM` 各设置及读回成功；每种模式以约 60 秒间隔进行 10 轮 DHCP 地址保持、DNS、TCP/UDP 回显，合计 30/30 轮通过。主动断线后重新取址并恢复 DNS、TCP/UDP。AP+STA 模式及 AP 配置读回成功，板端 DHCP 启动；手机获取租约、访问 HTTP 页面并收到 UDP 回显，板端记录对应客户端、HTTP 和 UDP 事件。此项功能回归与后续整板断电重启分别统计。

2026-09-30 的自动重连实板诊断：使用仅供本地测试的固件让 CP 发出真实 STA 断线事件，保留组件的自动重连策略；在断线后约 11.8 秒重新完成关联、DHCP、DNS 与 TCP/UDP 回显，期间未记录回显失败。诊断入口随后已移除，正式固件重新烧录并在一次复位后通过 CP INIT、STA、DHCP、DNS 与两轮间隔约 60 秒的 TCP/UDP 回显，零失败。此项验证 CP 断线事件的恢复路径，不代表路由器断电或射频持续干扰场景，也不替代此前的整板断电验收。原始日志仅在本地保留。

2026-09-30 的整板断电重启验收共观察 13 次：10 次有效通过，均重新完成 CP 3.0.9 INIT、STA 关联、DHCP、DNS 及 TCP/UDP 回显；其中还观察到一次欠压启动失败、一次采集缺口无法判定、一次无法证明整板重启且网络验证失败。10 次通过并非连续 10 次无故障通过。最后一次有效循环从 ESP 串口断连到首轮全链路通过约 33.5 秒；主动断线后的 DHCP 重新取址、DNS 和 TCP/UDP 回显恢复亦通过。部分循环在 USB 重新枚举期间未采集到 CP ROM 启动文本，因此不据此推断上电瞬态均正常。原始日志和逐次记录仅留在本地构建目录，不随组件发布。

## v0.5.0 验证范围

STM32H723 与 ESP32-C3 CP 3.0.9 完成运行 RSSI、实际信道和关联 AP 信息查询；STA 十轮约 60 秒间隔的 DHCP 地址保持、真实 DNS 与 TCP/UDP 回显通过，零失败。单台手机完成三轮指定 AID 主动断开与重新接入，每轮确认离线事件、列表移除、事件/RPC AID 一致、有效 DHCP 租约、HTTP 访问和 UDP 原样回复；三轮通信恢复均在 90 秒内，AP 测试期间 STA 通信保持正常。多客户端列表及指定客户端操作由模拟测试覆盖，未进行第二台客户端实板隔离验证。

模拟测试新增负 RSSI、信道枚举、列表数量/容量/空列表、多客户端、非法 MAC/AID、CP 错误、重复/畸形/超长响应、超时和迟到响应，以及 attach 前事件和过期网卡链路状态回归。组件测试、STM32 构建、聚合仓库相关编译与 CP 固件构建通过。本轮为功能验收，不增加断电或长期稳定性结论；原始日志、逐次实验记录和凭据仅保留本地。

## v0.6.0 验证范围与复现

STM32H723 与 ESP32-C3 CP 3.0.9 已完成 B/HT20、BG/HT20、BGN/HT20、BGN/HT40 四组 STA 配置读回、关联与 DHCP 验证；各进行十轮约 60 秒间隔的真实 DNS、TCP/UDP 回显，合计 40/40 轮，零失败或缺失。每组显式断开后均在 90 秒内重新关联、取址并恢复 DNS 和双协议通信。

AP+STA 模式中，STA 固定 BGN/HT20，单台手机分别完成四组 AP 配置的接入、有效 DHCP 租约、HTTP 页面及 UDP 原样回复，同时验证 STA 通信。AP 验收中的操作超时和手机缓存旧租约的尝试未计为通过，重新测试后四组均在关联后的 90 秒内完成；结束后原始 STA/AP 配置读回和全链路恢复通过。此结论仅为配置功能验收，不新增实际 40 MHz、吞吐、长期运行或断电稳定性结论。

配套 H723 演示的 `CONFIG_APP_ESP_HOSTED_RADIO_TESTS` 默认关闭；开启时要求 ESP-Hosted 和 STA 测试开启，与 INFO、PS、CLIENT、SCAN 专项测试互斥，允许 AP 示例。准备本地忽略的凭据与可达的 TCP/UDP 原样回显服务后启用此开关，主循环自动执行 STA 四组；手机 AP 组在每次准备完成后逐组启动。手机需保持连接并在每组重新访问 `http://192.168.40.1/`、向 `192.168.40.1:24681` 发新 UDP 数据。短暂切换后可能缓存旧 IP；必要时忘记测试网络再连接，必须确认新有效租约，不能只凭页面和回包判定 DHCP 通过。

模拟测试覆盖 STA/AP 独立配置、三种协议与两种带宽、CP 拒绝、无效参数、缺字段、非法枚举、重复/畸形/超长响应、超时和跨事务迟到响应，以及失败时输出不变。组件测试 3/3、H723 固件、H723/H757 C/C++ 公开接口检查、聚合相关检查及现有 ESP-IDF 固件构建通过。原始日志、逐次记录和凭据仅留本地，不随发布分发。

## v0.7.0 验证范围与复现

2026-10-01，STM32H723 + ESP32-C3 CP 3.0.9 完成十个故障注入场景：五次 CP 独立 EN 复位、三次 EN 保持低 40 秒并观察心跳超时，以及查询进行中和扫描进行中各一次复位。STM32 在注入期间保持运行，十次均在 90 秒内恢复会话、配置、DHCP、真实 DNS 和 TCP/UDP，最慢约 22.5 秒；随后十轮约 60 秒间隔正常通信通过。首次扫描注入的脉冲计时问题修复后，完整十个场景从头回归通过，故障期间中断未计为正常周期通过。

CN MANUAL 与 AUTO/802.11d 各十轮 DHCP 保持、真实 DNS、TCP/UDP，20/20 通过。功率请求 20、44、60（5、11、15 dBm）分别设置并读回，各十轮同样检查，30/30 通过；每档显式断线后均在 90 秒内重新取址和恢复通信。原未关联国家策略、协议、带宽、省电及功率保存、重放和恢复核对通过。

单台手机在纯 AP BGN/HT20 下完成信道 1、6、11 的读回、新有效 DHCP 租约、HTTP 页面和 UDP 原样回复，均在关联后 90 秒内完成。AP+STA 共享信道及同时通信通过；一次 AP+STA 下 CP 独立复位后约 22 秒恢复 STA 全链路，手机重新取得租约并恢复 HTTP/UDP。三个功率档位下手机与 STA 同时通信、结束后的原配置读回与两端复测均通过。准备延迟或手机缓存旧 IP 的超时尝试未计为通过，重新准备并取得新租约后重测通过。

复现时使用配套演示默认关闭的 `CONFIG_APP_ESP_HOSTED_RECOVERY_TESTS`、`CONFIG_APP_ESP_HOSTED_REGION_TESTS`、`CONFIG_APP_ESP_HOSTED_POWER_TESTS`，每次仅开一项；要求 ESP-Hosted、STA 开启，与其他专项测试互斥，手机阶段还需 AP 示例。准备忽略的本地凭据、真实 DNS 及电脑 TCP/UDP 原样回显服务，先运行自动阶段，再在手机准备完成后逐组启动。手机保持连接，切换后必要时忘记热点以获取新租约，重新访问 HTTP 并发送新 UDP；仅凭旧 IP 下通信成功不足以通过 DHCP 验收。故障注入入口只在本地恢复测试构建启用，正式演示关闭专项测试。

自动检查覆盖重复 INIT、无数据、心跳启停/超时、计时回绕、事务取消/互斥、迟到响应、回调重入、恢复成功/失败、诊断饱和计数、STA 地址/ARP 清理、AP 租约清理和重复启停，以及国家/信道/功率参数边界、默认值省略、错误状态字段、畸形/超长响应和失败时输出不变。组件测试 3/3、H723 正常/专项固件、H723/H757 C/C++ 接口及聚合编译、现有 ESP-IDF CP 固件构建通过。

本轮仅覆盖所列异常恢复与功能配置；功率读回和通信不代表实际射频输出测量，HT40 仍只是配置值。不新增长期运行、吞吐或整板断电稳定性结论。原始日志、故障注入和逐次记录及凭据只留本地。

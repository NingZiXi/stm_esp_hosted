#include "stm_esp_hosted.h"
#include "stm_esp_hosted_private.h"
#include "hal_stub.h"

#include <stdio.h>
#include <string.h>

void test_hal_set_spi_status(HAL_StatusTypeDef status);

#define TEST_ASSERT(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1; \
        } \
    } while (0)

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)(value >> 8U);
}

static esp_hosted_handle_t make_handle(uint8_t *tx, uint8_t *rx, uint8_t dummy_if_type)
{
    static SPI_HandleTypeDef spi;
    static GPIO_TypeDef cs;
    static GPIO_TypeDef reset;
    static GPIO_TypeDef handshake;
    static GPIO_TypeDef data_ready;
    esp_hosted_config_t config = {
        .spi = &spi,
        .cs_port = &cs,
        .cs_pin = 1U,
        .reset_port = &reset,
        .reset_pin = 2U,
        .handshake_port = &handshake,
        .handshake_pin = 3U,
        .data_ready_port = &data_ready,
        .data_ready_pin = 4U,
        .tx_buffer = tx,
        .rx_buffer = rx,
        .buffer_size = ESP_HOSTED_FRAME_SIZE,
        .transfer_timeout_ms = 10U,
        .dummy_if_type = dummy_if_type,
        .checksum_enabled = 1U,
    };
    esp_hosted_handle_t handle = NULL;
    if (esp_hosted_create(&config, &handle) != STM_OK) {
        return NULL;
    }
    return handle;
}

static int test_dummy_transfer(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    esp_hosted_handle_t handle = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    esp_hosted_frame_t frame;
    esp_hosted_info_t info;

    TEST_ASSERT(handle != NULL);
    TEST_ASSERT(esp_hosted_transfer(handle, NULL, rx) == STM_OK);
    TEST_ASSERT(tx[0] == (uint8_t)(0xF0U | ESP_HOSTED_DUMMY_IF_TYPE));
    TEST_ASSERT(tx[2] == 0U && tx[3] == 0U);
    TEST_ASSERT(tx[4] == 0U && tx[5] == 0U);
    TEST_ASSERT(tx[6] == 0U && tx[7] == 0U);
    TEST_ASSERT(esp_hosted_is_dummy_frame(handle, rx, sizeof(rx)) != 0U);
    TEST_ASSERT(esp_hosted_decode_frame(handle, rx, sizeof(rx), &frame) == ESP_HOSTED_FRAME_DUMMY);
    TEST_ASSERT(esp_hosted_get_info(handle, &info) == STM_OK);
    TEST_ASSERT(info.transfer_count == 1U && info.ready != 0U);
    TEST_ASSERT(esp_hosted_delete(&handle) == STM_OK && handle == NULL);
    return 0;
}

static int test_normal_frame(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    uint8_t frame[ESP_HOSTED_FRAME_SIZE] = {0};
    esp_hosted_handle_t handle = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    esp_hosted_frame_t decoded;
    const uint8_t payload[] = {0x11U, 0x22U, 0x33U, 0x44U};

    frame[0] = 0x01U;
    frame[1] = 0xA0U;
    write_le16(&frame[2], (uint16_t)sizeof(payload));
    write_le16(&frame[4], 12U);
    write_le16(&frame[8], 0x1234U);
    frame[10] = 0x02U;
    frame[11] = 0x07U;
    memcpy(&frame[12], payload, sizeof(payload));
    write_le16(&frame[6], esp_hosted_frame_checksum(frame, 16U));

    TEST_ASSERT(esp_hosted_decode_frame(handle, frame, sizeof(frame), &decoded) == ESP_HOSTED_FRAME_OK);
    TEST_ASSERT(decoded.if_type == 1U && decoded.if_num == 0U);
    TEST_ASSERT(decoded.flags == 0xA0U && decoded.payload_length == sizeof(payload));
    TEST_ASSERT(decoded.payload_offset == 12U && decoded.sequence == 0x1234U);
    TEST_ASSERT(decoded.throttle_command == 2U && decoded.packet_type == 7U);
    TEST_ASSERT(memcmp(decoded.payload, payload, sizeof(payload)) == 0);
    TEST_ASSERT(esp_hosted_delete(&handle) == STM_OK);
    return 0;
}

static int test_init_caps(void)
{
    uint8_t caps[20];
    size_t length = 0U;
    const uint8_t init_v2[] = {0x22U, 9U, 0x11U, 1U, 0x01U,
                               0x12U, 1U, 0x05U, 0x1AU, 1U, 0x02U};
    const uint8_t init_v1[] = {0x22U, 3U, 0x12U, 1U, 0x05U};
    const uint8_t malformed[] = {0x22U, 3U, 0x12U, 2U, 0x05U};
    TEST_ASSERT(esp_hosted_build_host_caps(init_v2, sizeof(init_v2),
                                           caps, sizeof(caps), &length) == STM_OK);
    TEST_ASSERT(length == 20U && caps[1] == 18U && caps[7] == 5U);
    TEST_ASSERT(caps[17] == 0x1AU && caps[18] == 1U && caps[19] == 2U);
    TEST_ASSERT(esp_hosted_build_host_caps(init_v1, sizeof(init_v1),
                                           caps, sizeof(caps), &length) == STM_OK);
    TEST_ASSERT(length == 17U && caps[1] == 15U);
    TEST_ASSERT(esp_hosted_build_host_caps(malformed, sizeof(malformed),
                                           caps, sizeof(caps), &length) == STM_ERR_VERIFY);
    return 0;
}

static int test_encode_frame(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    uint8_t frame[ESP_HOSTED_FRAME_SIZE];
    esp_hosted_frame_t decoded;
    const uint8_t caps[] = {0x22U, 3U, 0x44U, 1U, 0U};
    esp_hosted_handle_t handle = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);

    TEST_ASSERT(handle != NULL);
    TEST_ASSERT(esp_hosted_encode_frame(handle, 5U, 0x33U, 42U,
                                        caps, sizeof(caps), frame, sizeof(frame)) == STM_OK);
    TEST_ASSERT(esp_hosted_decode_frame(handle, frame, sizeof(frame), &decoded) == ESP_HOSTED_FRAME_OK);
    TEST_ASSERT(decoded.if_type == 5U && decoded.packet_type == 0x33U);
    TEST_ASSERT(decoded.sequence == 42U && decoded.payload_length == sizeof(caps));
    TEST_ASSERT(memcmp(decoded.payload, caps, sizeof(caps)) == 0);
    TEST_ASSERT(esp_hosted_encode_frame(handle, 5U, 0x33U, 0U,
                                        frame + 12U, sizeof(caps), frame, sizeof(frame)) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(esp_hosted_delete(&handle) == STM_OK);
    return 0;
}

static int test_invalid_frames(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    uint8_t frame[ESP_HOSTED_FRAME_SIZE] = {0};
    esp_hosted_handle_t handle = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    esp_hosted_frame_t decoded;

    frame[0] = 0x01U;
    write_le16(&frame[2], 4U);
    write_le16(&frame[4], 12U);
    TEST_ASSERT(esp_hosted_decode_frame(handle, frame, sizeof(frame), &decoded) == ESP_HOSTED_FRAME_CORRUPT);

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x01U;
    write_le16(&frame[2], 1U);
    write_le16(&frame[4], 8U);
    TEST_ASSERT(esp_hosted_decode_frame(handle, frame, sizeof(frame), &decoded) == ESP_HOSTED_FRAME_INVALID);

    memset(frame, 0, sizeof(frame));
    frame[0] = 0x01U;
    write_le16(&frame[2], 0xFFFFU);
    write_le16(&frame[4], 12U);
    TEST_ASSERT(esp_hosted_decode_frame(handle, frame, sizeof(frame), &decoded) == ESP_HOSTED_FRAME_TOO_BIG);

    TEST_ASSERT(esp_hosted_decode_frame(handle, frame, 4U, &decoded) == ESP_HOSTED_FRAME_INVALID);
    TEST_ASSERT(esp_hosted_delete(&handle) == STM_OK);
    return 0;
}

static int test_transfer_errors(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    esp_hosted_handle_t handle = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    esp_hosted_info_t info;

    TEST_ASSERT(handle != NULL);
    test_hal_set_spi_status(HAL_TIMEOUT);
    TEST_ASSERT(esp_hosted_transfer(handle, NULL, rx) == STM_ERR_TIMEOUT);
    TEST_ASSERT(esp_hosted_get_info(handle, &info) == STM_OK);
    TEST_ASSERT(info.transfer_count == 0U && info.last_hal_status == HAL_TIMEOUT);
    test_hal_set_spi_status(HAL_ERROR);
    TEST_ASSERT(esp_hosted_transfer(handle, NULL, rx) == STM_ERR_IO);
    TEST_ASSERT(esp_hosted_delete(&handle) == STM_OK);
    test_hal_set_spi_status(HAL_OK);
    return 0;
}

static int test_legacy_dummy(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    esp_hosted_handle_t handle = make_handle(tx, rx, ESP_HOSTED_LEGACY_DUMMY_IF_TYPE);
    tx[0] = (uint8_t)(0xF0U | ESP_HOSTED_LEGACY_DUMMY_IF_TYPE);
    tx[2] = 0U;
    tx[3] = 0U;
    tx[4] = 0U;
    tx[5] = 0U;
    TEST_ASSERT(handle != NULL);
    TEST_ASSERT(esp_hosted_is_dummy_frame(handle, tx, sizeof(tx)) != 0U);
    TEST_ASSERT(esp_hosted_delete(&handle) == STM_OK);
    return 0;
}

static unsigned rx_count, link_count;
static size_t received_length;
static uint8_t last_link;
static void receive_cb(void *user, const uint8_t *frame, size_t size)
{
    (void)user;
    if (frame && size >= 14U) { ++rx_count; received_length = size; }
}
static void link_cb(void *user, uint8_t connected)
{
    (void)user; ++link_count; last_link = connected;
}
static int inject_payload(esp_hosted_handle_t h, uint8_t type,
                          const uint8_t *payload, size_t length)
{
    uint8_t frame[ESP_HOSTED_FRAME_SIZE];
    if (esp_hosted_encode_frame(h, type, 0U, 1U, payload, length,
                                frame, sizeof(frame)) != STM_OK) { return 0; }
    test_hal_inject_rx(frame, sizeof(frame));
    return 1;
}
static int test_control_and_sta(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t ethernet[ESP_HOSTED_STA_MTU + 14U];
    esp_hosted_handle_t h = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    const uint8_t connected[] = {1U, 0U, 0U, 2U, 10U, 0U,
                                  8U, 3U, 16U, 0x87U, 6U, 0xBAU, 0x30U, 2U, 0x12U, 0U}; /* event 775 */
    const uint8_t disconnected[] = {1U, 0U, 0U, 2U, 12U, 0U,
                                     8U, 3U, 16U, 0x88U, 6U, 0xC2U, 0x30U, 4U, 0x12U, 2U, 0x20U, 7U}; /* event 776 */
    const uint8_t broken[] = {1U, 0U, 0U, 2U, 4U, 0U, 8U, 3U, 16U};
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(esp_hosted_set_callbacks(h, receive_cb, link_cb, NULL) == STM_OK);
    TEST_ASSERT(inject_payload(h, 3U, connected, sizeof(connected)));
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK && eh_wifi_is_connected(h));
    TEST_ASSERT(link_count == 1U && last_link == 1U);
    const uint8_t malformed_disconnect[] = {1U, 0U, 0U, 2U, 8U, 0U,
        8U, 3U, 16U, 0x88U, 6U, 0xC2U, 0x30U, 0U};
    TEST_ASSERT(inject_payload(h, 3U, malformed_disconnect, sizeof(malformed_disconnect)));
    TEST_ASSERT(esp_hosted_poll(h) == STM_ERR_VERIFY && eh_wifi_is_connected(h));
    TEST_ASSERT(link_count == 1U);
    TEST_ASSERT(inject_payload(h, 1U, ethernet, sizeof(ethernet)));
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK);
    TEST_ASSERT(rx_count == 1U && received_length == sizeof(ethernet));
    TEST_ASSERT(esp_hosted_send(h, ethernet, 13U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(esp_hosted_send(h, ethernet, sizeof(ethernet) + 1U) == STM_ERR_INVALID_ARG);
    h->initialized = 1U;
    TEST_ASSERT(esp_hosted_send(h, ethernet, sizeof(ethernet)) == STM_OK);
    TEST_ASSERT(inject_payload(h, 3U, broken, sizeof(broken)));
    TEST_ASSERT(esp_hosted_poll(h) == STM_ERR_VERIFY);
    TEST_ASSERT(inject_payload(h, 3U, disconnected, sizeof(disconnected)));
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK && !eh_wifi_is_connected(h));
    TEST_ASSERT(link_count == 2U && last_link == 0U);
    TEST_ASSERT(esp_hosted_send(h, ethernet, 14U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(esp_hosted_delete(&h) == STM_OK);
    return 0;
}
static int test_init_timeout(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    esp_hosted_handle_t h = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h != NULL);
    TEST_ASSERT(esp_hosted_start(h, 3U) == STM_ERR_TIMEOUT);
    TEST_ASSERT(esp_hosted_delete(&h) == STM_OK);
    return 0;
}


/* The mock CP replies in the same SPI transaction and emits a later STA event. */
static esp_hosted_handle_t mock_host;
static unsigned mock_stage, mock_requests, mock_connect_event, mock_disconnect_event, mock_error_mode;
static unsigned query_fault, mock_mode = EH_WIFI_MODE_STA, mock_ps, mock_iface;
/* Runtime queries: CP error, missing value, invalid scalar, duplicate, wrong wire,
 * corrupt tail, timeout, stale UID, oversized body, valid unknown fields,
 * invalid second client, inconsistent count, sign-extended int32, default RSSI. */
static unsigned runtime_fault, runtime_clients = 2U, runtime_second, runtime_primary = 6U;
static unsigned runtime_request_ok;
/* 1=CP error, 2=missing cfg, 3=oversize SSID, 4=wrong iface,
 * 5=wrong branch, 6=malformed payload, 7=timeout, 8=stale uid,
 * 9=bad enum. */
static uint32_t mock_seen;
static uint64_t test_read_varint(const uint8_t **data)
{
    uint64_t result = 0U;
    for (unsigned i = 0U; i < 10U; ++i) {
        uint8_t byte = *(*data)++;
        result |= (uint64_t)(byte & 0x7FU) << (7U * i);
        if (!(byte & 0x80U)) { break; }
    }
    return result;
}
static size_t test_put_varint(uint8_t *data, uint64_t value)
{
    size_t n = 0U;
    do {
        data[n++] = (uint8_t)((value & 0x7FU) | (value > 127U ? 0x80U : 0U));
        value >>= 7U;
    } while (value);
    return n;
}
static size_t test_put_num(uint8_t *data, uint32_t tag, uint64_t value)
{
    size_t n = test_put_varint(data, (uint64_t)tag << 3U);
    return n + test_put_varint(data + n, value);
}
static size_t test_put_bytes(uint8_t *data, uint32_t tag, const uint8_t *value, size_t length)
{
    size_t n = test_put_varint(data, ((uint64_t)tag << 3U) | 2U);
    n += test_put_varint(data + n, length);
    memcpy(data + n, value, length);
    return n + length;
}
static unsigned radio_fault;
static unsigned advanced_fault;
static unsigned reset_during_request, mock_bad_version, mock_monitor_reject;
static uint8_t mock_power = 80U;
static uint64_t radio_invalid_value;
static uint8_t radio_delayed[800], radio_current[800];
static size_t radio_delayed_len, radio_current_len;
static uint8_t radio_protocol[2] = {7U,7U}, radio_bw[2] = {1U,1U};
static unsigned scan_event_pending, scan_config_seen, scan_rejected, scan_result_status, ap_info_bad;
static eh_wifi_event_t last_wifi_event;
static unsigned wifi_event_count;
static void wifi_event_cb(void *user, const eh_wifi_event_t *event)
{
    (void)user; last_wifi_event = *event; ++wifi_event_count;
}
static void mock_cp(const uint8_t *tx, uint8_t *rx, uint16_t length)
{
    static const uint8_t init[] = {0x22U, 9U, 0x11U, 1U, 0x01U,
                                   0x12U, 1U, 0x05U, 0x1AU, 1U, 0x02U};
    static const uint8_t event[] = {1U, 0U, 0U, 2U, 10U, 0U,
                                    8U, 3U, 16U, 0x87U, 6U, 0xBAU, 0x30U, 2U, 0x12U, 0U};
    static const uint8_t mac[] = {0x02U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U};
    static const uint8_t ep[] = "RPCRsp";
    if ((tx[0] & 0x0FU) == ESP_HOSTED_DUMMY_IF_TYPE && radio_current_len) {
        (void)esp_hosted_encode_frame(mock_host, 3U, 0U, 2U, radio_current,
                                     radio_current_len, rx, length);
        radio_current_len = 0U;
        return;
    }
    if (!mock_stage) {
        (void)esp_hosted_encode_frame(mock_host, 5U, 0U, 1U, init,
                                       sizeof(init), rx, length);
        mock_stage = 1U;
        return;
    }
    if ((tx[0] & 0x0FU) == ESP_HOSTED_DUMMY_IF_TYPE && scan_event_pending) {
        uint8_t done[] = {1,0,0,2,14,0,8,3,16,0x86,6,
                          0xB2,0x30,6,0x12,4,8,0,16,1};
        done[17] = (uint8_t)scan_result_status;
        (void)esp_hosted_encode_frame(mock_host, 3U, 0U, 2U, done, sizeof(done), rx, length);
        scan_event_pending = 0U;
        return;
    }
    if ((tx[0] & 0x0FU) == ESP_HOSTED_DUMMY_IF_TYPE && mock_disconnect_event) {
        const uint8_t disconnected[] = {1U, 0U, 0U, 2U, 12U, 0U,
            8U, 3U, 16U, 0x88U, 6U, 0xC2U, 0x30U, 4U, 0x12U, 2U, 0x20U, 7U};
        (void)esp_hosted_encode_frame(mock_host, 3U, 0U, 2U, disconnected,
                                       sizeof(disconnected), rx, length);
        mock_disconnect_event = 0U;
        return;
    }
    if ((tx[0] & 0x0FU) == ESP_HOSTED_DUMMY_IF_TYPE && mock_connect_event) {
        (void)esp_hosted_encode_frame(mock_host, 3U, 0U, 2U, event,
                                       sizeof(event), rx, length);
        mock_connect_event = 0U;
        return;
    }
    if ((tx[0] & 0x0FU) != 3U) { return; }
    uint16_t offset = (uint16_t)(12U + 3U + tx[13U]);
    const uint8_t *rpc = tx + offset + 3U;
    if (rpc[0] != 8U || rpc[2] != 16U) { return; }
    rpc += 3U;
    uint32_t id = (uint32_t)test_read_varint(&rpc);
    if (*rpc++ != 24U) { return; }
    uint32_t uid = (uint32_t)test_read_varint(&rpc);
    uint8_t body[640] = {0}, payload[740] = {0}, wire[800] = {0};
    size_t b = 0U, n = 0U, w = 0U;
    ++mock_requests;
    if(reset_during_request) {
        reset_during_request=0U;
        (void)esp_hosted_encode_frame(mock_host,5U,0U,1U,init,sizeof(init),rx,length);
        return;
    }
    if ((id == 304U || id == 335U || id == 275U || id == 276U || id == 334U || id == 301U) && advanced_fault == 7U) { return; }
    if (runtime_fault == 7U && (id == 341U || id == 302U || id == 311U ||
        id == 312U || id == 293U)) { return; }
    if (query_fault == 7U && (id == 259U || id == 271U || id == 285U)) { return; }
    if (id == 304U || id == 335U || id == 275U || id == 276U || id == 334U || id == 301U) {
        uint32_t result_field = id == 276U ? 1U : 2U;
        uint32_t status_field = id == 276U ? 2U : 1U;
        if (advanced_fault == 1U) { b += test_put_num(body+b,status_field,0x1101U); }
        else if (advanced_fault != 2U) {
            if (id == 335U) {
                const uint8_t cc[] = {'C','N',0};
                b += test_put_bytes(body+b,2U,cc,advanced_fault == 3U ? 1U : sizeof(cc));
            } else if (id == 304U) {
                uint8_t country[100]; size_t c=0U;
                c+=test_put_bytes(country+c,1U,(const uint8_t *)"CN ",3U);
                c+=test_put_num(country+c,2U,1U);
                c+=test_put_num(country+c,3U,advanced_fault == 3U ? 15U : 13U);
                c+=test_put_num(country+c,4U,20U);
                /* AUTO is deliberately omitted. */
                b+=test_put_bytes(body+b,2U,country,c);
            } else if (id == 276U) {
                b+=test_put_num(body+b,1U,advanced_fault == 3U ? UINT64_MAX : mock_power);
            } else if (id == 275U) {
                const uint8_t *cursor = rpc;
                (void)test_read_varint(&cursor); (void)test_read_varint(&cursor);
                (void)test_read_varint(&cursor); mock_power=(uint8_t)test_read_varint(&cursor);
            }
        }
        if (advanced_fault == 4U) {
            if (id == 276U) { b+=test_put_num(body+b,result_field,20U); }
            else { b+=test_put_bytes(body+b,result_field,(const uint8_t *)"CN",2U); }
        }
        if (advanced_fault == 5U) {
            if (id == 276U) { b=0U; b+=test_put_bytes(body+b,result_field,(const uint8_t *)"x",1U); }
            else { b=0U; b+=test_put_num(body+b,result_field,1U); }
        }
        if (advanced_fault == 6U) { body[b++]=0x80U; }
        if (advanced_fault == 9U) { uint8_t pad[520]={0};b+=test_put_bytes(body+b,20U,pad,sizeof(pad)); }
        if (advanced_fault == 10U) { b+=test_put_num(body+b,20U,123U); }
    } else if (id >= 297U && id <= 300U) {
        if (radio_fault == 7U) { return; }
        const uint8_t *cursor = rpc;
        uint64_t key = test_read_varint(&cursor), size = test_read_varint(&cursor);
        const uint8_t *end = cursor + size;
        unsigned iface = 9U, value = 0U;
        if (cursor < end && *cursor++ == 8U) { iface = (unsigned)test_read_varint(&cursor); }
        if (cursor < end && *cursor++ == 16U) { value = (unsigned)test_read_varint(&cursor); }
        if (key != (((uint64_t)id << 3U) | 2U) || iface > 1U || cursor != end) {
            b += test_put_num(body+b,1U,0x102U);
        } else if (radio_fault == 1U || (id == 299U && value == 2U && !(radio_protocol[iface] & 4U))) {
            b += test_put_num(body+b,1U,0x102U);
        } else if (id == 297U || id == 299U) {
            if (id == 297U) { radio_protocol[iface]=(uint8_t)value; }
            else { radio_bw[iface]=(uint8_t)value; }
        } else if (radio_fault != 2U) {
            value = id == 298U ? radio_protocol[iface] : radio_bw[iface];
            if (radio_fault == 3U) { value=0U; }
            if (radio_fault == 5U) { b += test_put_bytes(body+b,2U,(const uint8_t *)"x",1U); }
            else if (radio_fault == 13U) { b += test_put_num(body+b,2U,radio_invalid_value); }
            else { b += test_put_num(body+b,2U,value); }
            if (radio_fault == 4U) { b += test_put_num(body+b,2U,value); }
        }
        if (radio_fault == 6U) { body[b++]=0x80U; }
        if (radio_fault == 9U) { uint8_t pad[64]={0}; b+=test_put_bytes(body+b,20U,pad,sizeof(pad)); }
        if (radio_fault == 10U) { body[b++]=0xA5U;body[b++]=1U;memset(body+b,0,4U);b+=4U; }
        if (radio_fault == 11U) { b+=test_put_num(body+b,1U,0U);b+=test_put_num(body+b,1U,0U); }
        if (radio_fault == 12U) { b+=test_put_bytes(body+b,1U,(const uint8_t *)"x",1U); }
    } else if (id == 277U && mock_monitor_reject) {
        b += test_put_num(body+b,1U,0x102U);
    } else if (id == 350U) {
        b += test_put_num(body + b, 2U, mock_bad_version ? 4U : 3U);
        /* protobuf omits the zero-valued minor field on the real CP. */
        b += test_put_num(body + b, 4U, 9U);
        mock_seen |= 1U;
    } else if (id == 257U) {
        b += test_put_bytes(body + b, 1U, mac, sizeof(mac));
        mock_seen |= 2U;
    } else if (id == 288U) { b += test_put_num(body + b, 2U, 1U); }
    else if (id == 351U || id == 294U) {
        uint8_t record[64]; size_t r = 0U;
        const uint8_t bssid[6] = {2,4,6,8,10,12};
        r += test_put_bytes(record + r, 1U, bssid, 6U);
        r += test_put_bytes(record + r, 2U, (const uint8_t *)"test-ap", 7U);
        r += test_put_num(record + r, 3U, 6U);
        r += test_put_num(record + r, 5U, (uint32_t)-42);
        r += test_put_num(record + r, 6U, 3U);
        b += test_put_bytes(body + b, 2U, record, ap_info_bad && id == 294U ? 2U : r);
    } else if (id == 286U) {
        const uint8_t *cursor = rpc;
        (void)test_read_varint(&cursor); /* request body field tag */
        size_t body_length = (size_t)test_read_varint(&cursor);
        static const uint8_t active_time[] = {0x32U, 0x06U, 0x0AU, 0x04U,
                                               0x08U, 30U, 0x10U, 120U};
        for (size_t i = 0U; i + sizeof(active_time) <= body_length; ++i) {
            if (!memcmp(cursor + i, active_time, sizeof(active_time))) {
                scan_config_seen = 1U; break;
            }
        }
        if (scan_rejected) { b += test_put_num(body + b, 1U, 1U); }
        else { scan_event_pending = 1U; }
    }
    else if (id == 278U) { mock_seen |= 4U; }
    else if (id == 259U) {
        mock_seen |= 8U;
        if (query_fault == 1U) { b += test_put_num(body + b, 2U, 1U); }
        else if (query_fault == 9U) { b += test_put_num(body + b, 1U, 4U); }
        else if (mock_mode) { b += test_put_num(body + b, 1U, mock_mode); }
    }
    else if (id == 260U) {
        mock_seen |= 16U;
        if (mock_error_mode) { b += test_put_num(body + b, 1U, 1U); }
    } else if (id == 280U) { mock_seen |= 32U; }
    else if (id == 284U) {
        mock_seen |= 64U;
        if (mock_error_mode) { b += test_put_num(body + b, 1U, 1U); }
    }
    else if (id == 270U) {
        if (query_fault == 1U) { b += test_put_num(body + b, 1U, 1U); }
        else {
            const uint8_t *cursor = rpc;
            (void)test_read_varint(&cursor);
            (void)test_read_varint(&cursor);
            if (*cursor++ == 8U) { mock_ps = (unsigned)test_read_varint(&cursor); }
            else { mock_ps = 0U; }
        }
    }
    else if (id == 271U) {
        if (query_fault == 1U) { b += test_put_num(body + b, 1U, 1U); }
        else if (query_fault == 9U) { b += test_put_num(body + b, 2U, 3U); }
        else if (mock_ps) { b += test_put_num(body + b, 2U, mock_ps); }
    }
    else if (id == 285U) {
        if (query_fault == 1U) { b += test_put_num(body + b, 1U, 1U); }
        else if (query_fault != 2U) {
            uint8_t entry[192] = {0}, config[208] = {0}; size_t e = 0U, c = 0U;
            const uint8_t *cursor = rpc;
            (void)test_read_varint(&cursor);
            (void)test_read_varint(&cursor);
            if (*cursor++ == 8U) { mock_iface = (unsigned)test_read_varint(&cursor); }
            if (query_fault == 6U) { entry[e++] = 0x0AU; entry[e++] = 0x7FU; }
            else {
                uint8_t too_long[33]; memset(too_long, 'X', sizeof(too_long));
                const char *ssid = mock_iface == EH_WIFI_IF_STA ? "test-network" : "hosted-test";
                e += test_put_bytes(entry + e, 1U,
                     query_fault == 3U ? too_long : (const uint8_t *)ssid,
                     query_fault == 3U ? sizeof(too_long) : strlen(ssid));
                e += test_put_bytes(entry + e, 2U, (const uint8_t *)"secret-password", 15U);
                if (mock_iface == EH_WIFI_IF_AP) {
                    e += test_put_num(entry + e, 4U, 6U);
                    e += test_put_num(entry + e, 5U, 3U);
                    e += test_put_num(entry + e, 7U, 4U);
                }
            }
            c += test_put_bytes(config + c,
                   (mock_iface == EH_WIFI_IF_STA) != (query_fault == 5U) ? 2U : 1U,
                   entry, e);
            /* Proto3 omits the zero-valued STA iface in the actual CP response. */
            if (mock_iface || query_fault == 4U) {
                b += test_put_num(body + b, 2U,
                      query_fault == 4U ? 1U - mock_iface : mock_iface);
            }
            b += test_put_bytes(body + b, 3U, config, c);
        }
    }
    else if (id == 341U || id == 302U || id == 311U || id == 312U || id == 293U) {
        if (runtime_fault == 1U) { b += test_put_num(body + b, 1U, 0x3001U); }
        else if (id == 341U) {
            if (runtime_fault != 2U && runtime_fault != 14U) {
                if (runtime_fault == 5U) { b += test_put_bytes(body + b, 2U, mac, 6U); }
                else { b += test_put_num(body + b, 2U,
                    runtime_fault == 3U ? 128U : runtime_fault == 13U ? (uint64_t)(int64_t)-42 : (uint32_t)-42); }
                if (runtime_fault == 4U) { b += test_put_num(body + b, 2U, 10U); }
            }
        } else if (id == 302U) {
            if (runtime_fault != 2U) {
                b += test_put_num(body + b, 2U, runtime_primary);
                if (runtime_second || runtime_fault == 3U) {
                    b += test_put_num(body + b, 3U, runtime_fault == 3U ? 3U : runtime_second);
                }
                if (runtime_fault == 4U) { b += test_put_num(body + b, 2U, 6U); }
                if (runtime_fault == 5U) { b += test_put_bytes(body + b, 3U, mac, 6U); }
            }
        } else if (id == 311U) {
            if (runtime_fault != 2U) {
                uint8_t list[256] = {0}; size_t l = 0U;
                for (unsigned i = 0U; i < runtime_clients; ++i) {
                    uint8_t entry[64], client[6]; size_t e = 0U;
                    memcpy(client, mac, 6U); client[5] += (uint8_t)i;
                    if (runtime_fault == 16U && i == 1U) { client[0] = 1U; }
                    e += test_put_bytes(entry + e, 1U, client,
                        runtime_fault == 11U && i == 1U ? 5U : 6U);
                    if (runtime_fault == 4U) { e += test_put_bytes(entry + e, 1U, client, 6U); }
                    if (runtime_fault == 5U) { e += test_put_num(entry + e, 1U, 1U); }
                    if (runtime_fault != 14U) { e += test_put_num(entry + e, 2U,
                        runtime_fault == 3U ? (uint64_t)(int64_t)-129 :
                        runtime_fault == 13U ? (uint64_t)(int64_t)-42 : (uint32_t)-42); }
                    l += test_put_bytes(list + l, 1U, entry, e);
                }
                if (runtime_clients || runtime_fault == 12U) {
                    l += test_put_num(list + l, 2U, runtime_fault == 12U ? runtime_clients + 1U : runtime_clients);
                }
                if (runtime_fault == 15U) { l += test_put_num(list + l, 2U, runtime_clients); }
                if (runtime_fault == 17U) { list[l++] = 0x1DU; list[l++] = 1U; } /* truncated unknown fixed32 */
                b += test_put_bytes(body + b, 2U, list, l);
            }
        } else if (id == 312U) {
            const uint8_t *cursor = rpc;
            uint64_t key = test_read_varint(&cursor), body_len = test_read_varint(&cursor);
            runtime_request_ok = key == (((uint64_t)312U << 3U) | 2U) && body_len == 8U &&
                cursor[0] == 10U && cursor[1] == 6U && memcmp(cursor + 2U, mac, 6U) == 0;
            if (runtime_fault != 2U) {
                b += test_put_num(body + b, 2U, runtime_fault == 3U ? 2008U : 7U);
                if (runtime_fault == 4U) { b += test_put_num(body + b, 2U, 8U); }
                if (runtime_fault == 5U) { b += test_put_bytes(body + b, 2U, mac, 6U); }
            }
        } else {
            const uint8_t *cursor = rpc;
            uint64_t key = test_read_varint(&cursor), body_len = test_read_varint(&cursor);
            runtime_request_ok = key == (((uint64_t)293U << 3U) | 2U) && body_len == 2U &&
                cursor[0] == 8U && cursor[1] == 7U;
            if (runtime_fault == 4U) { b += test_put_num(body + b, 1U, 0U); b += test_put_num(body + b, 1U, 0U); }
            if (runtime_fault == 5U) { b += test_put_bytes(body + b, 1U, mac, 6U); }
        }
        if (runtime_fault == 6U) { body[b++] = 0x80U; }
        if (runtime_fault == 9U) { uint8_t pad[514] = {0}; b += test_put_bytes(body + b, 20U, pad, sizeof(pad)); }
        if (runtime_fault == 10U) {
            body[b++] = 0xA5U; body[b++] = 1U; memset(body + b, 0xAA, 4U); b += 4U;
            body[b++] = 0xA9U; body[b++] = 1U; memset(body + b, 0xBB, 8U); b += 8U;
            b += test_put_num(body + b, 22U, 99U);
        }
    }
    else if (id == 282U) { mock_seen |= 128U; mock_connect_event = 1U; }
    else if (id == 283U) { mock_seen |= 256U; }
    n += test_put_num(payload + n, 1U, 2U);
    n += test_put_num(payload + n, 2U, id + 256U);
    n += test_put_num(payload + n, 3U, query_fault == 8U || runtime_fault == 8U || radio_fault == 8U || advanced_fault == 8U ? uid - 1U : uid);
    n += test_put_bytes(payload + n, id + 256U, body, b);
    wire[w++] = 1U; wire[w++] = sizeof(ep) - 1U; wire[w++] = 0U;
    memcpy(wire + w, ep, sizeof(ep) - 1U); w += sizeof(ep) - 1U;
    wire[w++] = 2U; wire[w++] = (uint8_t)n; wire[w++] = (uint8_t)(n >> 8U);
    memcpy(wire + w, payload, n); w += n;
    if (id >= 297U && id <= 300U && radio_fault == 14U) {
        memcpy(radio_delayed, wire, w); radio_delayed_len = w;
        return; /* Deliver this transaction only after its caller has timed out. */
    }
    if (id >= 297U && id <= 300U && radio_delayed_len) {
        memcpy(radio_current, wire, w); radio_current_len = w;
        (void)esp_hosted_encode_frame(mock_host, 3U, 0U, 2U, radio_delayed,
                                     radio_delayed_len, rx, length);
        radio_delayed_len = 0U;
        return;
    }
    (void)esp_hosted_encode_frame(mock_host, 3U, 0U, 2U, wire, w, rx, length);
}
static int test_successful_rpc_sequence(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    esp_hosted_handle_t h = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    esp_hosted_version_t version;
    uint8_t mac[6];
    TEST_ASSERT(h != NULL);
    mock_host = h; mock_stage = mock_requests = mock_connect_event = mock_disconnect_event = mock_error_mode = 0U;
    mock_seen = scan_event_pending = scan_config_seen = scan_rejected = 0U;
    scan_result_status = ap_info_bad = query_fault = mock_ps = mock_iface = 0U; mock_mode = EH_WIFI_MODE_STA;
    test_hal_set_frame_callback(mock_cp);
    stm_err_t start_result = esp_hosted_start(h, 500U);
    if (start_result != STM_OK) { fprintf(stderr, "start=%ld stage=%u requests=%u seen=%lu\n", (long)start_result, mock_stage, mock_requests, (unsigned long)mock_seen); }
    TEST_ASSERT(start_result == STM_OK);
    TEST_ASSERT(esp_hosted_get_version(h, &version) == STM_OK);
    TEST_ASSERT(version.major == 3U && version.minor == 0U && version.patch == 9U);
    eh_wifi_status_t snapshot;
    TEST_ASSERT(eh_wifi_get_status(h, &snapshot) == STM_OK && !snapshot.started);
    TEST_ASSERT(eh_wifi_init(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_set_mode(h, EH_WIFI_MODE_STA, 500U) == STM_OK);
    eh_wifi_mode_t actual_mode = EH_WIFI_MODE_NULL;
    TEST_ASSERT(eh_wifi_get_mode(h, &actual_mode, 500U) == STM_OK && actual_mode == EH_WIFI_MODE_STA);
    for (unsigned fault = 1U; fault <= 9U; ++fault) {
        if (fault == 2U || fault == 3U || fault == 4U || fault == 5U || fault == 6U) { continue; }
        query_fault = fault; actual_mode = EH_WIFI_MODE_AP;
        stm_err_t expected = fault == 1U ? STM_ERR_IO :
                             fault == 9U ? STM_ERR_VERIFY : STM_ERR_TIMEOUT;
        TEST_ASSERT(eh_wifi_get_mode(h, &actual_mode, 5U) == expected &&
                    actual_mode == EH_WIFI_MODE_AP);
    }
    query_fault = 0U;
    eh_wifi_ps_t ps = EH_WIFI_PS_NONE;
    for (unsigned mode = 0U; mode < 3U; ++mode) {
        TEST_ASSERT(eh_wifi_set_ps(h, (eh_wifi_ps_t)mode, 500U) == STM_OK);
        TEST_ASSERT(eh_wifi_get_ps(h, &ps, 500U) == STM_OK && ps == (eh_wifi_ps_t)mode);
    }
    TEST_ASSERT(eh_wifi_set_ps(h, (eh_wifi_ps_t)3U, 500U) == STM_ERR_INVALID_ARG);
    query_fault = 1U;
    TEST_ASSERT(eh_wifi_set_ps(h, EH_WIFI_PS_NONE, 500U) == STM_ERR_IO);
    TEST_ASSERT(eh_wifi_get_ps(h, &ps, 500U) == STM_ERR_IO && ps == EH_WIFI_PS_MAX_MODEM);
    query_fault = 9U;
    TEST_ASSERT(eh_wifi_get_ps(h, &ps, 500U) == STM_ERR_VERIFY && ps == EH_WIFI_PS_MAX_MODEM);
    query_fault = 0U;
    TEST_ASSERT(eh_wifi_get_mac(h, EH_WIFI_IF_STA, mac) == STM_OK);
    TEST_ASSERT(mac[0] == 2U && mac[5] == 0x55U);
    eh_wifi_config_t config = {0};
    strcpy(config.sta.ssid, "test-network");
    strcpy(config.sta.password, "test-password");
    mock_error_mode = 1U;
    TEST_ASSERT(eh_wifi_set_config(h, EH_WIFI_IF_STA, &config, 500U) == STM_ERR_IO);
    mock_error_mode = 0U;
    TEST_ASSERT(eh_wifi_set_config(h, EH_WIFI_IF_STA, &config, 500U) == STM_OK);
    eh_wifi_config_info_t info = {0};
    TEST_ASSERT(eh_wifi_get_config(h, EH_WIFI_IF_STA, &info, 500U) == STM_OK &&
                !strcmp(info.ssid, "test-network") && info.channel == 0U);
    TEST_ASSERT(sizeof(info) < 65U);
    TEST_ASSERT(eh_wifi_get_config(h, (eh_wifi_if_t)2U, &info, 500U) == STM_ERR_INVALID_ARG);
    for (unsigned fault = 1U; fault <= 8U; ++fault) {
        query_fault = fault;
        memset(&info, 0xA5, sizeof(info));
        stm_err_t expected = fault == 1U ? STM_ERR_IO :
                             fault >= 7U ? STM_ERR_TIMEOUT : STM_ERR_VERIFY;
        TEST_ASSERT(eh_wifi_get_config(h, EH_WIFI_IF_STA, &info, 5U) == expected);
        TEST_ASSERT((unsigned char)info.ssid[0] == 0xA5U);
    }
    query_fault = 0U;
    TEST_ASSERT(eh_wifi_get_config(h, EH_WIFI_IF_STA, &info, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_start(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_connect(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_get_status(h, &snapshot) == STM_OK && snapshot.started &&
                snapshot.mode == EH_WIFI_MODE_STA && !snapshot.sta_connected);
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK && eh_wifi_is_connected(h));
    TEST_ASSERT(eh_wifi_get_status(h, &snapshot) == STM_OK && snapshot.sta_connected);
    eh_wifi_ap_record_t associated;
    ap_info_bad = 1U;
    TEST_ASSERT(eh_wifi_sta_get_ap_info(h, &associated, 500U) == STM_ERR_VERIFY);
    ap_info_bad = 0U;
    TEST_ASSERT(eh_wifi_sta_get_ap_info(h, &associated, 500U) == STM_OK);
    TEST_ASSERT(!strcmp(associated.ssid, "test-ap") && associated.channel == 6U && associated.rssi == -42);
    TEST_ASSERT(eh_wifi_disconnect(h, 500U) == STM_OK && !eh_wifi_is_connected(h));
    TEST_ASSERT(eh_wifi_sta_get_ap_info(h, &associated, 500U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_connect(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_set_event_callback(h, wifi_event_cb, NULL) == STM_OK);
    eh_wifi_scan_config_t scan_config = {.show_hidden = 1U};
    scan_rejected = 1U;
    TEST_ASSERT(eh_wifi_scan_start(h, &scan_config, 500U) == STM_ERR_IO);
    scan_rejected = 0U;
    TEST_ASSERT(eh_wifi_scan_start(h, &scan_config, 500U) == STM_OK);
    TEST_ASSERT(scan_config_seen);
    TEST_ASSERT(eh_wifi_get_status(h, &snapshot) == STM_OK && snapshot.scan_pending);
    TEST_ASSERT(eh_wifi_scan_start(h, &scan_config, 500U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_scan_stop(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_get_status(h, &snapshot) == STM_OK && !snapshot.scan_pending);
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK && wifi_event_count == 0U); /* late completion */
    TEST_ASSERT(eh_wifi_scan_start(h, &scan_config, 500U) == STM_OK);
    scan_result_status = 1U;
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK);
    TEST_ASSERT(last_wifi_event.id == EH_WIFI_EVENT_SCAN_DONE && last_wifi_event.scan_status == 1U);
    TEST_ASSERT(eh_wifi_scan_get_results(h, NULL, 0U, &(size_t){0}, 500U) == STM_ERR_INVALID_STATE);
    scan_result_status = 0U;
    TEST_ASSERT(eh_wifi_scan_start(h, &scan_config, 500U) == STM_OK);
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK);
    TEST_ASSERT(wifi_event_count == 2U && last_wifi_event.id == EH_WIFI_EVENT_SCAN_DONE &&
                last_wifi_event.scan_status == 0U &&
                last_wifi_event.scan_count == 1U);
    eh_wifi_ap_record_t records[1]; size_t count = 0U;
    TEST_ASSERT(eh_wifi_scan_get_results(h, records, 0U, &count, 500U) == STM_ERR_OUT_OF_RANGE && count == 1U);
    TEST_ASSERT(eh_wifi_scan_get_results(h, records, 1U, &count, 500U) == STM_OK && count == 1U);
    TEST_ASSERT(!strcmp(records[0].ssid, "test-ap") && records[0].channel == 6U && records[0].rssi == -42);
    TEST_ASSERT(eh_wifi_scan_get_results(h, records, 1U, &count, 500U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_stop(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_set_mode(h, EH_WIFI_MODE_APSTA, 500U) == STM_OK);
    eh_wifi_config_t ap_config = {0};
    strcpy(ap_config.ap.ssid, "hosted-test");
    strcpy(ap_config.ap.password, "test-password");
    ap_config.ap.channel = 6U;
    TEST_ASSERT(eh_wifi_set_config(h, EH_WIFI_IF_AP, &ap_config, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_get_config(h, EH_WIFI_IF_AP, &info, 500U) == STM_OK &&
                !strcmp(info.ssid, "hosted-test") && info.channel == 6U &&
                info.authmode == 3U && info.max_connections == 4U);
    TEST_ASSERT(eh_wifi_start(h, 500U) == STM_OK);
    TEST_ASSERT(mock_seen == 511U && mock_requests >= 15U);
    test_hal_set_frame_callback(NULL);
    TEST_ASSERT(esp_hosted_delete(&h) == STM_OK);
    return 0;
}


static int test_reconnect_policy(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    esp_hosted_handle_t h = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h != NULL);
    mock_host = h; mock_stage = mock_requests = mock_connect_event = mock_disconnect_event = mock_error_mode = 0U;
    mock_seen = scan_event_pending = scan_config_seen = scan_rejected = 0U;
    scan_result_status = ap_info_bad = query_fault = mock_ps = mock_iface = 0U;
    mock_mode = EH_WIFI_MODE_STA;
    test_hal_set_frame_callback(mock_cp);
    TEST_ASSERT(esp_hosted_start(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_init(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_set_mode(h, EH_WIFI_MODE_STA, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_start(h, 500U) == STM_OK);
    eh_wifi_reconnect_config_t config = {.enabled = 1U, .initial_delay_ms = 100U,
        .max_delay_ms = 400U, .association_timeout_ms = 60U,
        .rpc_timeout_ms = 50U, .max_attempts = 2U};
    config.max_delay_ms = 99U;
    TEST_ASSERT(eh_wifi_set_reconnect(h, &config) == STM_ERR_INVALID_ARG);
    config.max_delay_ms = 400U;
    TEST_ASSERT(eh_wifi_set_reconnect(h, &config) == STM_OK);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && !h->reconnect_armed);
    TEST_ASSERT(eh_wifi_connect(h, 500U) == STM_OK && h->connect_pending);
    mock_connect_event = 0U; /* CP accepts the request but never associates. */
    HAL_Delay(65U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && h->reconnect_waiting);
    unsigned before = mock_requests;
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && mock_requests == before);
    HAL_Delay(105U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && mock_requests == before + 1U);
    TEST_ASSERT(h->reconnect_attempts == 1U && h->reconnect_delay == 200U);
    mock_connect_event = 0U;
    HAL_Delay(65U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && h->reconnect_waiting);
    HAL_Delay(105U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && mock_requests == before + 1U);
    HAL_Delay(105U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && mock_requests == before + 2U);
    TEST_ASSERT(h->reconnect_attempts == 2U && h->reconnect_delay == 400U);
    mock_connect_event = 0U;
    HAL_Delay(65U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && !h->reconnect_waiting);
    HAL_Delay(1000U);
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && mock_requests == before + 2U);
    TEST_ASSERT(eh_wifi_connect(h, 500U) == STM_OK);
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK && eh_wifi_is_connected(h));
    TEST_ASSERT(h->reconnect_attempts == 0U && h->reconnect_delay == 100U);
    mock_disconnect_event = 1U;
    TEST_ASSERT(esp_hosted_poll(h) == STM_OK && h->reconnect_waiting);
    TEST_ASSERT(eh_wifi_disconnect(h, 500U) == STM_OK && !h->reconnect_armed);
    HAL_Delay(1000U);
    before = mock_requests;
    TEST_ASSERT(eh_wifi_reconnect_update(h) == STM_OK && mock_requests == before);
    TEST_ASSERT(eh_wifi_connect(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_stop(h, 500U) == STM_OK && !h->reconnect_armed);
    config.enabled = 0U;
    TEST_ASSERT(eh_wifi_set_reconnect(h, &config) == STM_OK);
    TEST_ASSERT(eh_wifi_start(h, 500U) == STM_OK);
    TEST_ASSERT(eh_wifi_connect(h, 500U) == STM_OK && !h->reconnect_armed);
    mock_connect_event = 0U;
    test_hal_set_frame_callback(NULL);
    TEST_ASSERT(esp_hosted_delete(&h) == STM_OK);
    return 0;
}

static int test_runtime_queries(void)
{
    static uint8_t tx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    static uint8_t rx[ESP_HOSTED_FRAME_SIZE] __attribute__((aligned(ESP_HOSTED_DMA_ALIGNMENT)));
    const uint8_t mac[6] = {2,0x11,0x22,0x33,0x44,0x55};
    esp_hosted_handle_t h = make_handle(tx, rx, ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h != NULL);
    mock_host = h; mock_stage = 1U;
    mock_connect_event = mock_disconnect_event = scan_event_pending = query_fault = runtime_fault = 0U;
    test_hal_set_frame_callback(mock_cp);
    h->negotiated = 1U; h->wifi_started = 1U; h->wifi_mode = EH_WIFI_MODE_APSTA;
    h->connected = 1U; h->ap_up = 1U;
    int8_t rssi = 11; uint8_t primary = 99U; uint16_t aid = 99U;
    eh_wifi_second_chan_t second = EH_WIFI_SECOND_CHAN_BELOW;
    eh_wifi_sta_record_t records[2], saved[2]; memset(saved, 0xA5, sizeof(saved));
    size_t count = 99U;
    TEST_ASSERT(eh_wifi_sta_get_rssi(h, &rssi, 50U) == STM_OK && rssi == -42);
    for (unsigned i = 0U; i <= 2U; ++i) {
        runtime_second = i;
        TEST_ASSERT(eh_wifi_get_channel(h, &primary, &second, 50U) == STM_OK && primary == 6U && (unsigned)second == i);
    }
    runtime_second = 0U;
    TEST_ASSERT(eh_wifi_ap_get_sta_aid(h, mac, &aid, 50U) == STM_OK && aid == 7U && runtime_request_ok);
    TEST_ASSERT(eh_wifi_deauth_sta(h, 7U, 50U) == STM_OK && runtime_request_ok && h->ap_up);
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, NULL, 0U, &count, 50U) == STM_OK && count == 2U);
    memcpy(records, saved, sizeof(records));
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 1U, &count, 50U) == STM_ERR_OUT_OF_RANGE && count == 2U);
    TEST_ASSERT(memcmp(records, saved, sizeof(records)) == 0);
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 2U, &count, 50U) == STM_OK && count == 2U);
    TEST_ASSERT(memcmp(records[0].mac, mac, 6U) == 0 && records[1].mac[5] == 0x56U && records[1].rssi == -42);
    runtime_clients = 0U;
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 0U, &count, 50U) == STM_OK && count == 0U);
    runtime_clients = 2U;
    for (unsigned fault = 1U; fault <= 14U; ++fault) {
        runtime_fault = fault;
        stm_err_t expected = fault == 1U ? STM_ERR_IO :
            (fault == 7U || fault == 8U) ? STM_ERR_TIMEOUT : STM_ERR_VERIFY;
        int valid_rssi = fault == 2U || fault == 10U || fault == 11U || fault == 12U || fault == 13U || fault == 14U;
        rssi = 11;
        TEST_ASSERT(eh_wifi_sta_get_rssi(h, &rssi, 20U) == (valid_rssi ? STM_OK : expected));
        TEST_ASSERT(rssi == (valid_rssi ? ((fault == 2U || fault == 14U) ? 0 : -42) : 11));
        int valid_channel = fault >= 10U;
        primary = 99U; second = EH_WIFI_SECOND_CHAN_BELOW;
        TEST_ASSERT(eh_wifi_get_channel(h, &primary, &second, 20U) == (valid_channel ? STM_OK : expected));
        TEST_ASSERT(primary == (valid_channel ? 6U : 99U) && second == (valid_channel ? EH_WIFI_SECOND_CHAN_NONE : EH_WIFI_SECOND_CHAN_BELOW));
        int valid_list = fault == 10U || fault == 13U || fault == 14U;
        count = 99U; memcpy(records, saved, sizeof(records));
        TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 2U, &count, 20U) == (valid_list ? STM_OK : expected));
        TEST_ASSERT(count == (valid_list ? 2U : 99U));
        if (valid_list) { TEST_ASSERT(records[0].rssi == (fault == 14U ? 0 : -42)); }
        else { TEST_ASSERT(memcmp(records, saved, sizeof(records)) == 0); }
        int valid_aid = fault >= 10U;
        aid = 99U;
        TEST_ASSERT(eh_wifi_ap_get_sta_aid(h, mac, &aid, 20U) == (valid_aid ? STM_OK : expected));
        TEST_ASSERT(aid == (valid_aid ? 7U : 99U));
        int valid_deauth = fault == 2U || fault == 3U || fault >= 10U;
        TEST_ASSERT(eh_wifi_deauth_sta(h, 7U, 20U) == (valid_deauth ? STM_OK : expected));
    }
    runtime_fault = 0U;
    for (unsigned fault = 15U; fault <= 17U; ++fault) {
        runtime_fault = fault; count = 99U; memcpy(records, saved, sizeof(records));
        TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 2U, &count, 50U) == STM_ERR_VERIFY);
        TEST_ASSERT(count == 99U && memcmp(records, saved, sizeof(records)) == 0);
    }
    runtime_fault = 0U;
    /* A stale reply was discarded; a new UID transaction can still complete. */
    TEST_ASSERT(eh_wifi_sta_get_rssi(h, &rssi, 50U) == STM_OK && rssi == -42);
    runtime_primary = 0U;
    TEST_ASSERT(eh_wifi_get_channel(h, &primary, &second, 50U) == STM_ERR_VERIFY);
    runtime_primary = 15U;
    TEST_ASSERT(eh_wifi_get_channel(h, &primary, &second, 50U) == STM_ERR_VERIFY);
    runtime_primary = 6U;
    unsigned before = mock_requests;
    TEST_ASSERT(eh_wifi_sta_get_rssi(NULL, &rssi, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_sta_get_rssi(h, NULL, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_channel(h, &primary, NULL, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, NULL, 1U, &count, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 2U, NULL, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_ap_get_sta_aid(h, (const uint8_t[6]){0}, &aid, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_ap_get_sta_aid(h, (const uint8_t[6]){1,2,3,4,5,6}, &aid, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_deauth_sta(h, 0U, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_deauth_sta(h, 2008U, 50U) == STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_deauth_sta(h, 7U, 0U) == STM_ERR_INVALID_ARG);
    h->connected = 0U;
    TEST_ASSERT(eh_wifi_sta_get_rssi(h, &rssi, 50U) == STM_ERR_INVALID_STATE);
    h->ap_up = 0U;
    TEST_ASSERT(eh_wifi_ap_get_sta_list(h, records, 2U, &count, 50U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_ap_get_sta_aid(h, mac, &aid, 50U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_deauth_sta(h, 7U, 50U) == STM_ERR_INVALID_STATE);
    h->wifi_started = 0U;
    TEST_ASSERT(eh_wifi_get_channel(h, &primary, &second, 50U) == STM_ERR_INVALID_STATE);
    TEST_ASSERT(mock_requests == before);
    test_hal_set_frame_callback(NULL);
    TEST_ASSERT(esp_hosted_delete(&h) == STM_OK);
    return 0;
}

static int test_radio_config(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32))), rx[1600] __attribute__((aligned(32)));
    esp_hosted_handle_t h=make_handle(tx,rx,ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h);
    mock_host=h;mock_stage=1U;mock_connect_event=mock_disconnect_event=scan_event_pending=0U;
    query_fault=runtime_fault=radio_fault=0U;
    h->negotiated=1U;h->wifi_initialized=1U;h->wifi_mode=EH_WIFI_MODE_APSTA;
    test_hal_set_frame_callback(mock_cp);
    uint8_t bitmap=99U;eh_wifi_bandwidth_t bw=(eh_wifi_bandwidth_t)99;
    /* Configuring a stopped, unassociated interface is supported. */
    const uint8_t masks[]={1U,3U,7U};
    for(unsigned iface=0;iface<2;++iface) {
        for(unsigned i=0;i<3;++i) {
            TEST_ASSERT(eh_wifi_set_bandwidth(h,(eh_wifi_if_t)iface,EH_WIFI_BW_HT20,30U)==STM_OK);
            TEST_ASSERT(eh_wifi_set_protocol(h,(eh_wifi_if_t)iface,masks[i],30U)==STM_OK);
            TEST_ASSERT(eh_wifi_get_protocol(h,(eh_wifi_if_t)iface,&bitmap,30U)==STM_OK && bitmap==masks[i]);
            TEST_ASSERT(eh_wifi_get_bandwidth(h,(eh_wifi_if_t)iface,&bw,30U)==STM_OK && bw==EH_WIFI_BW_HT20);
        }
        unsigned before=mock_requests;
        TEST_ASSERT(eh_wifi_set_bandwidth(h,(eh_wifi_if_t)iface,EH_WIFI_BW_HT40,30U)==STM_OK);
        TEST_ASSERT(mock_requests==before+1U); /* No implicit protocol read or set. */
        TEST_ASSERT(eh_wifi_get_bandwidth(h,(eh_wifi_if_t)iface,&bw,30U)==STM_OK && bw==EH_WIFI_BW_HT40);
    }
    TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_AP,EH_WIFI_BW_HT20,30U)==STM_OK);
    TEST_ASSERT(eh_wifi_set_protocol(h,EH_WIFI_IF_AP,1U,30U)==STM_OK);
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_STA,&bitmap,30U)==STM_OK && bitmap==7U);
    TEST_ASSERT(eh_wifi_get_bandwidth(h,EH_WIFI_IF_STA,&bw,30U)==STM_OK && bw==EH_WIFI_BW_HT40);
    TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_AP,EH_WIFI_BW_HT40,30U)==STM_ERR_IO);
    for(unsigned f=1;f<=12;++f) {
        radio_fault=f;bitmap=99U;bw=(eh_wifi_bandwidth_t)99;radio_bw[0]=2U;
        stm_err_t expected=f==1U ? STM_ERR_IO : (f==7U || f==8U) ? STM_ERR_TIMEOUT : f==9U ? STM_ERR_OUT_OF_RANGE : f==10U ? STM_OK : STM_ERR_VERIFY;
        TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_STA,&bitmap,20U)==expected);
        TEST_ASSERT(eh_wifi_get_bandwidth(h,EH_WIFI_IF_STA,&bw,20U)==expected);
        TEST_ASSERT(bitmap==(f==10U?7U:99U) && bw==(f==10U?EH_WIFI_BW_HT40:(eh_wifi_bandwidth_t)99));
        if(f==1U || f==7U || f==8U || f==11U || f==12U || f==6U) {
            TEST_ASSERT(eh_wifi_set_protocol(h,EH_WIFI_IF_STA,7U,20U)==expected);
            TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_STA,EH_WIFI_BW_HT20,20U)==expected);
        }
    }
    radio_fault=0U;
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_STA,&bitmap,30U)==STM_OK && bitmap==7U);
    const uint64_t bad_values[]={2U,4U,5U,6U,8U,255U,263U,UINT64_MAX};
    radio_fault=13U;
    for(unsigned i=0;i<sizeof(bad_values)/sizeof(bad_values[0]);++i) {
        radio_invalid_value=bad_values[i];bitmap=99U;
        TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_STA,&bitmap,20U)==STM_ERR_VERIFY);
        TEST_ASSERT(bitmap==99U);
    }
    const uint64_t bad_bandwidths[]={3U,257U,UINT64_MAX};
    for(unsigned i=0;i<sizeof(bad_bandwidths)/sizeof(bad_bandwidths[0]);++i) {
        radio_invalid_value=bad_bandwidths[i];bw=(eh_wifi_bandwidth_t)99;
        TEST_ASSERT(eh_wifi_get_bandwidth(h,EH_WIFI_IF_STA,&bw,20U)==STM_ERR_VERIFY);
        TEST_ASSERT(bw==(eh_wifi_bandwidth_t)99);
    }
    radio_fault=14U;bitmap=99U;radio_protocol[0]=1U;
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_STA,&bitmap,20U)==STM_ERR_TIMEOUT && bitmap==99U);
    radio_fault=0U;radio_protocol[0]=7U;
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_STA,&bitmap,30U)==STM_OK && bitmap==7U);
    TEST_ASSERT(!radio_delayed_len && !radio_current_len);
    for(unsigned bad=0;bad<256;++bad) {
        if(bad==1U || bad==3U || bad==7U)continue;
        TEST_ASSERT(eh_wifi_set_protocol(h,EH_WIFI_IF_STA,(uint8_t)bad,20U)==STM_ERR_INVALID_ARG);
    }
    unsigned before=mock_requests;
    TEST_ASSERT(eh_wifi_set_protocol(NULL,EH_WIFI_IF_STA,7U,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_protocol(h,(eh_wifi_if_t)-1,7U,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_AP,NULL,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_AP,&bitmap,0U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_bandwidth(NULL,EH_WIFI_IF_STA,&bw,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_bandwidth(h,(eh_wifi_if_t)2,&bw,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_bandwidth(h,EH_WIFI_IF_AP,NULL,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_AP,(eh_wifi_bandwidth_t)-1,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_AP,(eh_wifi_bandwidth_t)3,20U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_AP,EH_WIFI_BW_HT20,0U)==STM_ERR_INVALID_ARG);
    h->wifi_initialized=0U;
    TEST_ASSERT(eh_wifi_set_protocol(h,EH_WIFI_IF_STA,7U,20U)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_get_bandwidth(h,EH_WIFI_IF_AP,&bw,20U)==STM_ERR_INVALID_STATE);
    h->wifi_initialized=1U;h->wifi_mode=EH_WIFI_MODE_STA;
    TEST_ASSERT(eh_wifi_get_protocol(h,EH_WIFI_IF_AP,&bitmap,20U)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_set_bandwidth(h,EH_WIFI_IF_AP,EH_WIFI_BW_HT20,20U)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(mock_requests==before);
    test_hal_set_frame_callback(NULL);
    TEST_ASSERT(esp_hosted_delete(&h)==STM_OK);
    return 0;
}

static int test_advanced_config(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32))), rx[1600] __attribute__((aligned(32)));
    esp_hosted_handle_t h=make_handle(tx,rx,ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h!=NULL);
    mock_host=h; mock_stage=0; query_fault=runtime_fault=radio_fault=advanced_fault=0;
    mock_connect_event=mock_disconnect_event=scan_event_pending=0;
    test_hal_set_frame_callback(mock_cp);
    TEST_ASSERT(esp_hosted_start(h,500U)==STM_OK);
    TEST_ASSERT(eh_wifi_init(h,100U)==STM_OK);
    TEST_ASSERT(eh_wifi_set_mode(h,EH_WIFI_MODE_APSTA,100U)==STM_OK);
    TEST_ASSERT(eh_wifi_start(h,100U)==STM_OK);
    char cc[4]; eh_wifi_country_info_t info;
    TEST_ASSERT(eh_wifi_set_country_code(h,"CN",0,100U)==STM_OK);
    TEST_ASSERT(eh_wifi_get_country_code(h,cc,100U)==STM_OK && !strcmp(cc,"CN"));
    TEST_ASSERT(eh_wifi_get_country(h,&info,100U)==STM_OK && !strcmp(info.country_code,"CN ") &&
                info.start_channel==1 && info.channel_count==13 && info.policy==EH_WIFI_COUNTRY_AUTO);
    TEST_ASSERT(eh_wifi_set_country_code(h,"01",1,100U)==STM_OK);
    TEST_ASSERT(eh_wifi_set_country_code(h,"CNX",1,100U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_country_code(h,"cn",1,100U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_country_code(h,"CN",2,100U)==STM_ERR_INVALID_ARG);
    for(unsigned ch=1;ch<=14;++ch)TEST_ASSERT(eh_wifi_set_channel(h,ch,EH_WIFI_SECOND_CHAN_NONE,100U)==STM_OK);
    TEST_ASSERT(eh_wifi_set_channel(h,0,EH_WIFI_SECOND_CHAN_NONE,100U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_set_channel(h,6,(eh_wifi_second_chan_t)-1,100U)==STM_ERR_INVALID_ARG);
    h->connected=1;TEST_ASSERT(eh_wifi_set_channel(h,6,EH_WIFI_SECOND_CHAN_NONE,100U)==STM_ERR_INVALID_STATE);
    h->connected=0;h->scan_pending=1;TEST_ASSERT(eh_wifi_set_channel(h,6,EH_WIFI_SECOND_CHAN_NONE,100U)==STM_ERR_INVALID_STATE);
    h->scan_pending=0;h->connect_pending=1;TEST_ASSERT(eh_wifi_set_channel(h,6,EH_WIFI_SECOND_CHAN_NONE,100U)==STM_ERR_INVALID_STATE);h->connect_pending=0;
    int8_t power=0;
    for(int p=8;p<=84;++p) {
        TEST_ASSERT(eh_wifi_set_max_tx_power(h,(int8_t)p,100U)==STM_OK);
        TEST_ASSERT(eh_wifi_get_max_tx_power(h,&power,100U)==STM_OK && power==p);
    }
    for(int p=-128;p<128;++p)if(p<8 || p>84)TEST_ASSERT(eh_wifi_set_max_tx_power(h,(int8_t)p,100U)==STM_ERR_INVALID_ARG);
    for(unsigned fault=1;fault<=10;++fault) {
        advanced_fault=fault;power=99;memcpy(cc,"old",4);memset(&info,0xA5,sizeof(info));
        eh_wifi_country_info_t saved=info;
        stm_err_t expected=fault==1?STM_ERR_IO:(fault==7 || fault==8)?STM_ERR_TIMEOUT:STM_ERR_VERIFY;
        stm_err_t e=eh_wifi_get_max_tx_power(h,&power,30U);
        if(fault==10){TEST_ASSERT(e==STM_OK);continue;}
        TEST_ASSERT(e==expected || (fault==9 && e==STM_ERR_OUT_OF_RANGE));TEST_ASSERT(power==99);
        e=eh_wifi_get_country_code(h,cc,30U);
        TEST_ASSERT(e==expected || (fault==9 && e==STM_ERR_OUT_OF_RANGE));TEST_ASSERT(!memcmp(cc,"old",4));
        e=eh_wifi_get_country(h,&info,30U);
        TEST_ASSERT(e==expected || (fault==9 && e==STM_ERR_OUT_OF_RANGE));TEST_ASSERT(!memcmp(&info,&saved,sizeof(info)));
    }
    advanced_fault=1;
    TEST_ASSERT(eh_wifi_set_max_tx_power(h,20,100U)==STM_ERR_IO);
    esp_hosted_diagnostics_t d;
    TEST_ASSERT(esp_hosted_get_diagnostics(h,&d)==STM_OK && d.last_failed_rpc==275U && d.last_cp_status==0x1101U);
    uint32_t failed_tick=d.last_fault_tick;
    advanced_fault=0;TEST_ASSERT(eh_wifi_get_max_tx_power(h,&power,100U)==STM_OK);
    TEST_ASSERT(esp_hosted_get_diagnostics(h,&d)==STM_OK && d.last_fault_tick==failed_tick && d.last_cp_status==0x1101U);
    advanced_fault=2;power=99;
    TEST_ASSERT(eh_wifi_get_max_tx_power(h,&power,100U)==STM_ERR_VERIFY && power==99);
    TEST_ASSERT(esp_hosted_get_diagnostics(h,&d)==STM_OK && d.last_failed_rpc==276U &&
                d.last_fault==ESP_HOSTED_FAULT_RPC && d.last_error==STM_ERR_VERIFY);
    advanced_fault=0;
    h->wifi_started=0;TEST_ASSERT(eh_wifi_get_max_tx_power(h,&power,100U)==STM_ERR_INVALID_STATE);
    h->wifi_initialized=0;TEST_ASSERT(eh_wifi_get_country(h,&info,100U)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_get_country_code(h,NULL,100U)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_get_country(NULL,&info,100U)==STM_ERR_INVALID_ARG);
    test_hal_set_frame_callback(NULL);TEST_ASSERT(esp_hosted_delete(&h)==STM_OK);return 0;
}
static stm_err_t callback_result;
static void reentrant_link(void *user,uint8_t connected)
{
    (void)connected;callback_result=eh_wifi_stop((esp_hosted_handle_t)user,100U);
}
static int test_monitor_recovery(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32))), rx[1600] __attribute__((aligned(32)));
    esp_hosted_handle_t h=make_handle(tx,rx,ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h!=NULL);
    mock_host=h;mock_stage=0;query_fault=runtime_fault=radio_fault=advanced_fault=0;
    mock_connect_event=mock_disconnect_event=scan_event_pending=0;
    test_hal_set_frame_callback(mock_cp);
    TEST_ASSERT(esp_hosted_start(h,500U)==STM_OK);
    TEST_ASSERT(eh_wifi_init(h,100U)==STM_OK);
    esp_hosted_monitor_config_t monitor={.enabled=1,.interval_s=10,.timeout_ms=35000};
    TEST_ASSERT(esp_hosted_set_monitor(h,&monitor,100U)==STM_OK);
    mock_monitor_reject=1;
    esp_hosted_monitor_config_t disabled={0};
    TEST_ASSERT(esp_hosted_set_monitor(h,&disabled,100U)==STM_ERR_IO && h->monitor.enabled);
    mock_monitor_reject=0;
    monitor.interval_s=9;TEST_ASSERT(esp_hosted_set_monitor(h,&monitor,100U)==STM_ERR_INVALID_ARG);
    monitor.interval_s=10;monitor.timeout_ms=20000;TEST_ASSERT(esp_hosted_set_monitor(h,&monitor,100U)==STM_ERR_INVALID_ARG);
    monitor.timeout_ms=35000;
    TEST_ASSERT(esp_hosted_set_callbacks(h,NULL,reentrant_link,h)==STM_OK);
    h->connected=h->ap_up=h->scan_pending=h->wifi_started=1;
    h->reconnect_armed=h->reconnect_waiting=1;
    test_hal_set_signals(GPIO_PIN_RESET);
    test_hal_set_tick(h->heartbeat_tick+34980U);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->initialized);
    test_hal_set_tick(h->heartbeat_tick+35000U);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->initialized && !h->connected && !h->ap_up && !h->scan_pending && !h->reconnect_waiting);
    TEST_ASSERT(callback_result==STM_ERR_INVALID_CONTEXT);
    esp_hosted_diagnostics_t d;
    TEST_ASSERT(esp_hosted_get_diagnostics(h,&d)==STM_OK && d.state==ESP_HOSTED_STATE_FAULT && d.heartbeat_timeouts==1);
    uint32_t old_uid=h->uid;
    test_hal_set_signals(GPIO_PIN_SET);mock_stage=0;
    uint32_t before=HAL_GetTick();TEST_ASSERT(esp_hosted_recover_begin(h,1000U)==STM_OK);
    TEST_ASSERT(HAL_GetTick()-before<10U);
    TEST_ASSERT(esp_hosted_recover_begin(h,1000U)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_init(h,100U)==STM_ERR_INVALID_STATE);
    while(h->recovery_phase)TEST_ASSERT(esp_hosted_poll(h)==STM_OK);
    TEST_ASSERT(h->initialized && h->uid>old_uid && !h->wifi_initialized && h->monitor.enabled);
    TEST_ASSERT(esp_hosted_get_diagnostics(h,&d)==STM_OK && d.state==ESP_HOSTED_STATE_READY && d.recovery_successes==2);
    mock_stage=0;mock_monitor_reject=1;
    TEST_ASSERT(esp_hosted_start(h,1000U)==STM_ERR_IO && !h->initialized && h->monitor.enabled);
    mock_stage=0;mock_monitor_reject=0;
    TEST_ASSERT(esp_hosted_start(h,1000U)==STM_OK);
    /* Heartbeat receipt and expiration across the HAL tick wrap. */
    test_hal_set_tick(UINT32_MAX-20000U);h->heartbeat_tick=HAL_GetTick();
    test_hal_set_signals(GPIO_PIN_RESET);test_hal_set_tick(14000U);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->initialized);
    test_hal_set_tick(16000U);TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->initialized);
    test_hal_set_signals(GPIO_PIN_SET);mock_stage=0;
    TEST_ASSERT(esp_hosted_start(h,1000U)==STM_OK);
    /* Ready heartbeat, including protobuf omission of initial zero sequence. */
    test_hal_set_frame_callback(NULL);
    uint8_t payload[64],event[32],wire[80];size_t pn=0,wn=0,en=0;
    en+=test_put_num(event+en,1U,0U);
    pn+=test_put_num(payload+pn,1U,3U);pn+=test_put_num(payload+pn,2U,770U);
    pn+=test_put_bytes(payload+pn,770U,event,en);
    wire[wn++]=1U;wire[wn++]=0U;wire[wn++]=0U;wire[wn++]=2U;wire[wn++]=(uint8_t)pn;wire[wn++]=0U;
    memcpy(wire+wn,payload,pn);wn+=pn;
    test_hal_set_tick(h->heartbeat_tick+34900U);
    TEST_ASSERT(inject_payload(h,3U,wire,wn));TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->initialized);
    uint32_t hb=h->heartbeat_tick;test_hal_set_signals(GPIO_PIN_RESET);test_hal_set_tick(hb+34900U);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->initialized);
    test_hal_set_signals(GPIO_PIN_SET);test_hal_set_frame_callback(mock_cp);
    /* Disable monitor, then an indefinite idle link remains valid. */
    monitor.enabled=0;TEST_ASSERT(esp_hosted_set_monitor(h,&monitor,100U)==STM_OK);
    test_hal_set_tick(999999U);TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->initialized);
    /* Ready-state INIT invalidates the old session, without an automatic reset. */
    mock_stage=0;TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->initialized && h->diagnostics.state==ESP_HOSTED_STATE_FAULT);
    uint32_t generation=h->diagnostics.generation;mock_stage=0;
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->diagnostics.generation==generation);
    test_hal_set_signals(GPIO_PIN_RESET);TEST_ASSERT(esp_hosted_recover_begin(h,12U)==STM_OK);
    stm_err_t e=STM_OK;while(h->recovery_phase && e==STM_OK)e=esp_hosted_poll(h);
    TEST_ASSERT(e==STM_ERR_TIMEOUT && h->diagnostics.recovery_failures==2);
    mock_stage=0;mock_bad_version=1U;test_hal_set_signals(GPIO_PIN_SET);
    TEST_ASSERT(esp_hosted_start(h,1000U)==STM_ERR_NOT_SUPPORTED && !h->initialized);
    mock_bad_version=0U;
    h->diagnostics.rpc_timeouts=UINT32_MAX;test_hal_set_signals(GPIO_PIN_SET);mock_stage=0;
    TEST_ASSERT(esp_hosted_start(h,1000U)==STM_OK);TEST_ASSERT(eh_wifi_init(h,100U)==STM_OK);
    TEST_ASSERT(eh_wifi_set_mode(h,EH_WIFI_MODE_STA,100U)==STM_OK);h->wifi_started=1U;
    reset_during_request=1U;int8_t old_output=99;
    TEST_ASSERT(eh_wifi_get_max_tx_power(h,&old_output,100U)==STM_ERR_CANCELLED && old_output==99);
    TEST_ASSERT(!h->wifi_initialized && !h->initialized && !h->request_active);
    test_hal_set_frame_callback(NULL);
    const uint8_t old_connected[]={1,0,0,2,10,0,8,3,16,0x87,6,0xBA,0x30,2,0x12,0};
    TEST_ASSERT(inject_payload(h,3U,old_connected,sizeof(old_connected)));
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->connected);
    mock_stage=0;test_hal_set_frame_callback(mock_cp);
    TEST_ASSERT(esp_hosted_start(h,1000U)==STM_OK);TEST_ASSERT(eh_wifi_init(h,100U)==STM_OK);
    advanced_fault=7;int8_t power=99;h->wifi_started=1;
    TEST_ASSERT(eh_wifi_get_max_tx_power(h,&power,20U)==STM_ERR_TIMEOUT && h->diagnostics.rpc_timeouts==UINT32_MAX);
    advanced_fault=0;test_hal_set_frame_callback(NULL);
    TEST_ASSERT(esp_hosted_reset(h,10,100)==STM_OK && !h->initialized && !h->wifi_started);
    TEST_ASSERT(esp_hosted_delete(&h)==STM_OK);return 0;
}
static stm_err_t queued_results[3];
static void queued_receive(void *user,const uint8_t *frame,size_t length)
{
    for(unsigned i=0;i<3U;++i)queued_results[i]=esp_hosted_send(user,frame,length);
}
static int test_receive_queue(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32))), rx[1600] __attribute__((aligned(32)));
    esp_hosted_handle_t h=make_handle(tx,rx,ESP_HOSTED_DUMMY_IF_TYPE);uint8_t ethernet[64]={0};
    TEST_ASSERT(h!=NULL);h->initialized=h->connected=1U;h->diagnostics.state=ESP_HOSTED_STATE_READY;
    TEST_ASSERT(esp_hosted_set_callbacks(h,queued_receive,NULL,h)==STM_OK);
    TEST_ASSERT(inject_payload(h,ESP_HOSTED_STA_IF_TYPE,ethernet,sizeof(ethernet)));
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && queued_results[0]==STM_OK && queued_results[1]==STM_OK && queued_results[2]==STM_ERR_NO_MEM);
    TEST_ASSERT(h->queued_count==2U);
    TEST_ASSERT(esp_hosted_set_callbacks(h,NULL,NULL,NULL)==STM_OK);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->queued_count==1U);
    esp_hosted_frame_t f;TEST_ASSERT(esp_hosted_decode_frame(h,tx,sizeof(tx),&f)==ESP_HOSTED_FRAME_OK && f.payload_length==sizeof(ethernet));
    TEST_ASSERT(esp_hosted_reset(h,10,100)==STM_OK && !h->queued_count);
    TEST_ASSERT(esp_hosted_delete(&h)==STM_OK);return 0;
}

static uint8_t async_held[1600];
static unsigned async_hold, async_release, async_calls;
static uint8_t async_rx_next, async_eth_order[4];
static unsigned async_eth_sent;
static void async_mock_cp(const uint8_t *tx, uint8_t *rx, uint16_t n)
{
    ++async_calls;
    if((tx[0]&15U)==ESP_HOSTED_STA_IF_TYPE || (tx[0]&15U)==ESP_HOSTED_AP_IF_TYPE) {
        if(async_eth_sent<4U)async_eth_order[async_eth_sent++]=tx[0]&15U;
    }
    if(async_rx_next && (tx[0]&15U)==ESP_HOSTED_DUMMY_IF_TYPE) {
        if(async_rx_next==1U) {
            const uint8_t heartbeat[]={1,0,0,2,7,0,8,3,16,0x82,6,0x92,0x30,0};
            /* Message body includes omitted default sequence. */
            uint8_t rpc[16],wire[32];size_t pn=0,wn=0;
            (void)heartbeat;
            pn+=test_put_num(rpc+pn,1,3);pn+=test_put_num(rpc+pn,2,770);
            pn+=test_put_bytes(rpc+pn,770,NULL,0);
            wire[wn++]=1;wire[wn++]=0;wire[wn++]=0;wire[wn++]=2;wire[wn++]=(uint8_t)pn;wire[wn++]=0;
            memcpy(wire+wn,rpc,pn);wn+=pn;
            (void)esp_hosted_encode_frame(mock_host,3,0,2,wire,wn,rx,n);
        } else {
            uint8_t ethernet[14]={9};
            (void)esp_hosted_encode_frame(mock_host,ESP_HOSTED_STA_IF_TYPE,0,2,ethernet,sizeof(ethernet),rx,n);
        }
        async_rx_next=0;return;
    }
    if (async_release && (tx[0]&15U)==ESP_HOSTED_DUMMY_IF_TYPE) {
        memcpy(rx,async_held,n);async_release=0U;return;
    }
    mock_cp(tx,rx,n);
    if(async_hold && (tx[0]&15U)==3U) {memcpy(async_held,rx,n);memset(rx,0,n);}
}
static int async_wait(esp_hosted_handle_t h,esp_hosted_async_token_t t,esp_hosted_async_result_t *r)
{
    for(unsigned i=0;i<1000;++i) {
        esp_hosted_async_status_t status;
        if(esp_hosted_async_get_status(h,t,&status)!=STM_OK)return STM_ERR_INVALID_STATE;
        if(status.state==ESP_HOSTED_ASYNC_DONE)return esp_hosted_async_take_result(h,t,r);
        uint32_t before=h->info.transfer_count;
        if(esp_hosted_poll(h)!=STM_OK || h->info.transfer_count-before>1U)return STM_ERR_VERIFY;
    }
    return STM_ERR_TIMEOUT;
}
static int test_async_tasks(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32))),rx[1600] __attribute__((aligned(32)));
    esp_hosted_handle_t h=make_handle(tx,rx,ESP_HOSTED_DUMMY_IF_TYPE);
    TEST_ASSERT(h);mock_host=h;mock_stage=0;mock_requests=mock_error_mode=query_fault=radio_fault=advanced_fault=runtime_fault=0;
    mock_connect_event=mock_disconnect_event=0;mock_mode=EH_WIFI_MODE_APSTA;mock_ps=1;
    test_hal_set_spi_status(HAL_OK);test_hal_set_signals(GPIO_PIN_SET);test_hal_set_frame_callback(async_mock_cp);
    TEST_ASSERT(esp_hosted_start(h,1000)==STM_OK);
    eh_wifi_async_request_t q={.op=EH_WIFI_ASYNC_INIT};esp_hosted_async_token_t t=99,other;
    esp_hosted_async_result_t r,old;memset(&r,0xA5,sizeof(r));old=r;
    unsigned count=async_calls;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK && t==1 && async_calls==count);
    TEST_ASSERT(esp_hosted_async_take_result(h,t,&r)==STM_ERR_INVALID_STATE && !memcmp(&r,&old,sizeof(r)));
    TEST_ASSERT(eh_wifi_init(h,100)==STM_ERR_INVALID_STATE && esp_hosted_recover_begin(h,100)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(eh_wifi_async_begin(h,&q,100,&other)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(async_wait(h,t,&r)==STM_OK && h->wifi_initialized);
    TEST_ASSERT(esp_hosted_async_take_result(h,t,&r)==STM_ERR_INVALID_STATE);
    q.op=EH_WIFI_ASYNC_SET_MODE;q.value.mode=EH_WIFI_MODE_APSTA;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK && async_wait(h,t,&r)==STM_OK && h->wifi_mode==3);
    q.op=EH_WIFI_ASYNC_SET_CONFIG;q.iface=EH_WIFI_IF_STA;memset(&q.value,0,sizeof(q.value));
    strcpy(q.value.config.sta.ssid,"test-sta");strcpy(q.value.config.sta.password,"private-password");
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK);
    TEST_ASSERT(!h->async_request.value.config.sta.password[0]);
    memset(&q.value,0,sizeof(q.value));
    TEST_ASSERT(async_wait(h,t,&r)==STM_OK);
    for(unsigned i=0;i<sizeof(h->request_frame);++i)TEST_ASSERT(h->request_frame[i]==0);
    q.op=EH_WIFI_ASYNC_START;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK && async_wait(h,t,&r)==STM_OK && h->wifi_started);
    h->connected=h->ap_up=1;
    const eh_wifi_async_op_t queries[]={EH_WIFI_ASYNC_GET_MODE,EH_WIFI_ASYNC_GET_MAC,EH_WIFI_ASYNC_GET_CONFIG,
        EH_WIFI_ASYNC_GET_PROTOCOL,EH_WIFI_ASYNC_GET_BANDWIDTH,EH_WIFI_ASYNC_GET_COUNTRY_CODE,
        EH_WIFI_ASYNC_GET_COUNTRY,EH_WIFI_ASYNC_GET_PS,EH_WIFI_ASYNC_GET_MAX_TX_POWER,
        EH_WIFI_ASYNC_GET_RSSI,EH_WIFI_ASYNC_GET_CHANNEL,EH_WIFI_ASYNC_GET_AP_INFO};
    for(unsigned i=0;i<sizeof(queries)/sizeof(*queries);++i){
        q.op=queries[i];q.iface=EH_WIFI_IF_STA;
        TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK);
        TEST_ASSERT(async_wait(h,t,&r)==STM_OK);
    }
    q.op=EH_WIFI_ASYNC_GET_CONFIG;query_fault=1;
    memset(&r,0xA5,sizeof(r));old=r;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK && async_wait(h,t,&r)==STM_ERR_IO && !memcmp(&r,&old,sizeof(r)));
    query_fault=0;q.op=EH_WIFI_ASYNC_GET_RSSI;runtime_fault=4;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK && async_wait(h,t,&r)==STM_ERR_VERIFY && !memcmp(&r,&old,sizeof(r)));
    runtime_fault=0;async_hold=1;q.op=EH_WIFI_ASYNC_GET_COUNTRY;
    test_hal_set_tick(UINT32_MAX-15);
    TEST_ASSERT(eh_wifi_async_begin(h,&q,50,&t)==STM_OK);
    TEST_ASSERT(async_wait(h,t,&r)==STM_ERR_TIMEOUT && !memcmp(&r,&old,sizeof(r)));
    TEST_ASSERT(h->diagnostics.rpc_timeouts>0);
    test_hal_set_tick(100);TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->request_sent);
    TEST_ASSERT(esp_hosted_async_cancel(h,t)==STM_OK && esp_hosted_async_cancel(h,t)==STM_OK);
    TEST_ASSERT(esp_hosted_async_take_result(h,t,&r)==STM_ERR_CANCELLED && !memcmp(&r,&old,sizeof(r)));
    async_hold=0;async_release=1;unsigned late=h->diagnostics.late_responses;
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->diagnostics.late_responses==late+1);
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK);
    while(!h->async_done)TEST_ASSERT(esp_hosted_poll(h)==STM_OK);
    TEST_ASSERT(eh_wifi_get_country(h,&r.country,100)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(esp_hosted_reset(h,10,100)==STM_OK);
    TEST_ASSERT(esp_hosted_async_take_result(h,t,&r)==STM_ERR_CANCELLED && !memcmp(&r,&old,sizeof(r)));
    mock_stage=0;TEST_ASSERT(esp_hosted_start(h,1000)==STM_OK);
    h->wifi_initialized=1;h->wifi_mode=3;h->wifi_started=1;
    h->async_counter=UINT32_MAX;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_ERR_OUT_OF_RANGE);
    h->async_counter=10;h->uid=UINT32_MAX;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_ERR_OUT_OF_RANGE);
    h->uid=100;h->callback_depth=1;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_ERR_INVALID_CONTEXT);
    h->callback_depth=0;
    esp_hosted_monitor_config_t monitor={1,10,35000};
    TEST_ASSERT(esp_hosted_monitor_begin(h,&monitor,500,&t)==STM_OK && async_wait(h,t,&r)==STM_OK);
    TEST_ASSERT(h->monitor.enabled);
    /* While an RPC response is held, queued TX alternates with RX; heartbeat
     * and Ethernet callbacks keep running. The encoded RPC remains isolated. */
    uint8_t frame[14]={0};h->connected=h->ap_up=1;
    TEST_ASSERT(esp_hosted_set_callbacks(h,receive_cb,NULL,NULL)==STM_OK);
    q.op=EH_WIFI_ASYNC_GET_COUNTRY;async_hold=1;async_eth_sent=0;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK);
    TEST_ASSERT(esp_hosted_send_enqueue(h,frame,sizeof(frame),500)==STM_OK);
    TEST_ASSERT(eh_wifi_ap_send_enqueue(h,frame,sizeof(frame),500)==STM_OK);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->request_sent && h->queued_count==2);
    h->io_last_tx=0;uint32_t hb_before=h->heartbeat_tick;async_rx_next=1;
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->queued_count==1 && async_rx_next==1);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->queued_count==1 && !async_rx_next && h->heartbeat_tick>hb_before);
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->queued_count);
    async_rx_next=2;received_length=0;
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && received_length==14U);
    TEST_ASSERT(async_eth_sent==2 && async_eth_order[0]==ESP_HOSTED_STA_IF_TYPE && async_eth_order[1]==ESP_HOSTED_AP_IF_TYPE);
    async_hold=0;async_release=1;
    TEST_ASSERT(async_wait(h,t,&r)==STM_OK);
    /* A composite INIT shares one deadline, including the second request. */
    h->monitor.enabled=0;h->wifi_initialized=0;q.op=EH_WIFI_ASYNC_INIT;
    test_hal_set_tick(1000);
    TEST_ASSERT(eh_wifi_async_begin(h,&q,100,&t)==STM_OK);
    uint32_t deadline_start=h->async_tick;
    TEST_ASSERT(esp_hosted_poll(h)==STM_OK && h->async_stage==1U);
    TEST_ASSERT(h->request_tick==deadline_start && h->request_timeout==100U);
    test_hal_set_tick(deadline_start+100U);
    TEST_ASSERT(async_wait(h,t,&r)==STM_ERR_TIMEOUT && !h->wifi_initialized);
    h->wifi_initialized=1;q.op=EH_WIFI_ASYNC_GET_MODE;
    unsigned before_cancel=async_calls;
    TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK);
    TEST_ASSERT(esp_hosted_async_cancel(h,t)==STM_OK);
    TEST_ASSERT(esp_hosted_async_take_result(h,t,&r)==STM_ERR_CANCELLED && async_calls==before_cancel);
    h->wifi_initialized=1;
    const eh_wifi_async_op_t setters[]={EH_WIFI_ASYNC_SET_PROTOCOL,EH_WIFI_ASYNC_SET_BANDWIDTH,
        EH_WIFI_ASYNC_SET_COUNTRY_CODE,EH_WIFI_ASYNC_SET_PS,EH_WIFI_ASYNC_SET_MAX_TX_POWER,
        EH_WIFI_ASYNC_DISCONNECT,EH_WIFI_ASYNC_CONNECT,EH_WIFI_ASYNC_STOP,EH_WIFI_ASYNC_START};
    for(unsigned i=0;i<sizeof(setters)/sizeof(*setters);++i) {
        memset(&q,0,sizeof(q));q.op=setters[i];q.iface=EH_WIFI_IF_STA;
        if(q.op==EH_WIFI_ASYNC_SET_PROTOCOL)q.value.protocol=7;
        if(q.op==EH_WIFI_ASYNC_SET_BANDWIDTH)q.value.bandwidth=EH_WIFI_BW_HT20;
        if(q.op==EH_WIFI_ASYNC_SET_COUNTRY_CODE)memcpy(q.value.country.code,"CN",3);
        if(q.op==EH_WIFI_ASYNC_SET_PS)q.value.ps=EH_WIFI_PS_NONE;
        if(q.op==EH_WIFI_ASYNC_SET_MAX_TX_POWER)q.value.power=44;
        TEST_ASSERT(eh_wifi_async_begin(h,&q,500,&t)==STM_OK && async_wait(h,t,&r)==STM_OK);
    }
    monitor.interval_s=9;
    TEST_ASSERT(esp_hosted_monitor_begin(h,&monitor,500,&t)==STM_ERR_INVALID_ARG);
    test_hal_set_frame_callback(NULL);TEST_ASSERT(esp_hosted_delete(&h)==STM_OK);return 0;
}
static int test_tx_queue_bounds(void)
{
    static uint8_t tx[1600] __attribute__((aligned(32))),rx[1600] __attribute__((aligned(32)));
    uint8_t a[14]={1},b[14]={2};
    esp_hosted_handle_t h=make_handle(tx,rx,ESP_HOSTED_DUMMY_IF_TYPE);TEST_ASSERT(h);
    h->initialized=h->connected=h->ap_up=1;h->diagnostics.state=ESP_HOSTED_STATE_READY;
    test_hal_set_frame_callback(NULL);test_hal_set_signals(GPIO_PIN_RESET);test_hal_set_tick(UINT32_MAX-10);
    TEST_ASSERT(esp_hosted_send_enqueue(h,a,sizeof(a),100)==STM_OK);
    TEST_ASSERT(eh_wifi_ap_send_enqueue(h,b,sizeof(b),100)==STM_OK);
    TEST_ASSERT(esp_hosted_send_enqueue(h,a,sizeof(a),100)==STM_ERR_NO_MEM);
    memset(a,0,sizeof(a));TEST_ASSERT(h->queued_frame[h->queued_head][0]==1);
    uint32_t transfers=h->info.transfer_count;TEST_ASSERT(esp_hosted_poll(h)==STM_OK && transfers==h->info.transfer_count);
    test_hal_set_tick(150);TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->queued_count && h->diagnostics.tx_expired==2);
    test_hal_set_signals(GPIO_PIN_SET);h->config.poll_transfer_timeout_ms=10;
    TEST_ASSERT(esp_hosted_send_enqueue(h,a,sizeof(a),100)==STM_OK);
    test_hal_set_spi_status(HAL_TIMEOUT);
    TEST_ASSERT(esp_hosted_poll(h)==STM_ERR_TIMEOUT && !h->queued_count && h->diagnostics.tx_failures==1);
    TEST_ASSERT(test_hal_last_spi_timeout()<=10U);
    test_hal_set_spi_status(HAL_OK);
    TEST_ASSERT(esp_hosted_send_enqueue(h,a,sizeof(a),100)==STM_OK);
    h->connected=0;TEST_ASSERT(esp_hosted_poll(h)==STM_OK && !h->queued_count && h->diagnostics.tx_cleared==1);
    TEST_ASSERT(esp_hosted_send_enqueue(h,a,sizeof(a),100)==STM_ERR_INVALID_STATE);
    TEST_ASSERT(esp_hosted_send_enqueue(h,a,13,100)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(eh_wifi_ap_send_enqueue(h,b,sizeof(b),0)==STM_ERR_INVALID_ARG);
    TEST_ASSERT(esp_hosted_reset(h,10,100)==STM_OK && esp_hosted_delete(&h)==STM_OK);return 0;
}

int main(void)
{
    TEST_ASSERT(test_dummy_transfer() == 0);
    TEST_ASSERT(test_normal_frame() == 0);
    TEST_ASSERT(test_encode_frame() == 0);
    TEST_ASSERT(test_init_caps() == 0);
    TEST_ASSERT(test_invalid_frames() == 0);
    TEST_ASSERT(test_legacy_dummy() == 0);
    TEST_ASSERT(test_transfer_errors() == 0);
    TEST_ASSERT(test_control_and_sta() == 0);
    TEST_ASSERT(test_init_timeout() == 0);
    TEST_ASSERT(test_successful_rpc_sequence() == 0);
    TEST_ASSERT(test_reconnect_policy() == 0);
    TEST_ASSERT(test_runtime_queries() == 0);
    TEST_ASSERT(test_radio_config() == 0);
    TEST_ASSERT(test_advanced_config() == 0);
    TEST_ASSERT(test_monitor_recovery() == 0);
    TEST_ASSERT(test_receive_queue() == 0);
    TEST_ASSERT(test_async_tasks()==0);
    TEST_ASSERT(test_tx_queue_bounds()==0);
    puts("stm_esp_hosted tests: PASS");
    return 0;
}

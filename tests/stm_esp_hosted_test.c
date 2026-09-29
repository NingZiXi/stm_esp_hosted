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
static unsigned mock_stage, mock_requests, mock_connect_event, mock_error_mode;
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
    uint8_t body[96] = {0}, payload[128] = {0}, wire[160] = {0};
    size_t b = 0U, n = 0U, w = 0U;
    ++mock_requests;
    if (id == 350U) {
        b += test_put_num(body + b, 2U, 3U);
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
    else if (id == 259U) { mock_seen |= 8U; }
    else if (id == 260U) {
        mock_seen |= 16U;
        if (mock_error_mode) { b += test_put_num(body + b, 1U, 1U); }
    } else if (id == 280U) { mock_seen |= 32U; }
    else if (id == 284U) {
        mock_seen |= 64U;
        if (mock_error_mode) { b += test_put_num(body + b, 1U, 1U); }
    }
    else if (id == 282U) { mock_seen |= 128U; mock_connect_event = 1U; }
    else if (id == 283U) { mock_seen |= 256U; }
    n += test_put_num(payload + n, 1U, 2U);
    n += test_put_num(payload + n, 2U, id + 256U);
    n += test_put_num(payload + n, 3U, uid);
    n += test_put_bytes(payload + n, id + 256U, body, b);
    wire[w++] = 1U; wire[w++] = sizeof(ep) - 1U; wire[w++] = 0U;
    memcpy(wire + w, ep, sizeof(ep) - 1U); w += sizeof(ep) - 1U;
    wire[w++] = 2U; wire[w++] = (uint8_t)n; wire[w++] = 0U;
    memcpy(wire + w, payload, n); w += n;
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
    mock_host = h; mock_stage = mock_requests = mock_connect_event = mock_error_mode = 0U;
    mock_seen = scan_event_pending = scan_config_seen = scan_rejected = 0U;
    scan_result_status = ap_info_bad = 0U;
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
    TEST_ASSERT(eh_wifi_get_mac(h, EH_WIFI_IF_STA, mac) == STM_OK);
    TEST_ASSERT(mac[0] == 2U && mac[5] == 0x55U);
    eh_wifi_config_t config = {0};
    strcpy(config.sta.ssid, "test-network");
    strcpy(config.sta.password, "test-password");
    mock_error_mode = 1U;
    TEST_ASSERT(eh_wifi_set_config(h, EH_WIFI_IF_STA, &config, 500U) == STM_ERR_IO);
    mock_error_mode = 0U;
    TEST_ASSERT(eh_wifi_set_config(h, EH_WIFI_IF_STA, &config, 500U) == STM_OK);
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
    TEST_ASSERT(eh_wifi_start(h, 500U) == STM_OK);
    TEST_ASSERT(mock_seen == 511U && mock_requests >= 15U);
    test_hal_set_frame_callback(NULL);
    TEST_ASSERT(esp_hosted_delete(&h) == STM_OK);
    return 0;
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
    puts("stm_esp_hosted tests: PASS");
    return 0;
}

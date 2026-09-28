/** ESP-Hosted 3.0.9 RPC v2 control and STA data path. */
#include "stm_esp_hosted.h"
#include <string.h>

/* Minimal, bounded protobuf wire codec for the specific RPC messages below. */
#define RPC_WIRE_CAP 320U
#define RPC_BODY_CAP 256U
#define RPC_IF 3U
#define PRIV_IF 5U
#define WIFI_CONNECTED_EVENT 775U
#define WIFI_DISCONNECTED_EVENT 776U

/* Internal layout is shared with the transport implementation within this component. */
#include "stm_esp_hosted_private.h"

static size_t put_varint(uint8_t *out, uint64_t value)
{
    size_t n = 0;
    do { out[n] = (uint8_t)((value & 127U) | (value > 127U ? 128U : 0U));
         value >>= 7U; ++n; } while (value != 0U);
    return n;
}
static size_t put_num(uint8_t *out, uint32_t field, uint64_t value)
{
    size_t n = put_varint(out, (uint64_t)field << 3U);
    return n + put_varint(out + n, value);
}
static size_t put_bytes(uint8_t *out, uint32_t field, const uint8_t *bytes, size_t len)
{
    size_t n = put_varint(out, ((uint64_t)field << 3U) | 2U);
    n += put_varint(out + n, len);
    if (len) { memcpy(out + n, bytes, len); }
    return n + len;
}
static int read_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 10U && *p < end; ++i) {
        uint8_t b = *(*p)++;
        if (i == 9U && b > 1U) { return 0; }
        value |= (uint64_t)(b & 127U) << (i * 7U);
        if (!(b & 128U)) { *v = value; return 1; }
    }
    return 0;
}
/* Reject unknown wire types and malformed/truncated fields, including fields after the match. */
static int field(const uint8_t *data, size_t len, uint32_t wanted, uint8_t wire,
                 const uint8_t **bytes, size_t *size, uint64_t *number)
{
    const uint8_t *p = data, *end = data + len;
    int found = 0;
    while (p < end) {
        uint64_t key, n;
        if (!read_varint(&p, end, &key) || !(key >> 3U) || !read_varint(&p, end, &n)) { return -1; }
        if ((key & 7U) == 0U) {
            if ((key >> 3U) == wanted && wire == 0U) { *number = n; found = 1; }
        } else if ((key & 7U) == 2U) {
            if (n > (uint64_t)(end - p)) { return -1; }
            if ((key >> 3U) == wanted && wire == 2U) {
                *bytes = p; *size = (size_t)n; found = 1;
            }
            p += (size_t)n;
        } else { return -1; }
    }
    return found;
}

static void link_change(struct esp_hosted_context *ctx, uint8_t connected)
{
    if (ctx->connected != connected) {
        ctx->connected = connected;
        if (ctx->link) { ctx->link(ctx->user, connected); }
    }
}
static stm_err_t consume(struct esp_hosted_context *ctx)
{
    esp_hosted_frame_t frame = {0};
    esp_hosted_frame_result_t r = esp_hosted_decode_frame(ctx, ctx->config.rx_buffer,
                                                           ESP_HOSTED_FRAME_SIZE, &frame);
    /* Some CP SPI transactions return an idle zero header instead of a V1 dummy. */
    if (r == ESP_HOSTED_FRAME_DUMMY ||
        memcmp(ctx->config.rx_buffer, (const uint8_t[ESP_HOSTED_FRAME_HEADER_SIZE]){0},
               ESP_HOSTED_FRAME_HEADER_SIZE) == 0) { return STM_OK; }
    if (r != ESP_HOSTED_FRAME_OK) { return STM_ERR_VERIFY; }
    if (frame.if_type == ESP_HOSTED_STA_IF_TYPE) {
        if (frame.payload_length >= 14U && frame.payload_length <= ESP_HOSTED_STA_MTU + 14U &&
            ctx->receive) { ctx->receive(ctx->user, frame.payload, frame.payload_length); }
        return STM_OK;
    }
    if (frame.if_type == PRIV_IF && frame.payload_length >= 2U && frame.payload[0] == 0x22U) {
        uint8_t caps[20]; size_t n;
        if (ctx->negotiated) { return STM_OK; }
        stm_err_t result = esp_hosted_build_host_caps(frame.payload, frame.payload_length,
                                                       caps, sizeof(caps), &n);
        if (result != STM_OK || n != 20U) { return STM_ERR_NOT_SUPPORTED; }
        result = esp_hosted_encode_frame(ctx, PRIV_IF, 0x33U, 0U, caps, n,
                                         ctx->config.tx_buffer, ESP_HOSTED_FRAME_SIZE);
        if (result != STM_OK) { return result; }
        ctx->negotiated = 1U;
        /* Caller must send INIT reply before any subsequent request. */
        return STM_OK;
    }
    if (frame.if_type != RPC_IF || frame.payload_length < 9U) { return STM_OK; }
    const uint8_t *p = frame.payload;
    size_t len = frame.payload_length;
    if (p[0] != 1U) { return STM_ERR_VERIFY; }
    size_t ep_len = (size_t)p[1] | ((size_t)p[2] << 8U);
    size_t offset = 3U + ep_len;
    if (offset + 3U > len || p[offset] != 2U) { return STM_ERR_VERIFY; }
    size_t rpc_len = (size_t)p[offset + 1U] | ((size_t)p[offset + 2U] << 8U);
    offset += 3U;
    if (rpc_len > len - offset) { return STM_ERR_VERIFY; }
    p += offset;
    uint64_t type = 0, id = 0, uid = 0;
    const uint8_t *body = NULL; size_t body_len = 0;
    if (field(p, rpc_len, 1U, 0U, &body, &body_len, &type) != 1 ||
        field(p, rpc_len, 2U, 0U, &body, &body_len, &id) != 1) { return STM_ERR_VERIFY; }
    if (type == 3U) {
        if (id == WIFI_CONNECTED_EVENT) { link_change(ctx, 1U); }
        if (id == WIFI_DISCONNECTED_EVENT) { link_change(ctx, 0U); }
        return STM_OK;
    }
    if (type != 2U || id > UINT16_MAX ||
        field(p, rpc_len, 3U, 0U, &body, &body_len, &uid) != 1 ||
        field(p, rpc_len, (uint32_t)id, 2U, &body, &body_len, &uid) != 1 ||
        body_len > sizeof(ctx->response_data)) { return STM_ERR_VERIFY; }
    /* Ignore delayed responses from an older transaction. */
    if (id != ctx->response_id || uid != ctx->response_uid) { return STM_OK; }
    if (ctx->response_ready) { return STM_OK; }
    memcpy(ctx->response_data, body, body_len);
    ctx->response_length = body_len;
    ctx->response_ready = 1U;
    return STM_OK;
}
static stm_err_t exchange(struct esp_hosted_context *ctx, const uint8_t *tx)
{
    stm_err_t err = esp_hosted_transfer(ctx, tx, NULL);
    return err == STM_OK ? consume(ctx) : err;
}
stm_err_t esp_hosted_poll(esp_hosted_handle_t handle)
{
    esp_hosted_signals_t pins;
    if (!handle) { return STM_ERR_INVALID_ARG; }
    stm_err_t err = esp_hosted_get_signals(handle, &pins);
    if (err != STM_OK) { return err; }
    if (pins.handshake != GPIO_PIN_SET || pins.data_ready != GPIO_PIN_SET) { return STM_OK; }
    return exchange(handle, NULL);
}
stm_err_t esp_hosted_set_callbacks(esp_hosted_handle_t handle, esp_hosted_rx_fn rx,
                                   esp_hosted_link_fn link, void *user)
{
    if (!handle) { return STM_ERR_INVALID_ARG; }
    handle->receive = rx; handle->link = link; handle->user = user;
    return STM_OK;
}
static stm_err_t request(struct esp_hosted_context *ctx, uint16_t id,
                         const uint8_t *body, size_t body_len,
                         uint8_t *out, size_t *out_len, uint32_t timeout_ms)
{
    uint8_t rpc[RPC_BODY_CAP], wire[RPC_WIRE_CAP];
    static const uint8_t ep[] = "RPCRsp";
    size_t n = 0, w = 0;
    if (!ctx->negotiated || body_len > 192U || (!body && body_len)) { return STM_ERR_INVALID_STATE; }
    ctx->response_id = id + 256U;
    ctx->info.last_rpc_id = id;
    ctx->info.last_rpc_status = 0U;
    ctx->info.last_rpc_status_present = 0U;
    ctx->response_uid = ++ctx->uid;
    ctx->response_length = 0U; ctx->response_ready = 0U;
    n += put_num(rpc + n, 1U, 1U);
    n += put_num(rpc + n, 2U, id);
    n += put_num(rpc + n, 3U, ctx->response_uid);
    n += put_bytes(rpc + n, id, body, body_len);
    wire[w++] = 1U; wire[w++] = sizeof(ep) - 1U; wire[w++] = 0U;
    memcpy(wire + w, ep, sizeof(ep) - 1U); w += sizeof(ep) - 1U;
    wire[w++] = 2U; wire[w++] = (uint8_t)n; wire[w++] = (uint8_t)(n >> 8U);
    memcpy(wire + w, rpc, n); w += n;
    stm_err_t err = esp_hosted_encode_frame(ctx, RPC_IF, 0U, ++ctx->sequence, wire, w,
                                             ctx->config.tx_buffer, ESP_HOSTED_FRAME_SIZE);
    memset(wire, 0, sizeof(wire)); memset(rpc, 0, sizeof(rpc));
    if (err == STM_OK) { err = esp_hosted_wait_handshake(ctx, timeout_ms); }
    if (err == STM_OK) { err = exchange(ctx, ctx->config.tx_buffer); }
    uint32_t start = HAL_GetTick();
    while (err == STM_OK && !ctx->response_ready && HAL_GetTick() - start < timeout_ms) {
        err = esp_hosted_poll(ctx);
    }
    if (err == STM_OK && !ctx->response_ready) { err = STM_ERR_TIMEOUT; }
    if (err == STM_OK) {
        const uint8_t *unused = NULL; size_t size = 0; uint64_t status = 0;
        uint32_t status_field = (id == 259U || id == 257U) ? 2U : 1U;
        int has = field(ctx->response_data, ctx->response_length, status_field, 0U,
                        &unused, &size, &status);
        if (has == 1) {
            ctx->info.last_rpc_status_present = 1U;
            ctx->info.last_rpc_status = (uint32_t)status;
        }
        if (has < 0 || (has == 1 && status != 0U)) { err = STM_ERR_IO; }
        if (err == STM_OK && out && out_len) {
            if (*out_len < ctx->response_length) { err = STM_ERR_OUT_OF_RANGE; }
            else { memcpy(out, ctx->response_data, ctx->response_length);
                   *out_len = ctx->response_length; }
        }
    }
    ctx->response_length = 0U; ctx->response_ready = 0U;
    return err;
}
stm_err_t esp_hosted_start(esp_hosted_handle_t handle, uint32_t timeout_ms)
{
    if (!handle || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    handle->negotiated = 0U; handle->initialized = 0U;
    handle->wifi_initialized = 0U; link_change(handle, 0U);
    stm_err_t err = esp_hosted_reset(handle, 10U, 100U);
    if (err == STM_OK) { err = esp_hosted_wait_handshake(handle, timeout_ms); }
    uint32_t start = HAL_GetTick();
    while (err == STM_OK && !handle->negotiated && HAL_GetTick() - start < timeout_ms) {
        err = esp_hosted_poll(handle);
    }
    if (err == STM_OK && !handle->negotiated) { err = STM_ERR_TIMEOUT; }
    /* consume() encodes the capability reply in tx_buffer. */
    if (err == STM_OK) { err = esp_hosted_wait_handshake(handle, timeout_ms); }
    if (err == STM_OK) { err = exchange(handle, handle->config.tx_buffer); }
    if (err != STM_OK) { return err; }
    uint8_t version[64]; size_t len = sizeof(version);
    err = request(handle, 350U, NULL, 0U, version, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    uint64_t major = 0, minor = 0, patch = 0;
    const uint8_t *unused = NULL; size_t unused_len = 0;
    if (field(version, len, 2U, 0U, &unused, &unused_len, &major) != 1 ||
        field(version, len, 3U, 0U, &unused, &unused_len, &minor) < 0 ||
        field(version, len, 4U, 0U, &unused, &unused_len, &patch) < 0 ||
        major != 3U || minor != 0U || patch != 9U) { return STM_ERR_NOT_SUPPORTED; }
    handle->version = (esp_hosted_version_t){(uint8_t)major, (uint8_t)minor, (uint8_t)patch};
    handle->initialized = 1U;
    return STM_OK;
}
stm_err_t esp_hosted_get_version(esp_hosted_handle_t h, esp_hosted_version_t *v)
{
    if (!h || !v) { return STM_ERR_INVALID_ARG; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    *v = h->version; return STM_OK;
}
static stm_err_t wifi_setup(struct esp_hosted_context *h, uint32_t timeout_ms)
{
    if (h->wifi_initialized) { return STM_OK; }
    uint8_t cfg[96], body[112]; size_t n = 0;
    n += put_num(cfg + n, 1U, 10U); n += put_num(cfg + n, 2U, 32U);
    n += put_num(cfg + n, 3U, 1U); n += put_num(cfg + n, 5U, 32U);
    n += put_num(cfg + n, 8U, 1U); n += put_num(cfg + n, 9U, 1U);
    n += put_num(cfg + n, 11U, 1U); n += put_num(cfg + n, 13U, 6U);
    n += put_num(cfg + n, 15U, 752U); n += put_num(cfg + n, 16U, 32U);
    n += put_num(cfg + n, 20U, 0x1F2F3F4FU);
    size_t len = put_bytes(body, 1U, cfg, n);
    stm_err_t err = request(h, 278U, body, len, NULL, NULL, timeout_ms);
    if (err == STM_OK) { err = request(h, 259U, NULL, 0U, NULL, NULL, timeout_ms); }
    n = put_num(body, 1U, 1U); /* WIFI_MODE_STA */
    if (err == STM_OK) { err = request(h, 260U, body, n, NULL, NULL, timeout_ms); }
    if (err == STM_OK) { err = request(h, 280U, NULL, 0U, NULL, NULL, timeout_ms); }
    memset(cfg, 0, sizeof(cfg)); memset(body, 0, sizeof(body));
    if (err == STM_OK) { h->wifi_initialized = 1U; }
    return err;
}
stm_err_t esp_hosted_get_sta_mac(esp_hosted_handle_t h, uint8_t mac[6])
{
    if (!h || !mac) { return STM_ERR_INVALID_ARG; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = wifi_setup(h, 5000U);
    if (err != STM_OK) { return err; }
    uint8_t body[4], response[32]; size_t len = sizeof(response);
    size_t n = put_num(body, 1U, 0U); /* WIFI_IF_STA */
    err = request(h, 257U, body, n, response, &len, 5000U);
    if (err != STM_OK) { return err; }
    const uint8_t *bytes = NULL; size_t count = 0; uint64_t number = 0;
    if (field(response, len, 1U, 2U, &bytes, &count, &number) != 1 || count != 6U) {
        return STM_ERR_VERIFY;
    }
    memcpy(h->mac, bytes, 6U); memcpy(mac, bytes, 6U);
    return STM_OK;
}
uint8_t esp_hosted_is_connected(esp_hosted_handle_t h) { return h ? h->connected : 0U; }
stm_err_t esp_hosted_send(esp_hosted_handle_t h, const uint8_t *frame, size_t length)
{
    if (!h || !frame || length < 14U || length > ESP_HOSTED_STA_MTU + 14U) {
        return STM_ERR_INVALID_ARG;
    }
    if (!h->initialized || !h->connected) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = esp_hosted_encode_frame(h, ESP_HOSTED_STA_IF_TYPE, 0U,
                                             ++h->sequence, frame, length,
                                             h->config.tx_buffer, ESP_HOSTED_FRAME_SIZE);
    if (err == STM_OK) { err = esp_hosted_wait_handshake(h, 1000U); }
    return err == STM_OK ? exchange(h, h->config.tx_buffer) : err;
}
stm_err_t esp_hosted_connect(esp_hosted_handle_t h, const char *ssid,
                             const char *password, uint32_t timeout_ms)
{
    if (!h || !ssid || !password || !timeout_ms || !*ssid || strlen(ssid) > 32U ||
        strlen(password) > 64U) { return STM_ERR_INVALID_ARG; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = wifi_setup(h, timeout_ms);
    if (err != STM_OK) { return err; }
    uint8_t station[112], wifi_cfg[128], body[144]; size_t n = 0;
    n += put_bytes(station + n, 1U, (const uint8_t *)ssid, strlen(ssid));
    n += put_bytes(station + n, 2U, (const uint8_t *)password, strlen(password));
    size_t wifi_len = put_bytes(wifi_cfg, 2U, station, n);
    size_t body_len = put_bytes(body, 2U, wifi_cfg, wifi_len);
    err = request(h, 284U, body, body_len, NULL, NULL, timeout_ms);
    memset(station, 0, sizeof(station)); memset(wifi_cfg, 0, sizeof(wifi_cfg));
    memset(body, 0, sizeof(body));
    if (err == STM_OK) { err = request(h, 282U, NULL, 0U, NULL, NULL, timeout_ms); }
    uint32_t start = HAL_GetTick();
    while (err == STM_OK && !h->connected && HAL_GetTick() - start < timeout_ms) {
        err = esp_hosted_poll(h);
    }
    if (err == STM_OK && !h->connected) { err = STM_ERR_TIMEOUT; }
    return err;
}
stm_err_t esp_hosted_disconnect(esp_hosted_handle_t h)
{
    if (!h) { return STM_ERR_INVALID_ARG; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = request(h, 283U, NULL, 0U, NULL, NULL, 5000U);
    if (err == STM_OK) { link_change(h, 0U); }
    return err;
}

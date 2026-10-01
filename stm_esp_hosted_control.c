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
#define WIFI_SCAN_DONE_EVENT 774U
#define WIFI_AP_CLIENT_CONNECTED_EVENT 771U
#define WIFI_AP_CLIENT_DISCONNECTED_EVENT 772U
#define WIFI_NO_ARGS_EVENT 773U

/* Internal layout is shared with the transport implementation within this component. */
#include "stm_esp_hosted_private.h"

/* A volatile write prevents credential-bearing RPC buffers from being optimized away. */
static void clear_sensitive(void *data, size_t length)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (length--) { *p++ = 0U; }
}

void esp_hosted_count(uint32_t *counter)
{
    if (*counter != UINT32_MAX) { ++*counter; }
}
void esp_hosted_record_fault(struct esp_hosted_context *ctx, esp_hosted_fault_t reason,
                             stm_err_t error)
{
    ctx->diagnostics.last_fault = reason;
    ctx->diagnostics.last_fault_tick = HAL_GetTick();
    ctx->diagnostics.last_error = error;
}

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
/* Bounded proto3 field iterator. Unknown fixed32/fixed64 fields can be skipped. */
static int next_field(const uint8_t **p, const uint8_t *end, uint32_t *tag,
                      uint8_t *wire, const uint8_t **bytes, size_t *size, uint64_t *number)
{
    uint64_t key, n;
    if (*p == end) { return 0; }
    if (!read_varint(p, end, &key) || !(key >> 3U) || key > UINT32_MAX) { return -1; }
    *tag = (uint32_t)(key >> 3U); *wire = (uint8_t)(key & 7U);
    *bytes = NULL; *size = 0U; *number = 0U;
    if (*wire == 0U) { return read_varint(p, end, number) ? 1 : -1; }
    if (*wire == 2U) {
        if (!read_varint(p, end, &n) || n > (uint64_t)(end - *p)) { return -1; }
        *size = (size_t)n;
    } else if (*wire == 1U || *wire == 5U) {
        *size = *wire == 1U ? 8U : 4U;
        if (*size > (size_t)(end - *p)) { return -1; }
    } else { return -1; }
    *bytes = *p; *p += *size;
    return 1;
}
/* Singular fields reject duplicates, wrong wire types and trailing corruption. */
static int field(const uint8_t *data, size_t len, uint32_t wanted, uint8_t wire,
                 const uint8_t **bytes, size_t *size, uint64_t *number)
{
    const uint8_t *p = data, *end = data + len;
    int found = 0;
    while (p < end) {
        uint32_t tag; uint8_t actual_wire; const uint8_t *value;
        size_t value_size; uint64_t n;
        if (next_field(&p, end, &tag, &actual_wire, &value, &value_size, &n) < 0) { return -1; }
        if (tag == wanted) {
            if (found || actual_wire != wire) { return -1; }
            if (wire == 0U) { *number = n; }
            else { *bytes = value; *size = value_size; }
            found = 1;
        }
    }
    return found;
}

static void reconnect_clear(struct esp_hosted_context *ctx)
{
    ctx->reconnect_armed = 0U;
    ctx->reconnect_waiting = 0U;
    ctx->connect_pending = 0U;
    ctx->reconnect_attempts = 0U;
    ctx->reconnect_delay = ctx->reconnect_config.initial_delay_ms;
}
static void reconnect_schedule(struct esp_hosted_context *ctx)
{
    ctx->connect_pending = 0U;
    if (!ctx->reconnect_config.enabled || !ctx->reconnect_armed ||
        (ctx->reconnect_config.max_attempts &&
         ctx->reconnect_attempts >= ctx->reconnect_config.max_attempts)) {
        ctx->reconnect_waiting = 0U;
        return;
    }
    ctx->reconnect_waiting = 1U;
    ctx->reconnect_since = HAL_GetTick();
    if (!ctx->reconnect_delay) { ctx->reconnect_delay = ctx->reconnect_config.initial_delay_ms; }
}
static void link_change(struct esp_hosted_context *ctx, uint8_t connected)
{
    if (ctx->connected != connected) {
        ctx->connected = connected;
        if (connected) {
            ctx->reconnect_waiting = ctx->connect_pending = 0U;
            ctx->reconnect_attempts = 0U;
            ctx->reconnect_delay = ctx->reconnect_config.initial_delay_ms;
        } else if (ctx->reconnect_armed) { reconnect_schedule(ctx); }
        if (ctx->link) { ++ctx->callback_depth; ctx->link(ctx->user, connected); --ctx->callback_depth; }
    }
}
static void ap_link_change(struct esp_hosted_context *ctx, uint8_t up)
{
    if (ctx->ap_up != up) {
        ctx->ap_up = up;
        if (ctx->ap_link) { ++ctx->callback_depth; ctx->ap_link(ctx->ap_link_user, up); --ctx->callback_depth; }
    }
}
static void emit_wifi_event(struct esp_hosted_context *ctx, const eh_wifi_event_t *event)
{
    if (ctx->wifi_event) { ++ctx->callback_depth; ctx->wifi_event(ctx->wifi_event_user, event); --ctx->callback_depth; }
}
void esp_hosted_invalidate(struct esp_hosted_context *ctx, esp_hosted_fault_t reason,
                           stm_err_t error, esp_hosted_state_t state)
{
    ++ctx->session_epoch;
    esp_hosted_count(&ctx->diagnostics.generation);
    ctx->diagnostics.state = state;
    esp_hosted_record_fault(ctx, reason, error);
    ctx->initialized = ctx->negotiated = ctx->info.ready = 0U;
    ctx->wifi_initialized = ctx->wifi_started = ctx->wifi_mode = 0U;
    ctx->scan_pending = ctx->scan_done = 0U;
    ctx->last_disconnect_reason = 0U;
    reconnect_clear(ctx);
    link_change(ctx, 0U); ap_link_change(ctx, 0U);
    ctx->request_error = STM_ERR_CANCELLED;
    ctx->request_active = ctx->request_sent = ctx->response_ready = 0U;
    ctx->response_length = 0U;
    clear_sensitive(ctx->response_data, sizeof(ctx->response_data));
    clear_sensitive(ctx->config.tx_buffer, ESP_HOSTED_FRAME_SIZE);
    /* rx_buffer may still contain the INIT being validated by consume(). */
    memset(ctx->mac, 0, sizeof(ctx->mac));
    memset(&ctx->version, 0, sizeof(ctx->version));
    ctx->queued_count = ctx->queued_head = 0U;
    clear_sensitive(ctx->queued_frame, sizeof(ctx->queued_frame));
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
    if (r != ESP_HOSTED_FRAME_OK) {
        esp_hosted_count(r == ESP_HOSTED_FRAME_CORRUPT ? &ctx->diagnostics.checksum_failures :
                                                       &ctx->diagnostics.frame_failures);
        esp_hosted_record_fault(ctx, ESP_HOSTED_FAULT_FRAME, STM_ERR_VERIFY);
        return STM_ERR_VERIFY;
    }
    if (frame.if_type == ESP_HOSTED_STA_IF_TYPE) {
        if (ctx->diagnostics.state == ESP_HOSTED_STATE_FAULT || ctx->recovery_phase) { return STM_OK; }
        if (frame.payload_length >= 14U && frame.payload_length <= ESP_HOSTED_STA_MTU + 14U &&
            ctx->receive) { ++ctx->callback_depth; ctx->rx_callback = 1U; ctx->receive(ctx->user, frame.payload, frame.payload_length); ctx->rx_callback = 0U; --ctx->callback_depth; }
        return STM_OK;
    }
    if (frame.if_type == ESP_HOSTED_AP_IF_TYPE) {
        if (ctx->diagnostics.state == ESP_HOSTED_STATE_FAULT || ctx->recovery_phase) { return STM_OK; }
        if (frame.payload_length >= 14U && frame.payload_length <= ESP_HOSTED_STA_MTU + 14U &&
            ctx->ap_receive) { ++ctx->callback_depth; ctx->rx_callback = 1U; ctx->ap_receive(ctx->ap_user, frame.payload, frame.payload_length); ctx->rx_callback = 0U; --ctx->callback_depth; }
        return STM_OK;
    }
    if (frame.if_type == PRIV_IF && frame.payload_length >= 2U && frame.payload[0] == 0x22U) {
        uint8_t caps[20]; size_t n;
        stm_err_t result = esp_hosted_build_host_caps(frame.payload, frame.payload_length,
                                                       caps, sizeof(caps), &n);
        if (result != STM_OK || n != 20U) { return STM_ERR_NOT_SUPPORTED; }
        if (ctx->diagnostics.state == ESP_HOSTED_STATE_READY) {
            esp_hosted_invalidate(ctx, ESP_HOSTED_FAULT_UNEXPECTED_INIT,
                                  STM_ERR_CANCELLED, ESP_HOSTED_STATE_FAULT);
            return STM_OK;
        }
        if (ctx->diagnostics.state == ESP_HOSTED_STATE_FAULT || ctx->negotiated) { return STM_OK; }
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
    if (rpc_len != len - offset) { return STM_ERR_VERIFY; }
    p += offset;
    uint64_t type = 0, id = 0, uid = 0;
    const uint8_t *body = NULL; size_t body_len = 0;
    if (field(p, rpc_len, 1U, 0U, &body, &body_len, &type) != 1 ||
        field(p, rpc_len, 2U, 0U, &body, &body_len, &id) != 1 || id > UINT16_MAX) { return STM_ERR_VERIFY; }
    if (type == 3U) {
        const uint8_t *evt = NULL, *nested = NULL; size_t evt_len = 0U, nested_len = 0U;
        uint64_t val = 0U;
        eh_wifi_event_t event = {0};
        if (field(p, rpc_len, (uint32_t)id, 2U, &evt, &evt_len, &val) != 1) {
            return STM_ERR_VERIFY;
        }
        if (id == 770U) {
            if (field(evt, evt_len, 1U, 0U, &nested, &nested_len, &val) < 0 || val > INT32_MAX) {
                return STM_ERR_VERIFY;
            }
            if (ctx->monitor.enabled && ctx->diagnostics.state == ESP_HOSTED_STATE_READY) {
                ctx->heartbeat_tick = HAL_GetTick();
            }
            return STM_OK;
        }
        if (ctx->diagnostics.state == ESP_HOSTED_STATE_FAULT || ctx->recovery_phase ||
            (ctx->diagnostics.generation && !ctx->wifi_initialized)) { return STM_OK; }
        if (id == WIFI_CONNECTED_EVENT) {
            event.id = EH_WIFI_EVENT_STA_CONNECTED;
            if (field(evt, evt_len, 2U, 2U, &nested, &nested_len, &val) != 1) {
                return STM_ERR_VERIFY;
            }
            ctx->last_disconnect_reason = 0U;
            link_change(ctx, 1U);
        } else if (id == WIFI_DISCONNECTED_EVENT) {
            event.id = EH_WIFI_EVENT_STA_DISCONNECTED;
            if (field(evt, evt_len, 2U, 2U, &nested, &nested_len, &val) != 1) {
                return STM_ERR_VERIFY;
            }
            val = 0U;
            if (field(nested, nested_len, 4U, 0U, &evt, &evt_len, &val) < 0 ||
                val > UINT32_MAX) { return STM_ERR_VERIFY; }
            event.reason = (uint32_t)val;
            ctx->last_disconnect_reason = event.reason;
            uint8_t pending = ctx->connect_pending;
            link_change(ctx, 0U);
            if (pending && !ctx->reconnect_waiting) { reconnect_schedule(ctx); }
        } else if (id == WIFI_SCAN_DONE_EVENT) {
            event.id = EH_WIFI_EVENT_SCAN_DONE;
            if (field(evt, evt_len, 2U, 2U, &nested, &nested_len, &val) != 1) {
                return STM_ERR_VERIFY;
            }
            val = 0U;
            if (field(nested, nested_len, 1U, 0U, &evt, &evt_len, &val) < 0 ||
                val > UINT32_MAX) { return STM_ERR_VERIFY; }
            event.scan_status = (uint32_t)val;
            val = 0U;
            if (field(nested, nested_len, 2U, 0U, &evt, &evt_len, &val) < 0 ||
                val > UINT32_MAX) { return STM_ERR_VERIFY; }
            event.scan_count = (uint32_t)val;
            if (!ctx->scan_pending) { return STM_OK; } /* stale event after stop */
            ctx->scan_pending = 0U;
            ctx->scan_done = (uint8_t)(event.scan_status == 0U);
        } else if (id == WIFI_NO_ARGS_EVENT) {
            if (field(evt, evt_len, 2U, 0U, &nested, &nested_len, &val) != 1) {
                return STM_ERR_VERIFY;
            }
            if (val != 12U && val != 13U) { return STM_OK; }
            event.id = val == 12U ? EH_WIFI_EVENT_AP_STARTED : EH_WIFI_EVENT_AP_STOPPED;
            ap_link_change(ctx, val == 12U);
        } else if (id == WIFI_AP_CLIENT_CONNECTED_EVENT ||
                   id == WIFI_AP_CLIENT_DISCONNECTED_EVENT) {
            event.id = id == WIFI_AP_CLIENT_CONNECTED_EVENT ?
                EH_WIFI_EVENT_AP_CLIENT_CONNECTED : EH_WIFI_EVENT_AP_CLIENT_DISCONNECTED;
            if (field(evt, evt_len, 2U, 2U, &nested, &nested_len, &val) != 1 || nested_len != 6U) {
                return STM_ERR_VERIFY;
            }
            memcpy(event.client_mac, nested, 6U);
            val = 0U;
            if (field(evt, evt_len, 3U, 0U, &nested, &nested_len, &val) < 0) { return STM_ERR_VERIFY; }
            event.aid = (uint16_t)val;
            val = 0U;
            if (id == WIFI_AP_CLIENT_DISCONNECTED_EVENT &&
                field(evt, evt_len, 5U, 0U, &nested, &nested_len, &val) < 0) { return STM_ERR_VERIFY; }
            event.reason = (uint32_t)val;
        } else { return STM_OK; }
        emit_wifi_event(ctx, &event);
        return STM_OK;
    }
    if (type != 2U || id > UINT16_MAX ||
        field(p, rpc_len, 3U, 0U, &body, &body_len, &uid) != 1 || uid > UINT32_MAX ||
        field(p, rpc_len, (uint32_t)id, 2U, &body, &body_len, &uid) != 1 ||
        body_len > sizeof(ctx->response_data)) { return STM_ERR_VERIFY; }
    /* Ignore delayed responses from an older transaction. */
    if (!ctx->request_active || id != ctx->response_id || uid != ctx->response_uid) {
        esp_hosted_count(&ctx->diagnostics.late_responses); return STM_OK;
    }
    if (ctx->response_ready) { return STM_OK; }
    memcpy(ctx->response_data, body, body_len);
    ctx->response_length = body_len;
    ctx->response_ready = 1U;
    return STM_OK;
}
static stm_err_t exchange(struct esp_hosted_context *ctx, const uint8_t *tx)
{
    stm_err_t err = esp_hosted_transfer(ctx, tx, NULL);
    if (err != STM_OK) { return err; }
    err = consume(ctx);
    /* RPC frames can include Wi-Fi configuration (and its password), even if late. */
    if ((ctx->config.rx_buffer[0] & 0x0FU) == RPC_IF) {
        clear_sensitive(ctx->config.rx_buffer, ESP_HOSTED_FRAME_SIZE);
    }
    return err;
}
static stm_err_t poll_frame(esp_hosted_handle_t handle)
{
    esp_hosted_signals_t pins;
    stm_err_t err = esp_hosted_get_signals(handle, &pins);
    if (err != STM_OK) { return err; }
    if (pins.handshake != GPIO_PIN_SET || pins.data_ready != GPIO_PIN_SET) { return STM_OK; }
    return exchange(handle, NULL);
}
static void monitor_update(struct esp_hosted_context *ctx)
{
    if (ctx->monitor.enabled && ctx->diagnostics.state == ESP_HOSTED_STATE_READY &&
        (uint32_t)(HAL_GetTick() - ctx->heartbeat_tick) >= ctx->monitor.timeout_ms) {
        esp_hosted_count(&ctx->diagnostics.heartbeat_timeouts);
        esp_hosted_invalidate(ctx, ESP_HOSTED_FAULT_HEARTBEAT_TIMEOUT,
                              STM_ERR_TIMEOUT, ESP_HOSTED_STATE_FAULT);
    }
}
stm_err_t esp_hosted_set_callbacks(esp_hosted_handle_t handle, esp_hosted_rx_fn rx,
                                   esp_hosted_link_fn link, void *user)
{
    if (handle && handle->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (handle && (handle->recovery_phase || handle->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!handle) { return STM_ERR_INVALID_ARG; }
    handle->receive = rx; handle->link = link; handle->user = user;
    return STM_OK;
}
static stm_err_t request_begin(struct esp_hosted_context *ctx, uint16_t id,
                               const uint8_t *body, size_t body_len, uint32_t timeout_ms)
{
    uint8_t rpc[RPC_BODY_CAP], wire[RPC_WIRE_CAP];
    static const uint8_t ep[] = "RPCRsp";
    size_t n = 0, w = 0;
    if (ctx->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (ctx->request_active || !ctx->negotiated || body_len > 192U || (!body && body_len)) { return STM_ERR_INVALID_STATE; }
    if (!timeout_ms || timeout_ms > INT32_MAX) { return STM_ERR_INVALID_ARG; }
    if (ctx->uid == UINT32_MAX) { return STM_ERR_OUT_OF_RANGE; }
    ctx->response_id = id + 256U;
    ctx->info.last_rpc_id = id;
    ctx->info.last_rpc_status = 0U;
    ctx->info.last_rpc_status_present = 0U;
    ctx->response_uid = ++ctx->uid;
    ctx->request_id = id;
    ctx->request_epoch = ctx->session_epoch;
    ctx->request_tick = HAL_GetTick(); ctx->request_timeout = timeout_ms;
    ctx->request_error = STM_OK; ctx->request_sent = 0U;
    clear_sensitive(ctx->response_data, sizeof(ctx->response_data));
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
    clear_sensitive(wire, sizeof(wire)); clear_sensitive(rpc, sizeof(rpc));
    if (err == STM_OK) { ctx->request_active = 1U; }
    return err;
}
static stm_err_t bounded_exchange(struct esp_hosted_context *ctx, const uint8_t *tx,
                                   uint32_t remaining)
{
    uint32_t saved = ctx->config.transfer_timeout_ms;
    if (saved > remaining) { ctx->config.transfer_timeout_ms = remaining; }
    stm_err_t err = exchange(ctx, tx);
    ctx->config.transfer_timeout_ms = saved;
    return err;
}
/* One bounded transaction per step; a pending request never busy-waits for GPIO. */
static stm_err_t request_step(struct esp_hosted_context *ctx)
{
    if (ctx->request_epoch != ctx->session_epoch || !ctx->request_active) { return STM_ERR_CANCELLED; }
    uint32_t elapsed = HAL_GetTick() - ctx->request_tick;
    if (elapsed >= ctx->request_timeout) { return STM_ERR_TIMEOUT; }
    stm_err_t err = STM_OK;
    if (!ctx->request_sent) {
        if (HAL_GPIO_ReadPin(ctx->config.handshake_port, ctx->config.handshake_pin) == GPIO_PIN_SET) {
            ctx->request_sent = 1U;
            err = bounded_exchange(ctx, ctx->config.tx_buffer, ctx->request_timeout - elapsed);
            if (ctx->request_id == 284U) { clear_sensitive(ctx->config.tx_buffer, ESP_HOSTED_FRAME_SIZE); }
        }
    } else {
        esp_hosted_signals_t signals;
        esp_hosted_get_signals(ctx, &signals);
        if (signals.handshake == GPIO_PIN_SET && signals.data_ready == GPIO_PIN_SET) {
            err = bounded_exchange(ctx, NULL, ctx->request_timeout - elapsed);
        }
    }
    monitor_update(ctx);
    if (ctx->request_epoch != ctx->session_epoch) { return STM_ERR_CANCELLED; }
    if (err == STM_OK && HAL_GetTick() - ctx->request_tick >= ctx->request_timeout) { return STM_ERR_TIMEOUT; }
    return err;
}
static stm_err_t request_finish(struct esp_hosted_context *ctx, stm_err_t err,
                                uint8_t *out, size_t *out_len)
{
    if (err == STM_OK) {
        const uint8_t *unused = NULL; size_t size = 0; uint64_t status = 0;
        uint32_t status_field = (ctx->request_id == 259U || ctx->request_id == 257U ||
                                 ctx->request_id == 276U) ? 2U : 1U;
        int has = field(ctx->response_data, ctx->response_length, status_field, 0U,
                        &unused, &size, &status);
        if (has == 1) {
            ctx->info.last_rpc_status_present = 1U;
            ctx->info.last_rpc_status = (uint32_t)status;
        }
        if (has < 0 || status > UINT32_MAX) { err = STM_ERR_VERIFY; }
        else if (has == 1 && status != 0U) { err = STM_ERR_IO; }
        if (err == STM_OK && out && out_len) {
            if (*out_len < ctx->response_length) { err = STM_ERR_OUT_OF_RANGE; }
            else { memcpy(out, ctx->response_data, ctx->response_length);
                   *out_len = ctx->response_length; }
        }
    }
    if (err != STM_OK) {
        if (err == STM_ERR_TIMEOUT) { esp_hosted_count(&ctx->diagnostics.rpc_timeouts); }
        /* Preserve the cause that invalidated this request. */
        if (err != STM_ERR_CANCELLED) { esp_hosted_record_fault(ctx, ESP_HOSTED_FAULT_RPC, err); }
        ctx->diagnostics.last_failed_rpc = ctx->request_id;
        ctx->diagnostics.last_cp_status = ctx->info.last_rpc_status;
        ctx->diagnostics.last_cp_status_present = ctx->info.last_rpc_status_present;
    }
    clear_sensitive(ctx->response_data, sizeof(ctx->response_data));
    ctx->response_length = 0U; ctx->response_ready = 0U;
    ctx->request_active = ctx->request_sent = 0U;
    if (ctx->request_id == 284U) { clear_sensitive(ctx->config.tx_buffer, ESP_HOSTED_FRAME_SIZE); }
    return err;
}
static stm_err_t request(struct esp_hosted_context *ctx, uint16_t id,
                         const uint8_t *body, size_t body_len,
                         uint8_t *out, size_t *out_len, uint32_t timeout_ms)
{
    if (ctx->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (ctx->recovery_phase || ctx->diagnostics.state == ESP_HOSTED_STATE_FAULT) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = request_begin(ctx, id, body, body_len, timeout_ms);
    if (err != STM_OK) { return err; }
    while (err == STM_OK && !ctx->response_ready) { err = request_step(ctx); }
    return request_finish(ctx, err, out, out_len);
}
/* Parsing takes place after the transaction engine validates the CP status. */
static stm_err_t rpc_verify(struct esp_hosted_context *ctx)
{
    esp_hosted_record_fault(ctx, ESP_HOSTED_FAULT_RPC, STM_ERR_VERIFY);
    ctx->diagnostics.last_failed_rpc = ctx->request_id;
    ctx->diagnostics.last_cp_status = ctx->info.last_rpc_status;
    ctx->diagnostics.last_cp_status_present = ctx->info.last_rpc_status_present;
    return STM_ERR_VERIFY;
}
static stm_err_t version_parse(struct esp_hosted_context *handle, const uint8_t *version, size_t len)
{
    uint64_t major = 0, minor = 0, patch = 0;
    const uint8_t *unused = NULL; size_t unused_len = 0;
    if (field(version, len, 2U, 0U, &unused, &unused_len, &major) != 1 ||
        field(version, len, 3U, 0U, &unused, &unused_len, &minor) < 0 ||
        field(version, len, 4U, 0U, &unused, &unused_len, &patch) < 0 ||
        major != 3U || minor != 0U || patch != 9U) { return STM_ERR_NOT_SUPPORTED; }
    handle->version = (esp_hosted_version_t){(uint8_t)major, (uint8_t)minor, (uint8_t)patch};
    return STM_OK;
}
static size_t monitor_body(uint8_t *body, const esp_hosted_monitor_config_t *config)
{
    size_t n = put_num(body, 1U, config->enabled);
    return n + put_num(body + n, 2U, config->enabled ? config->interval_s : 0U);
}
stm_err_t esp_hosted_set_monitor(esp_hosted_handle_t h,
                                const esp_hosted_monitor_config_t *config, uint32_t timeout_ms)
{
    if (!h || !config || !timeout_ms || config->enabled > 1U ||
        (config->enabled && (config->interval_s < 10U || config->interval_s > 3600U ||
         config->timeout_ms <= config->interval_s * 2000U || config->timeout_ms > INT32_MAX))) {
        return STM_ERR_INVALID_ARG;
    }
    if (h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    uint8_t body[16]; size_t n = monitor_body(body, config);
    stm_err_t err = request(h, 277U, body, n, NULL, NULL, timeout_ms);
    if (err == STM_OK) { h->monitor = *config; h->heartbeat_tick = HAL_GetTick(); }
    return err;
}
stm_err_t esp_hosted_get_diagnostics(esp_hosted_handle_t h, esp_hosted_diagnostics_t *out)
{
    if (!h || !out) { return STM_ERR_INVALID_ARG; }
    *out = h->diagnostics; return STM_OK;
}
stm_err_t esp_hosted_recover_begin(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (!h || !timeout_ms || timeout_ms > INT32_MAX) { return STM_ERR_INVALID_ARG; }
    if (h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h->recovery_phase || h->request_active) { return STM_ERR_INVALID_STATE; }
    esp_hosted_fault_t cause = h->diagnostics.last_fault;
    uint32_t cause_tick = h->diagnostics.last_fault_tick;
    stm_err_t cause_error = h->diagnostics.last_error;
    esp_hosted_invalidate(h, ESP_HOSTED_FAULT_RESET, STM_ERR_CANCELLED, ESP_HOSTED_STATE_RECOVERING);
    if (cause != ESP_HOSTED_FAULT_NONE) {
        h->diagnostics.last_fault = cause; h->diagnostics.last_fault_tick = cause_tick;
        h->diagnostics.last_error = cause_error;
    }
    h->recovery_tick = h->phase_tick = HAL_GetTick(); h->recovery_timeout = timeout_ms;
    h->recovery_phase = 1U;
    HAL_GPIO_WritePin(h->config.cs_port, h->config.cs_pin, GPIO_PIN_SET);
    HAL_GPIO_WritePin(h->config.reset_port, h->config.reset_pin, GPIO_PIN_RESET);
    return STM_OK;
}
static stm_err_t recovery_failed(struct esp_hosted_context *h, stm_err_t err)
{
    HAL_GPIO_WritePin(h->config.reset_port, h->config.reset_pin, GPIO_PIN_SET);
    if (h->request_active) { (void)request_finish(h, err, NULL, NULL); }
    h->recovery_phase = 0U;
    esp_hosted_count(&h->diagnostics.recovery_failures);
    esp_hosted_invalidate(h, ESP_HOSTED_FAULT_RECOVERY, err, ESP_HOSTED_STATE_FAULT);
    return err;
}
static stm_err_t recovery_step(struct esp_hosted_context *h)
{
    uint32_t now = HAL_GetTick(), elapsed = now - h->recovery_tick;
    if (elapsed >= h->recovery_timeout) { return recovery_failed(h, STM_ERR_TIMEOUT); }
    stm_err_t err = STM_OK;
    uint32_t remaining = h->recovery_timeout - elapsed;
    if (h->recovery_phase == 1U) {
        if (now - h->phase_tick >= 10U) {
            HAL_GPIO_WritePin(h->config.reset_port, h->config.reset_pin, GPIO_PIN_SET);
            h->phase_tick = now; h->recovery_phase = 2U;
        }
    } else if (h->recovery_phase == 2U) {
        if (now - h->phase_tick >= 100U) { h->recovery_phase = 3U; }
    } else if (h->recovery_phase == 3U) {
        uint32_t saved = h->config.transfer_timeout_ms;
        if (saved > remaining) { h->config.transfer_timeout_ms = remaining; }
        err = poll_frame(h); h->config.transfer_timeout_ms = saved;
        if (h->negotiated) { h->recovery_phase = 4U; }
    } else if (h->recovery_phase == 4U) {
        if (HAL_GPIO_ReadPin(h->config.handshake_port, h->config.handshake_pin) == GPIO_PIN_SET) {
            err = bounded_exchange(h, h->config.tx_buffer, remaining);
            if (err == STM_OK) { err = request_begin(h, 350U, NULL, 0U, remaining); }
            if (err == STM_OK) { h->recovery_phase = 5U; }
        }
    } else if (h->recovery_phase == 5U || h->recovery_phase == 6U) {
        err = request_step(h);
        if (err == STM_OK && h->response_ready) {
            uint8_t response[64]; size_t n = sizeof(response);
            err = request_finish(h, STM_OK, response, &n);
            if (err == STM_OK && h->recovery_phase == 5U) { err = version_parse(h, response, n); }
            clear_sensitive(response, sizeof(response));
            if (err == STM_OK && h->recovery_phase == 5U && h->monitor.enabled) {
                uint8_t body[16]; n = monitor_body(body, &h->monitor);
                remaining = h->recovery_timeout - (HAL_GetTick() - h->recovery_tick);
                if (remaining > h->recovery_timeout) { err = STM_ERR_TIMEOUT; }
                else { err = request_begin(h, 277U, body, n, remaining); }
                if (err == STM_OK) { h->recovery_phase = 6U; }
            } else if (err == STM_OK) {
                h->recovery_phase = 0U; h->initialized = 1U;
                h->heartbeat_tick = HAL_GetTick();
                h->diagnostics.state = ESP_HOSTED_STATE_READY;
                esp_hosted_count(&h->diagnostics.recovery_successes);
            }
        }
    }
    return err != STM_OK ? recovery_failed(h, err) : STM_OK;
}
stm_err_t esp_hosted_poll(esp_hosted_handle_t h)
{
    if (!h) { return STM_ERR_INVALID_ARG; }
    if (h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h->recovery_phase) { return recovery_step(h); }
    if (h->request_active) { return STM_ERR_INVALID_STATE; }
    if (h->queued_count && h->diagnostics.state == ESP_HOSTED_STATE_READY &&
        HAL_GPIO_ReadPin(h->config.handshake_port, h->config.handshake_pin) == GPIO_PIN_SET) {
        uint8_t index = h->queued_head;
        uint8_t frame[ESP_HOSTED_STA_MTU + 14U];
        size_t length = h->queued_length[index];
        memcpy(frame, h->queued_frame[index], length);
        h->queued_head = (index + 1U) % 2U; --h->queued_count;
        stm_err_t err = h->queued_iface[index] == ESP_HOSTED_STA_IF_TYPE ?
            esp_hosted_send(h, frame, length) : eh_wifi_ap_send(h, frame, length);
        monitor_update(h); return err;
    }
    stm_err_t err = poll_frame(h);
    monitor_update(h);
    return err;
}
stm_err_t esp_hosted_start(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    stm_err_t err = esp_hosted_recover_begin(h, timeout_ms);
    if (err != STM_OK) { return err; }
    while (h->recovery_phase && err == STM_OK) { err = esp_hosted_poll(h); }
    return err;
}
stm_err_t esp_hosted_get_version(esp_hosted_handle_t h, esp_hosted_version_t *v)
{
    if (!h || !v) { return STM_ERR_INVALID_ARG; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    *v = h->version; return STM_OK;
}
static stm_err_t queue_frame(struct esp_hosted_context *h, uint8_t iface,
                              const uint8_t *frame, size_t length)
{
    if (!h->rx_callback) { return STM_ERR_INVALID_CONTEXT; }
    if (h->queued_count == 2U) { return STM_ERR_NO_MEM; }
    uint8_t index = (h->queued_head + h->queued_count) % 2U;
    memcpy(h->queued_frame[index], frame, length);
    h->queued_length[index] = (uint16_t)length; h->queued_iface[index] = iface;
    ++h->queued_count; return STM_OK;
}
stm_err_t esp_hosted_send(esp_hosted_handle_t h, const uint8_t *frame, size_t length)
{

    if (!h || !frame || length < 14U || length > ESP_HOSTED_STA_MTU + 14U) {
        return STM_ERR_INVALID_ARG;
    }
    if (!h->initialized || !h->connected) { return STM_ERR_INVALID_STATE; }
    if (h->callback_depth) { return queue_frame(h, ESP_HOSTED_STA_IF_TYPE, frame, length); }
    if (h->recovery_phase || h->request_active) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = esp_hosted_encode_frame(h, ESP_HOSTED_STA_IF_TYPE, 0U,
                                             ++h->sequence, frame, length,
                                             h->config.tx_buffer, ESP_HOSTED_FRAME_SIZE);
    if (err == STM_OK) { err = esp_hosted_wait_handshake(h, 1000U); }
    return err == STM_OK ? exchange(h, h->config.tx_buffer) : err;
}

static size_t bounded_string(const char *s, size_t max)
{
    size_t n = 0U;
    while (n <= max && s[n] != '\0') { ++n; }
    return n;
}

stm_err_t eh_wifi_init(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->initialized) { return STM_ERR_INVALID_STATE; }
    if (h->wifi_initialized) { return STM_OK; }
    uint8_t cfg[96], body[112]; size_t n = 0U;
    n += put_num(cfg + n, 1U, 10U); n += put_num(cfg + n, 2U, 32U);
    n += put_num(cfg + n, 3U, 1U); n += put_num(cfg + n, 5U, 32U);
    n += put_num(cfg + n, 8U, 1U); n += put_num(cfg + n, 9U, 1U);
    n += put_num(cfg + n, 11U, 1U); n += put_num(cfg + n, 13U, 6U);
    n += put_num(cfg + n, 15U, 752U); n += put_num(cfg + n, 16U, 32U);
    n += put_num(cfg + n, 20U, 0x1F2F3F4FU);
    size_t len = put_bytes(body, 1U, cfg, n);
    stm_err_t err = request(h, 278U, body, len, NULL, NULL, timeout_ms);
    if (err == STM_OK) { err = request(h, 259U, NULL, 0U, NULL, NULL, timeout_ms); }
    memset(cfg, 0, sizeof(cfg)); memset(body, 0, sizeof(body));
    if (err == STM_OK) { h->wifi_initialized = 1U; }
    return err;
}
stm_err_t eh_wifi_set_mode(esp_hosted_handle_t h, eh_wifi_mode_t mode, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms || mode > EH_WIFI_MODE_APSTA) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized) { return STM_ERR_INVALID_STATE; }
    uint8_t body[4]; size_t n = put_num(body, 1U, (uint32_t)mode);
    stm_err_t err = request(h, 260U, body, n, NULL, NULL, timeout_ms);
    if (err == STM_OK) {
        h->wifi_mode = (uint8_t)mode;
        if (!(mode & EH_WIFI_MODE_STA)) { reconnect_clear(h); link_change(h, 0U); h->scan_pending = h->scan_done = 0U; }
        if (!(mode & EH_WIFI_MODE_AP)) { ap_link_change(h, 0U); }
    }
    return err;
}
stm_err_t eh_wifi_get_mode(esp_hosted_handle_t h, eh_wifi_mode_t *mode, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !mode || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized) { return STM_ERR_INVALID_STATE; }
    uint8_t response[16] = {0}; size_t len = sizeof(response);
    stm_err_t err = request(h, 259U, NULL, 0U, response, &len, timeout_ms);
    if (err == STM_OK) {
        const uint8_t *unused = NULL; size_t size = 0U; uint64_t value = 0U;
        /* Proto3 elides NULL (zero); response field 2 was checked by request(). */
        int present = field(response, len, 1U, 0U, &unused, &size, &value);
        if (present < 0 || value > EH_WIFI_MODE_APSTA) { err = STM_ERR_VERIFY; }
        else { *mode = (eh_wifi_mode_t)value; }
    }
    clear_sensitive(response, sizeof(response));
    return err;
}

stm_err_t eh_wifi_set_ps(esp_hosted_handle_t h, eh_wifi_ps_t mode, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms || (unsigned)mode > EH_WIFI_PS_MAX_MODEM) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized || !(h->wifi_mode & EH_WIFI_MODE_STA)) { return STM_ERR_INVALID_STATE; }
    uint8_t body[4]; size_t len = put_num(body, 1U, (uint32_t)mode);
    return request(h, 270U, body, len, NULL, NULL, timeout_ms);
}

stm_err_t eh_wifi_get_ps(esp_hosted_handle_t h, eh_wifi_ps_t *mode, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !mode || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized || !(h->wifi_mode & EH_WIFI_MODE_STA)) { return STM_ERR_INVALID_STATE; }
    uint8_t response[16] = {0}; size_t len = sizeof(response);
    stm_err_t err = request(h, 271U, NULL, 0U, response, &len, timeout_ms);
    if (err == STM_OK) {
        const uint8_t *unused = NULL; size_t size = 0U; uint64_t value = 0U;
        int present = field(response, len, 2U, 0U, &unused, &size, &value);
        if (present < 0 || value > EH_WIFI_PS_MAX_MODEM) { err = STM_ERR_VERIFY; }
        else { *mode = (eh_wifi_ps_t)value; }
    }
    clear_sensitive(response, sizeof(response));
    return err;
}

static int radio_iface_valid(eh_wifi_if_t iface)
{
    return iface == EH_WIFI_IF_STA || iface == EH_WIFI_IF_AP;
}
static int radio_iface_enabled(esp_hosted_handle_t h, eh_wifi_if_t iface)
{
    return h->wifi_initialized && (h->wifi_mode &
        (iface == EH_WIFI_IF_STA ? EH_WIFI_MODE_STA : EH_WIFI_MODE_AP));
}
static int protocol_valid(uint64_t bitmap)
{
    return bitmap == EH_WIFI_PROTOCOL_11B ||
        bitmap == (EH_WIFI_PROTOCOL_11B | EH_WIFI_PROTOCOL_11G) ||
        bitmap == (EH_WIFI_PROTOCOL_11B | EH_WIFI_PROTOCOL_11G | EH_WIFI_PROTOCOL_11N);
}
static stm_err_t radio_set(esp_hosted_handle_t h, eh_wifi_if_t iface,
                           uint32_t id, uint32_t value, uint32_t timeout_ms)
{
    uint8_t body[16];
    size_t n = put_num(body, 1U, (uint32_t)iface);
    n += put_num(body + n, 2U, value);
    return request(h, id, body, n, NULL, NULL, timeout_ms);
}
static stm_err_t radio_get(esp_hosted_handle_t h, eh_wifi_if_t iface,
                           uint32_t id, uint64_t *value, uint32_t timeout_ms)
{
    uint8_t body[8], response[32]; size_t len = sizeof(response);
    size_t n = put_num(body, 1U, (uint32_t)iface);
    stm_err_t err = request(h, id, body, n, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *unused = NULL; size_t size = 0U;
    return field(response, len, 2U, 0U, &unused, &size, value) == 1 ? STM_OK : STM_ERR_VERIFY;
}
stm_err_t eh_wifi_set_protocol(esp_hosted_handle_t h, eh_wifi_if_t iface,
                              uint8_t bitmap, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !radio_iface_valid(iface) || !timeout_ms || !protocol_valid(bitmap)) {
        return STM_ERR_INVALID_ARG;
    }
    if (!radio_iface_enabled(h, iface)) { return STM_ERR_INVALID_STATE; }
    return radio_set(h, iface, 297U, bitmap, timeout_ms);
}
stm_err_t eh_wifi_get_protocol(esp_hosted_handle_t h, eh_wifi_if_t iface,
                              uint8_t *bitmap, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !radio_iface_valid(iface) || !bitmap || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!radio_iface_enabled(h, iface)) { return STM_ERR_INVALID_STATE; }
    uint64_t value = 0U;
    stm_err_t err = radio_get(h, iface, 298U, &value, timeout_ms);
    if (err != STM_OK) { return err; }
    if (!protocol_valid(value)) { return STM_ERR_VERIFY; }
    *bitmap = (uint8_t)value;
    return STM_OK;
}
stm_err_t eh_wifi_set_bandwidth(esp_hosted_handle_t h, eh_wifi_if_t iface,
                               eh_wifi_bandwidth_t bandwidth, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !radio_iface_valid(iface) || !timeout_ms ||
        (bandwidth != EH_WIFI_BW_HT20 && bandwidth != EH_WIFI_BW_HT40)) {
        return STM_ERR_INVALID_ARG;
    }
    if (!radio_iface_enabled(h, iface)) { return STM_ERR_INVALID_STATE; }
    return radio_set(h, iface, 299U, (uint32_t)bandwidth, timeout_ms);
}
stm_err_t eh_wifi_get_bandwidth(esp_hosted_handle_t h, eh_wifi_if_t iface,
                               eh_wifi_bandwidth_t *bandwidth, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !radio_iface_valid(iface) || !bandwidth || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!radio_iface_enabled(h, iface)) { return STM_ERR_INVALID_STATE; }
    uint64_t value = 0U;
    stm_err_t err = radio_get(h, iface, 300U, &value, timeout_ms);
    if (err != STM_OK) { return err; }
    if (value != EH_WIFI_BW_HT20 && value != EH_WIFI_BW_HT40) { return STM_ERR_VERIFY; }
    *bandwidth = (eh_wifi_bandwidth_t)value;
    return STM_OK;
}

/** Decode only the documented, credential-free subset of wifi_config. */
static stm_err_t parse_config_info(const uint8_t *response, size_t len, eh_wifi_if_t iface,
                                   eh_wifi_config_info_t *out)
{
    const uint8_t *data = NULL, *cfg = NULL, *entry = NULL;
    size_t size = 0U, cfg_len = 0U, entry_len = 0U;
    uint64_t value = 0U;
    /* Proto3 omits the zero-valued STA interface, but AP must be explicit. */
    int iface_present = field(response, len, 2U, 0U, &data, &size, &value);
    if (iface_present < 0 || (iface == EH_WIFI_IF_AP && iface_present != 1) ||
        value != (uint32_t)iface ||
        field(response, len, 3U, 2U, &cfg, &cfg_len, &value) != 1 ||
        field(cfg, cfg_len, iface == EH_WIFI_IF_STA ? 1U : 2U, 2U,
              &data, &size, &value) != 0 ||
        field(cfg, cfg_len, iface == EH_WIFI_IF_STA ? 2U : 1U, 2U,
              &entry, &entry_len, &value) != 1 ||
        field(entry, entry_len, 1U, 2U, &data, &size, &value) != 1 ||
        size == 0U || size > 32U) { return STM_ERR_VERIFY; }
    eh_wifi_config_info_t parsed = {0};
    memcpy(parsed.ssid, data, size);
    if (iface == EH_WIFI_IF_AP) {
        uint64_t number = 0U;
        if (field(entry, entry_len, 4U, 0U, &data, &size, &number) != 1 ||
            number == 0U || number > 14U) { return STM_ERR_VERIFY; }
        parsed.channel = (uint8_t)number; number = 0U;
        if (field(entry, entry_len, 6U, 0U, &data, &size, &number) < 0 || number > 1U) {
            return STM_ERR_VERIFY;
        }
        parsed.hidden = (uint8_t)number; number = 0U;
        if (field(entry, entry_len, 7U, 0U, &data, &size, &number) != 1 ||
            number == 0U || number > UINT8_MAX) { return STM_ERR_VERIFY; }
        parsed.max_connections = (uint8_t)number; number = 0U;
        if (field(entry, entry_len, 5U, 0U, &data, &size, &number) < 0 || number > UINT8_MAX) {
            return STM_ERR_VERIFY;
        }
        parsed.authmode = (uint8_t)number;
    }
    *out = parsed;
    return STM_OK;
}

stm_err_t eh_wifi_get_config(esp_hosted_handle_t h, eh_wifi_if_t iface,
                             eh_wifi_config_info_t *info, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !info || !timeout_ms || iface > EH_WIFI_IF_AP) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized || !(h->wifi_mode & (iface == EH_WIFI_IF_STA ?
                                                     EH_WIFI_MODE_STA : EH_WIFI_MODE_AP))) {
        return STM_ERR_INVALID_STATE;
    }
    uint8_t request_body[4]; size_t body_len = put_num(request_body, 1U, (uint32_t)iface);
    uint8_t response[512] = {0}; size_t len = sizeof(response);
    stm_err_t err = request(h, 285U, request_body, body_len, response, &len, timeout_ms);
    if (err == STM_OK) { err = parse_config_info(response, len, iface, info); }
    clear_sensitive(response, sizeof(response));
    return err;
}

stm_err_t eh_wifi_set_config(esp_hosted_handle_t h, eh_wifi_if_t iface,
                             const eh_wifi_config_t *config, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !config || !timeout_ms || iface > EH_WIFI_IF_AP) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized || h->wifi_mode == EH_WIFI_MODE_NULL ||
        (iface == EH_WIFI_IF_STA && !(h->wifi_mode & EH_WIFI_MODE_STA)) ||
        (iface == EH_WIFI_IF_AP && !(h->wifi_mode & EH_WIFI_MODE_AP))) { return STM_ERR_INVALID_STATE; }
    uint8_t inner[128] = {0}, cfg[152] = {0}, body[168] = {0};
    size_t n = 0U, ssid_len, pass_len;
    if (iface == EH_WIFI_IF_STA) {
        ssid_len = bounded_string(config->sta.ssid, 32U);
        pass_len = bounded_string(config->sta.password, 64U);
        if (!ssid_len || ssid_len > 32U || pass_len > 64U) { return STM_ERR_INVALID_CONFIG; }
        n += put_bytes(inner + n, 1U, (const uint8_t *)config->sta.ssid, ssid_len);
        n += put_bytes(inner + n, 2U, (const uint8_t *)config->sta.password, pass_len);
    } else {
        ssid_len = bounded_string(config->ap.ssid, 32U);
        pass_len = bounded_string(config->ap.password, 64U);
        if (!ssid_len || ssid_len > 32U || (pass_len && (pass_len < 8U || pass_len > 63U)) ||
            config->ap.channel > 13U || config->ap.max_connections > 4U) { return STM_ERR_INVALID_CONFIG; }
        n += put_bytes(inner + n, 1U, (const uint8_t *)config->ap.ssid, ssid_len);
        n += put_bytes(inner + n, 2U, (const uint8_t *)config->ap.password, pass_len);
        /* CP 3.0.9 only includes AP SSID in get_config when ssid_len is nonzero. */
        n += put_num(inner + n, 3U, ssid_len);
        n += put_num(inner + n, 4U, config->ap.channel ? config->ap.channel : 1U);
        n += put_num(inner + n, 5U, pass_len ? 3U : 0U); /* OPEN or WPA2-PSK */
        n += put_num(inner + n, 6U, config->ap.hidden ? 1U : 0U);
        n += put_num(inner + n, 7U, config->ap.max_connections ? config->ap.max_connections : 4U);
    }
    size_t cfg_len = put_bytes(cfg, iface == EH_WIFI_IF_STA ? 2U : 1U, inner, n);
    n = put_num(body, 1U, (uint32_t)iface);
    n += put_bytes(body + n, 2U, cfg, cfg_len);
    stm_err_t err = request(h, 284U, body, n, NULL, NULL, timeout_ms);
    clear_sensitive(inner, sizeof(inner)); clear_sensitive(cfg, sizeof(cfg)); clear_sensitive(body, sizeof(body));
    return err;
}
stm_err_t eh_wifi_start(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized || !h->wifi_mode) { return STM_ERR_INVALID_STATE; }
    if (h->wifi_started) { return STM_OK; }
    stm_err_t err = request(h, 280U, NULL, 0U, NULL, NULL, timeout_ms);
    if (err == STM_OK) { h->wifi_started = 1U; }
    return err;
}
stm_err_t eh_wifi_stop(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started) { return STM_ERR_INVALID_STATE; }
    reconnect_clear(h);
    stm_err_t err = request(h, 281U, NULL, 0U, NULL, NULL, timeout_ms);
    if (err == STM_OK) { h->wifi_started = 0U; h->scan_pending = h->scan_done = 0U; h->last_disconnect_reason = 0U;
        link_change(h, 0U); ap_link_change(h, 0U); }
    return err;
}
stm_err_t eh_wifi_connect(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_STA)) { return STM_ERR_INVALID_STATE; }
    uint32_t epoch = h->session_epoch;
    stm_err_t err = request(h, 282U, NULL, 0U, NULL, NULL, timeout_ms);
    /* An INIT/heartbeat fault inside request() must stay fully invalidated. */
    if (h->session_epoch != epoch) { return err; }
    h->reconnect_armed = h->reconnect_config.enabled;
    h->reconnect_attempts = 0U;
    h->reconnect_delay = h->reconnect_config.initial_delay_ms;
    h->reconnect_waiting = h->connect_pending = 0U;
    if (err == STM_OK && !h->connected) {
        h->connect_pending = 1U;
        h->reconnect_since = HAL_GetTick();
    } else if (err != STM_OK && !h->connected) { reconnect_schedule(h); }
    return err;
}
stm_err_t eh_wifi_disconnect(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_STA)) { return STM_ERR_INVALID_STATE; }
    /* Explicit disconnect cancels retries even when the CP rejects the request. */
    reconnect_clear(h);
    stm_err_t err = request(h, 283U, NULL, 0U, NULL, NULL, timeout_ms);
    if (err == STM_OK) { link_change(h, 0U); }
    return err;
}
stm_err_t eh_wifi_set_reconnect(esp_hosted_handle_t h,
                                const eh_wifi_reconnect_config_t *config)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !config || (config->enabled != 0U && config->enabled != 1U)) {
        return STM_ERR_INVALID_ARG;
    }
    if (config->enabled && (!config->initial_delay_ms ||
        config->initial_delay_ms > config->max_delay_ms ||
        config->max_delay_ms > INT32_MAX || !config->association_timeout_ms ||
        config->association_timeout_ms > INT32_MAX || !config->rpc_timeout_ms ||
        config->rpc_timeout_ms > INT32_MAX)) {
        return STM_ERR_INVALID_ARG;
    }
    h->reconnect_config = *config;
    reconnect_clear(h);
    /* Only an explicit connect call arms reconnection. A live connection can be armed now. */
    if (config->enabled && h->connected) { h->reconnect_armed = 1U; }
    return STM_OK;
}
stm_err_t eh_wifi_reconnect_update(esp_hosted_handle_t h)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h) { return STM_ERR_INVALID_ARG; }
    if (!h->reconnect_config.enabled || !h->reconnect_armed || !h->wifi_started ||
        !(h->wifi_mode & EH_WIFI_MODE_STA) || h->connected) { return STM_OK; }
    if (h->connect_pending) {
        if ((uint32_t)(HAL_GetTick() - h->reconnect_since) <
            h->reconnect_config.association_timeout_ms) { return STM_OK; }
        reconnect_schedule(h);
    }
    if (!h->reconnect_waiting ||
        (uint32_t)(HAL_GetTick() - h->reconnect_since) < h->reconnect_delay) {
        return STM_OK;
    }
    h->reconnect_waiting = 0U;
    if (h->reconnect_attempts < UINT16_MAX) { ++h->reconnect_attempts; }
    /* Events may arrive inside request(): advance the next delay first. */
    if (h->reconnect_delay < h->reconnect_config.max_delay_ms / 2U) {
        h->reconnect_delay *= 2U;
    } else { h->reconnect_delay = h->reconnect_config.max_delay_ms; }
    stm_err_t err = request(h, 282U, NULL, 0U, NULL, NULL,
                            h->reconnect_config.rpc_timeout_ms);
    if (err == STM_OK) {
        if (!h->connected) {
            h->connect_pending = 1U;
            h->reconnect_since = HAL_GetTick();
        }
    } else if (!h->connected) { reconnect_schedule(h); }
    return err;
}
uint8_t eh_wifi_is_connected(esp_hosted_handle_t h) { return h ? h->connected : 0U; }
stm_err_t eh_wifi_get_status(esp_hosted_handle_t h, eh_wifi_status_t *status)
{
    if (!h || !status) { return STM_ERR_INVALID_ARG; }
    *status = (eh_wifi_status_t){.mode = (eh_wifi_mode_t)h->wifi_mode,
        .started = h->wifi_started, .sta_connected = h->connected,
        .ap_started = h->ap_up, .scan_pending = h->scan_pending,
        .last_disconnect_reason = h->last_disconnect_reason,
        .reconnect_attempts = h->reconnect_attempts,
        .reconnect_pending = (uint8_t)(h->reconnect_waiting || h->connect_pending),
        .reconnect_enabled = h->reconnect_config.enabled};
    return STM_OK;
}

stm_err_t eh_wifi_set_event_callback(esp_hosted_handle_t h, eh_wifi_event_fn callback, void *user)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h) { return STM_ERR_INVALID_ARG; }
    h->wifi_event = callback; h->wifi_event_user = user; return STM_OK;
}
stm_err_t eh_wifi_set_ap_rx_callback(esp_hosted_handle_t h, eh_wifi_ap_rx_fn callback, void *user)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h) { return STM_ERR_INVALID_ARG; }
    h->ap_receive = callback; h->ap_user = user; return STM_OK;
}
stm_err_t eh_wifi_set_ap_link_callback(esp_hosted_handle_t h, eh_wifi_ap_link_fn callback, void *user)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h) { return STM_ERR_INVALID_ARG; }
    h->ap_link = callback; h->ap_link_user = user; return STM_OK;
}
stm_err_t eh_wifi_get_mac(esp_hosted_handle_t h, eh_wifi_if_t iface, uint8_t mac[6])
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !mac || iface > EH_WIFI_IF_AP) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_initialized) { return STM_ERR_INVALID_STATE; }
    uint8_t body[4], response[32]; size_t len = sizeof(response);
    size_t n = put_num(body, 1U, (uint32_t)iface);
    stm_err_t err = request(h, 257U, body, n, response, &len, 5000U);
    if (err != STM_OK) { return err; }
    const uint8_t *bytes = NULL; size_t count = 0U; uint64_t number = 0U;
    if (field(response, len, 1U, 2U, &bytes, &count, &number) != 1 || count != 6U) { return STM_ERR_VERIFY; }
    memcpy(mac, bytes, 6U);
    if (iface == EH_WIFI_IF_STA) { memcpy(h->mac, bytes, 6U); }
    return STM_OK;
}
stm_err_t eh_wifi_ap_send(esp_hosted_handle_t h, const uint8_t *frame, size_t length)
{

    if (!h || !frame || length < 14U || length > ESP_HOSTED_STA_MTU + 14U) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_AP)) { return STM_ERR_INVALID_STATE; }
    if (h->callback_depth) { return queue_frame(h, ESP_HOSTED_AP_IF_TYPE, frame, length); }
    if (h->recovery_phase || h->request_active) { return STM_ERR_INVALID_STATE; }
    stm_err_t err = esp_hosted_encode_frame(h, ESP_HOSTED_AP_IF_TYPE, 0U, ++h->sequence,
                                             frame, length, h->config.tx_buffer, ESP_HOSTED_FRAME_SIZE);
    if (err == STM_OK) { err = esp_hosted_wait_handshake(h, 1000U); }
    return err == STM_OK ? exchange(h, h->config.tx_buffer) : err;
}

stm_err_t eh_wifi_scan_start(esp_hosted_handle_t h,
                             const eh_wifi_scan_config_t *config, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_STA) || h->scan_pending) {
        return STM_ERR_INVALID_STATE;
    }
    uint8_t cfg[64], body[80]; size_t n = 0U, body_len = 0U;
    if (config) {
        /* CP 3.0.9 requires both scan_time and scan_time.active whenever
         * config_set is true. Otherwise it rejects the RPC before scanning. */
        uint8_t active[8], scan_time[16];
        size_t active_len = put_num(active, 1U, 30U);
        active_len += put_num(active + active_len, 2U, 120U);
        size_t time_len = put_bytes(scan_time, 1U, active, active_len);
        if (config->channel > 14U) { return STM_ERR_INVALID_CONFIG; }
        if (config->ssid) {
            size_t len = bounded_string(config->ssid, 32U);
            if (len > 32U) { return STM_ERR_INVALID_CONFIG; }
            n += put_bytes(cfg + n, 1U, (const uint8_t *)config->ssid, len);
        }
        if (config->channel) { n += put_num(cfg + n, 3U, config->channel); }
        if (config->show_hidden) { n += put_num(cfg + n, 4U, 1U); }
        n += put_bytes(cfg + n, 6U, scan_time, time_len);
        body_len += put_bytes(body + body_len, 1U, cfg, n);
        body_len += put_num(body + body_len, 3U, 1U);
    }
    h->scan_pending = 1U; h->scan_done = 0U;
    stm_err_t err = request(h, 286U, body, body_len, NULL, NULL, timeout_ms);
    if (err != STM_OK) { h->scan_pending = h->scan_done = 0U; }
    return err;
}
static stm_err_t parse_ap_record(const uint8_t *raw, size_t raw_len,
                                 eh_wifi_ap_record_t *record)
{
    const uint8_t *bytes = NULL; size_t bytes_len = 0U; uint64_t number = 0U;
    eh_wifi_ap_record_t parsed = {0};
    if (field(raw, raw_len, 1U, 2U, &bytes, &bytes_len, &number) != 1 || bytes_len != 6U) { return STM_ERR_VERIFY; }
    memcpy(parsed.bssid, bytes, 6U);
    if (field(raw, raw_len, 2U, 2U, &bytes, &bytes_len, &number) == 1) {
        if (bytes_len > 32U) { return STM_ERR_VERIFY; }
        memcpy(parsed.ssid, bytes, bytes_len);
    }
    number = 0U;
    if (field(raw, raw_len, 3U, 0U, &bytes, &bytes_len, &number) < 0 || number > UINT8_MAX) { return STM_ERR_VERIFY; }
    parsed.channel = (uint8_t)number;
    number = 0U;
    if (field(raw, raw_len, 5U, 0U, &bytes, &bytes_len, &number) < 0) { return STM_ERR_VERIFY; }
    parsed.rssi = (int8_t)(int32_t)number;
    number = 0U;
    if (field(raw, raw_len, 6U, 0U, &bytes, &bytes_len, &number) < 0 || number > UINT8_MAX) { return STM_ERR_VERIFY; }
    parsed.authmode = (uint8_t)number;
    *record = parsed;
    return STM_OK;
}
stm_err_t eh_wifi_sta_get_ap_info(esp_hosted_handle_t h, eh_wifi_ap_record_t *record,
                                  uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !record || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_STA) || !h->connected) {
        return STM_ERR_INVALID_STATE;
    }
    uint8_t response[128]; size_t len = sizeof(response);
    stm_err_t err = request(h, 294U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *raw = NULL; size_t raw_len = 0U; uint64_t unused = 0U;
    if (field(response, len, 2U, 2U, &raw, &raw_len, &unused) != 1) { return STM_ERR_VERIFY; }
    return parse_ap_record(raw, raw_len, record);
}
/* Accept protobuf int32's sign-extended encoding and its 32-bit representation. */
static int decode_rssi(uint64_t raw, int8_t *rssi)
{
    int64_t value;
    if (raw <= INT32_MAX) { value = (int64_t)raw; }
    else if (raw <= UINT32_MAX) { value = (int64_t)raw - ((int64_t)UINT32_MAX + 1); }
    else if (raw >= UINT64_MAX - INT32_MAX) { value = -(int64_t)(UINT64_MAX - raw) - 1; }
    else { return 0; }
    if (value < INT8_MIN || value > INT8_MAX) { return 0; }
    *rssi = (int8_t)value;
    return 1;
}
static int valid_client_mac(const uint8_t mac[6])
{
    static const uint8_t zero[6] = {0};
    return !(mac[0] & 1U) && memcmp(mac, zero, sizeof(zero)) != 0;
}
static int ap_active(esp_hosted_handle_t h)
{
    return h->wifi_started && (h->wifi_mode & EH_WIFI_MODE_AP) && h->ap_up;
}
static stm_err_t config_ready(esp_hosted_handle_t h, uint32_t timeout_ms, uint8_t started)
{
    if (!h || !timeout_ms || timeout_ms > INT32_MAX) { return STM_ERR_INVALID_ARG; }
    if (h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (!h->initialized || !h->wifi_initialized || h->recovery_phase || h->request_active ||
        (started && !h->wifi_started)) { return STM_ERR_INVALID_STATE; }
    return STM_OK;
}
static int country_parse(const uint8_t *bytes, size_t len, char out[4])
{
    if (!bytes || len < 2U || len > 3U) { return 0; }
    if (!((bytes[0] >= 'A' && bytes[0] <= 'Z' && bytes[1] >= 'A' && bytes[1] <= 'Z') ||
          (bytes[0] == '0' && bytes[1] == '1'))) { return 0; }
    if (len == 3U && bytes[2] != 0U && bytes[2] != ' ' && bytes[2] != 'O' &&
        bytes[2] != 'I' && bytes[2] != 'X') { return 0; }
    memset(out, 0, 4U); memcpy(out, bytes, len); return 1;
}
stm_err_t eh_wifi_set_country_code(esp_hosted_handle_t h, const char *country_code,
                                   uint8_t ieee80211d_enabled, uint32_t timeout_ms)
{
    if (!country_code || ieee80211d_enabled > 1U) { return STM_ERR_INVALID_ARG; }
    /* CP 3.0.9's set-code handler copies exactly two characters. */
    char checked[4];
    if (bounded_string(country_code, 2U) != 2U ||
        !country_parse((const uint8_t *)country_code, 2U, checked)) { return STM_ERR_INVALID_ARG; }
    stm_err_t err = config_ready(h, timeout_ms, 0U);
    if (err != STM_OK) { return err; }
    uint8_t body[16]; size_t n = put_bytes(body, 1U, (const uint8_t *)country_code, 2U);
    n += put_num(body + n, 2U, ieee80211d_enabled);
    return request(h, 334U, body, n, NULL, NULL, timeout_ms);
}
stm_err_t eh_wifi_get_country_code(esp_hosted_handle_t h, char country_code[4], uint32_t timeout_ms)
{
    if (!country_code) { return STM_ERR_INVALID_ARG; }
    stm_err_t err = config_ready(h, timeout_ms, 0U);
    if (err != STM_OK) { return err; }
    uint8_t response[64]; size_t len = sizeof(response), size = 0;
    const uint8_t *bytes = NULL; uint64_t number = 0; char parsed[4];
    err = request(h, 335U, NULL, 0U, response, &len, timeout_ms);
    if (err == STM_OK) {
        if (field(response, len, 2U, 2U, &bytes, &size, &number) != 1 ||
            !country_parse(bytes, size, parsed)) { err = STM_ERR_VERIFY; }
        else { memcpy(country_code, parsed, sizeof(parsed)); }
    }
    return err == STM_ERR_VERIFY ? rpc_verify(h) : err;
}
stm_err_t eh_wifi_get_country(esp_hosted_handle_t h, eh_wifi_country_info_t *info, uint32_t timeout_ms)
{
    if (!info) { return STM_ERR_INVALID_ARG; }
    stm_err_t err = config_ready(h, timeout_ms, 0U);
    if (err != STM_OK) { return err; }
    uint8_t response[128]; size_t len = sizeof(response), n = 0, size = 0;
    const uint8_t *country = NULL, *bytes = NULL; uint64_t value = 0;
    err = request(h, 304U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    eh_wifi_country_info_t parsed = {0};
    if (field(response, len, 2U, 2U, &country, &n, &value) != 1 ||
        field(country, n, 1U, 2U, &bytes, &size, &value) != 1 ||
        !country_parse(bytes, size, parsed.country_code) ||
        field(country, n, 2U, 0U, &bytes, &size, &value) != 1 || value < 1U || value > 14U) {
        return rpc_verify(h);
    }
    parsed.start_channel = (uint8_t)value;
    if (field(country, n, 3U, 0U, &bytes, &size, &value) != 1 || value < 1U ||
        value > 15U - parsed.start_channel) { return rpc_verify(h); }
    parsed.channel_count = (uint8_t)value;
    value = 0U;
    if (field(country, n, 4U, 0U, &bytes, &size, &value) < 0 || value > INT8_MAX) { return rpc_verify(h); }
    parsed.max_tx_power = (int8_t)value;
    value = 0U;
    if (field(country, n, 5U, 0U, &bytes, &size, &value) < 0 || value > EH_WIFI_COUNTRY_MANUAL) {
        return rpc_verify(h);
    }
    parsed.policy = (eh_wifi_country_policy_t)value;
    *info = parsed; return STM_OK;
}
stm_err_t eh_wifi_set_channel(esp_hosted_handle_t h, uint8_t primary,
                              eh_wifi_second_chan_t second, uint32_t timeout_ms)
{
    if (primary < 1U || primary > 14U || (unsigned)second > EH_WIFI_SECOND_CHAN_BELOW) {
        return STM_ERR_INVALID_ARG;
    }
    stm_err_t err = config_ready(h, timeout_ms, 1U);
    if (err != STM_OK) { return err; }
    if (h->scan_pending || h->connect_pending || h->connected) { return STM_ERR_INVALID_STATE; }
    uint8_t body[16]; size_t n = put_num(body, 1U, primary);
    n += put_num(body + n, 2U, second);
    return request(h, 301U, body, n, NULL, NULL, timeout_ms);
}
stm_err_t eh_wifi_set_max_tx_power(esp_hosted_handle_t h, int8_t power, uint32_t timeout_ms)
{
    if (power < 8 || power > 84) { return STM_ERR_INVALID_ARG; }
    stm_err_t err = config_ready(h, timeout_ms, 1U);
    if (err != STM_OK) { return err; }
    uint8_t body[8]; size_t n = put_num(body, 1U, (uint8_t)power);
    return request(h, 275U, body, n, NULL, NULL, timeout_ms);
}
stm_err_t eh_wifi_get_max_tx_power(esp_hosted_handle_t h, int8_t *power, uint32_t timeout_ms)
{
    if (!power) { return STM_ERR_INVALID_ARG; }
    stm_err_t err = config_ready(h, timeout_ms, 1U);
    if (err != STM_OK) { return err; }
    uint8_t response[64]; size_t len = sizeof(response), size = 0;
    const uint8_t *bytes = NULL; uint64_t value = 0;
    err = request(h, 276U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    if (field(response, len, 1U, 0U, &bytes, &size, &value) != 1 || value < 8U || value > 84U) {
        return rpc_verify(h);
    }
    *power = (int8_t)value; return STM_OK;
}
stm_err_t eh_wifi_sta_get_rssi(esp_hosted_handle_t h, int8_t *rssi, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !rssi || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_STA) || !h->connected) {
        return STM_ERR_INVALID_STATE;
    }
    uint8_t response[512]; size_t len = sizeof(response);
    stm_err_t err = request(h, 341U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *bytes = NULL; size_t size = 0U; uint64_t raw = 0U;
    int8_t parsed;
    if (field(response, len, 2U, 0U, &bytes, &size, &raw) < 0 ||
        !decode_rssi(raw, &parsed)) { return STM_ERR_VERIFY; }
    *rssi = parsed;
    return STM_OK;
}
stm_err_t eh_wifi_get_channel(esp_hosted_handle_t h, uint8_t *primary,
                             eh_wifi_second_chan_t *second, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !primary || !second || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started) { return STM_ERR_INVALID_STATE; }
    uint8_t response[512]; size_t len = sizeof(response);
    stm_err_t err = request(h, 302U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *bytes = NULL; size_t size = 0U; uint64_t channel = 0U, secondary = 0U;
    if (field(response, len, 2U, 0U, &bytes, &size, &channel) < 0 || channel < 1U || channel > 14U ||
        field(response, len, 3U, 0U, &bytes, &size, &secondary) < 0 ||
        secondary > EH_WIFI_SECOND_CHAN_BELOW) { return STM_ERR_VERIFY; }
    *primary = (uint8_t)channel; *second = (eh_wifi_second_chan_t)secondary;
    return STM_OK;
}
static stm_err_t parse_sta_record(const uint8_t *raw, size_t len, eh_wifi_sta_record_t *record)
{
    const uint8_t *mac = NULL; size_t mac_len = 0U; uint64_t rssi = 0U;
    eh_wifi_sta_record_t parsed = {0};
    if (field(raw, len, 1U, 2U, &mac, &mac_len, &rssi) != 1 || mac_len != 6U ||
        !valid_client_mac(mac)) { return STM_ERR_VERIFY; }
    memcpy(parsed.mac, mac, sizeof(parsed.mac));
    rssi = 0U;
    if (field(raw, len, 2U, 0U, &mac, &mac_len, &rssi) < 0 ||
        !decode_rssi(rssi, &parsed.rssi)) { return STM_ERR_VERIFY; }
    /* Validate the known protocol bitmask, though it is not exposed by this API. */
    uint64_t bitmask = 0U;
    if (field(raw, len, 3U, 0U, &mac, &mac_len, &bitmask) < 0 || bitmask > UINT32_MAX) {
        return STM_ERR_VERIFY;
    }
    *record = parsed;
    return STM_OK;
}
stm_err_t eh_wifi_ap_get_sta_list(esp_hosted_handle_t h, eh_wifi_sta_record_t *records,
                                 size_t capacity, size_t *count, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !count || !timeout_ms || (!records && capacity)) { return STM_ERR_INVALID_ARG; }
    if (!ap_active(h)) { return STM_ERR_INVALID_STATE; }
    uint8_t response[512]; size_t len = sizeof(response);
    stm_err_t err = request(h, 311U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *list = NULL; size_t list_len = 0U; uint64_t declared = 0U;
    if (field(response, len, 2U, 2U, &list, &list_len, &declared) != 1) { return STM_ERR_VERIFY; }
    const uint8_t *unused = NULL; size_t unused_len = 0U;
    if (field(list, list_len, 2U, 0U, &unused, &unused_len, &declared) < 0 || declared > INT32_MAX) {
        return STM_ERR_VERIFY;
    }
    /* Validate every entry before writing anything to the caller's array. */
    const uint8_t *p = list, *end = list + list_len;
    size_t total = 0U;
    while (p < end) {
        uint32_t tag; uint8_t wire; const uint8_t *raw; size_t size; uint64_t value;
        if (next_field(&p, end, &tag, &wire, &raw, &size, &value) < 0) { return STM_ERR_VERIFY; }
        if (tag == 1U) {
            eh_wifi_sta_record_t parsed;
            if (wire != 2U || parse_sta_record(raw, size, &parsed) != STM_OK) { return STM_ERR_VERIFY; }
            ++total;
        }
    }
    if (declared != total) { return STM_ERR_VERIFY; }
    if (!records && !capacity) { *count = total; return STM_OK; }
    if (capacity < total) { *count = total; return STM_ERR_OUT_OF_RANGE; }
    p = list; size_t index = 0U;
    while (p < end) {
        uint32_t tag; uint8_t wire; const uint8_t *raw; size_t size; uint64_t value;
        (void)next_field(&p, end, &tag, &wire, &raw, &size, &value);
        if (tag == 1U) { (void)parse_sta_record(raw, size, &records[index++]); }
    }
    *count = total;
    return STM_OK;
}
stm_err_t eh_wifi_ap_get_sta_aid(esp_hosted_handle_t h, const uint8_t mac[6],
                                uint16_t *aid, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !mac || !aid || !timeout_ms || !valid_client_mac(mac)) { return STM_ERR_INVALID_ARG; }
    if (!ap_active(h)) { return STM_ERR_INVALID_STATE; }
    uint8_t body[8], response[512]; size_t len = sizeof(response);
    size_t n = put_bytes(body, 1U, mac, 6U);
    stm_err_t err = request(h, 312U, body, n, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *bytes = NULL; size_t size = 0U; uint64_t parsed = 0U;
    if (field(response, len, 2U, 0U, &bytes, &size, &parsed) < 0 || parsed < 1U || parsed > 2007U) {
        return STM_ERR_VERIFY;
    }
    *aid = (uint16_t)parsed;
    return STM_OK;
}
stm_err_t eh_wifi_deauth_sta(esp_hosted_handle_t h, uint16_t aid, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms || aid < 1U || aid > 2007U) { return STM_ERR_INVALID_ARG; }
    if (!ap_active(h)) { return STM_ERR_INVALID_STATE; }
    uint8_t body[4]; size_t n = put_num(body, 1U, aid);
    return request(h, 293U, body, n, NULL, NULL, timeout_ms);
}
stm_err_t eh_wifi_scan_stop(esp_hosted_handle_t h, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !timeout_ms) { return STM_ERR_INVALID_ARG; }
    if (!h->wifi_started || !(h->wifi_mode & EH_WIFI_MODE_STA) || !h->scan_pending) {
        return STM_ERR_INVALID_STATE;
    }
    stm_err_t err = request(h, 287U, NULL, 0U, NULL, NULL, timeout_ms);
    if (err == STM_OK) { h->scan_pending = h->scan_done = 0U; }
    return err;
}
stm_err_t eh_wifi_scan_get_results(esp_hosted_handle_t h, eh_wifi_ap_record_t *records,
                                   size_t capacity, size_t *count, uint32_t timeout_ms)
{
    if (h && h->callback_depth) { return STM_ERR_INVALID_CONTEXT; }
    if (h && (h->recovery_phase || h->request_active)) { return STM_ERR_INVALID_STATE; }

    if (!h || !count || !timeout_ms || (capacity && !records)) { return STM_ERR_INVALID_ARG; }
    *count = 0U;
    if (!h->scan_done || h->scan_pending) { return STM_ERR_INVALID_STATE; }
    uint8_t response[32]; size_t len = sizeof(response);
    stm_err_t err = request(h, 288U, NULL, 0U, response, &len, timeout_ms);
    if (err != STM_OK) { return err; }
    const uint8_t *data = NULL; size_t size = 0U; uint64_t available = 0U;
    if (field(response, len, 2U, 0U, &data, &size, &available) < 0 ||
        available > UINT16_MAX) { return STM_ERR_VERIFY; }
    /* The CP's get-records RPC consumes its scan list. A caller must supply room
     * for every record; return the required capacity without touching the list. */
    if (available > capacity) { *count = (size_t)available; return STM_ERR_OUT_OF_RANGE; }
    size_t total = (size_t)available;
    for (size_t i = 0; i < total; ++i) {
        uint8_t record[512]; len = sizeof(record);
        err = request(h, 351U, NULL, 0U, record, &len, timeout_ms);
        if (err != STM_OK) { return err; }
        if (field(record, len, 2U, 2U, &data, &size, &available) != 1) { return STM_ERR_VERIFY; }
        err = parse_ap_record(data, size, &records[i]);
        if (err != STM_OK) { return err; }
        ++*count;
    }
    h->scan_done = 0U;
    return STM_OK;
}

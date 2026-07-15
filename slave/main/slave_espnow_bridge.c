/*
 * SPDX-FileCopyrightText: 2026 Tactility
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slave-side ESP-NOW bridge: forwards ESP-NOW init/peer/send requests from
 * the host (over esp_hosted's custom RPC channel) to the real esp_now_*
 * API, and pushes RX/send-status events back to the host the same way.
 *
 * Depends on WiFi already being brought up by the existing slave_wifi_std.c
 * path (esp_wifi_init/mode/start) — this file only touches esp_now_*.
 */

#include "sdkconfig.h"

#ifdef CONFIG_ESP_HOSTED_ESPNOW_BRIDGE

#include <string.h>
#include <inttypes.h>
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_hosted_peer_data.h"
#include "esp_hosted_espnow_bridge_proto.h"
#include "slave_espnow_bridge.h"

static const char *TAG = "espnow_bridge";

static bool espnow_bridge_initialized = false;

static void send_status_resp(uint32_t resp_msg_id, esp_err_t err)
{
    espnow_bridge_resp_status_t resp = { .esp_err = (int32_t)err };
    esp_err_t ret = esp_hosted_send_custom_data(resp_msg_id, (const uint8_t *)&resp, sizeof(resp));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send resp 0x%" PRIx32 ": %d", resp_msg_id, ret);
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int data_len)
{
    if (data_len < 0 || data_len > ESPNOW_BRIDGE_MAX_DATA_LEN) {
        ESP_LOGW(TAG, "Dropping oversized/invalid RX (%d bytes)", data_len);
        return;
    }

    espnow_bridge_evt_recv_t evt = {0};
    memcpy(evt.src_addr, info->src_addr, ESPNOW_BRIDGE_ETH_ALEN);
    memcpy(evt.des_addr, info->des_addr, ESPNOW_BRIDGE_ETH_ALEN);
    evt.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    evt.channel = info->rx_ctrl ? info->rx_ctrl->channel : 0;
    evt.data_len = (uint16_t)data_len;
    memcpy(evt.data, data, data_len);

    esp_err_t ret = esp_hosted_send_custom_data(ESPNOW_BRIDGE_EVT_RECV, (const uint8_t *)&evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to forward RX event: %d", ret);
    }
}

static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    espnow_bridge_evt_send_status_t evt = {0};
    if (tx_info && tx_info->des_addr) {
        memcpy(evt.peer_addr, tx_info->des_addr, ESPNOW_BRIDGE_ETH_ALEN);
    }
    evt.success = (status == ESP_NOW_SEND_SUCCESS);

    esp_err_t ret = esp_hosted_send_custom_data(ESPNOW_BRIDGE_EVT_SEND_STATUS, (const uint8_t *)&evt, sizeof(evt));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to forward send-status event: %d", ret);
    }
}

/**
 * ESP-NOW requires the underlying WiFi driver to already be initialized (station/AP mode
 * configured) - it isn't standalone. On this bridge, WiFi is normally only brought up on the
 * slave when the host's own WiFi gets enabled (via the real Req_WifiInit RPC path in
 * slave_wifi_std.c). If a host never enables WiFi (e.g. ESP-NOW-only usage, matching how ESP-NOW
 * works standalone on native ESP32 chips), esp_now_init() would be called against an
 * uninitialized WiFi driver and crash. Bring up minimal WiFi ourselves if needed.
 *
 * esp_wifi_init() is safe to call here even if the host's real WiFi path initializes it later
 * (or already has) - slave_wifi_std.c wraps esp_wifi_init() (-Wl,--wrap=esp_wifi_init) and
 * treats a call with the same config as a no-op returning ESP_OK.
 */
static esp_err_t ensure_wifi_ready(wifi_mode_t mode)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init() failed: %d", ret);
        return ret;
    }

    ret = esp_wifi_set_mode(mode);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode() failed: %d", ret);
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STOPPED) {
        ESP_LOGE(TAG, "esp_wifi_start() failed: %d", ret);
        return ret;
    }

    return ESP_OK;
}

static void send_init_resp(esp_err_t err)
{
    espnow_bridge_resp_init_t resp = { .esp_err = (int32_t)err, .espnow_version = 0 };
    if (err == ESP_OK) {
        uint32_t version = 0;
        if (esp_now_get_version(&version) == ESP_OK) {
            resp.espnow_version = version;
        } else {
            ESP_LOGW(TAG, "esp_now_get_version() failed - reporting version 0 to host");
        }
    }
    esp_err_t ret = esp_hosted_send_custom_data(ESPNOW_BRIDGE_RESP_INIT, (const uint8_t *)&resp, sizeof(resp));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send RESP_INIT: %d", ret);
    }
}

static void on_req_init(uint32_t msg_id, const uint8_t *data, size_t data_len, void *ctx)
{
    (void)msg_id; (void)ctx;
    if (data_len != sizeof(espnow_bridge_req_init_t)) {
        send_init_resp(ESP_ERR_INVALID_SIZE);
        return;
    }
    const espnow_bridge_req_init_t *req = (const espnow_bridge_req_init_t *)data;
    wifi_mode_t wifi_mode = (req->mode == ESPNOW_BRIDGE_MODE_ACCESS_POINT) ? WIFI_MODE_AP : WIFI_MODE_STA;

    esp_err_t ret = ensure_wifi_ready(wifi_mode);

    /* An unassociated STA's operating channel is otherwise left undefined/wherever it last
     * scanned to - ESP-NOW then silently fails to reach peers on a different channel even though
     * esp_now_send() reports success. Only skip this when actually connected to an AP (station
     * mode already locked to the AP's channel; forcing a different one would disrupt that link). */
    if (ret == ESP_OK && req->channel != 0 && wifi_mode == WIFI_MODE_STA) {
        wifi_ap_record_t ap_info;
        bool is_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
        if (!is_connected) {
            esp_err_t chan_ret = esp_wifi_set_channel(req->channel, WIFI_SECOND_CHAN_NONE);
            if (chan_ret != ESP_OK) {
                ESP_LOGW(TAG, "esp_wifi_set_channel() failed: %d - ESP-NOW may not reach peers", chan_ret);
            }
        }
    }

    if (ret == ESP_OK) {
        ret = esp_now_init();
    }
    if (ret == ESP_OK) {
        ret = esp_now_register_recv_cb(espnow_recv_cb);
    }
    if (ret == ESP_OK) {
        ret = esp_now_register_send_cb(espnow_send_cb);
    }
    if (ret == ESP_OK) {
        ret = esp_now_set_pmk(req->pmk);
    }
    if (ret == ESP_OK) {
        espnow_bridge_initialized = true;
    }
    send_init_resp(ret);
}

static void on_req_deinit(uint32_t msg_id, const uint8_t *data, size_t data_len, void *ctx)
{
    (void)msg_id; (void)data; (void)data_len; (void)ctx;
    esp_err_t ret = esp_now_deinit();
    espnow_bridge_initialized = false;
    send_status_resp(ESPNOW_BRIDGE_RESP_DEINIT, ret);
}

static void on_req_add_peer(uint32_t msg_id, const uint8_t *data, size_t data_len, void *ctx)
{
    (void)msg_id; (void)ctx;
    if (data_len != sizeof(espnow_bridge_req_add_peer_t)) {
        send_status_resp(ESPNOW_BRIDGE_RESP_ADD_PEER, ESP_ERR_INVALID_SIZE);
        return;
    }
    const espnow_bridge_req_add_peer_t *req = (const espnow_bridge_req_add_peer_t *)data;

    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, req->peer_addr, ESPNOW_BRIDGE_ETH_ALEN);
    memcpy(peer.lmk, req->lmk, ESPNOW_BRIDGE_KEY_LEN);
    peer.channel = req->channel;
    peer.ifidx = (req->ifidx == ESPNOW_BRIDGE_MODE_ACCESS_POINT) ? WIFI_IF_AP : WIFI_IF_STA;
    peer.encrypt = req->encrypt;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_ERR_ESPNOW_EXIST) {
        ret = esp_now_mod_peer(&peer);
    }
    send_status_resp(ESPNOW_BRIDGE_RESP_ADD_PEER, ret);
}

static void on_req_send(uint32_t msg_id, const uint8_t *data, size_t data_len, void *ctx)
{
    (void)msg_id; (void)ctx;
    if (data_len != sizeof(espnow_bridge_req_send_t)) {
        send_status_resp(ESPNOW_BRIDGE_RESP_SEND, ESP_ERR_INVALID_SIZE);
        return;
    }
    const espnow_bridge_req_send_t *req = (const espnow_bridge_req_send_t *)data;
    if (req->data_len > ESPNOW_BRIDGE_MAX_DATA_LEN) {
        send_status_resp(ESPNOW_BRIDGE_RESP_SEND, ESP_ERR_INVALID_SIZE);
        return;
    }

    esp_err_t ret = esp_now_send(req->broadcast ? NULL : req->dest_addr, req->data, req->data_len);
    send_status_resp(ESPNOW_BRIDGE_RESP_SEND, ret);
}

esp_err_t slave_espnow_bridge_init(void)
{
    esp_err_t ret;

    ret = esp_hosted_register_custom_callback(ESPNOW_BRIDGE_REQ_INIT, on_req_init, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register REQ_INIT handler: %d", ret);
        return ret;
    }
    ret = esp_hosted_register_custom_callback(ESPNOW_BRIDGE_REQ_DEINIT, on_req_deinit, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register REQ_DEINIT handler: %d", ret);
        return ret;
    }
    ret = esp_hosted_register_custom_callback(ESPNOW_BRIDGE_REQ_ADD_PEER, on_req_add_peer, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register REQ_ADD_PEER handler: %d", ret);
        return ret;
    }
    ret = esp_hosted_register_custom_callback(ESPNOW_BRIDGE_REQ_SEND, on_req_send, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register REQ_SEND handler: %d", ret);
        return ret;
    }

    ESP_LOGI(TAG, "ESP-NOW bridge handlers registered");
    return ESP_OK;
}

#endif /* CONFIG_ESP_HOSTED_ESPNOW_BRIDGE */

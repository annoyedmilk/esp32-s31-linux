// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_private/wifi.h"
#include "heap_memory_layout.h"
#include "soc/soc.h"
#include "soc/hp_system_reg.h"
#include "soc/interrupts.h"
#include "esp32s31-wifi-ipc.h"
#include "loader.h"

/*
 * The ring window is outside the ESP-IDF heap because this firmware stays in
 * memory.  The harts read SRAM without the data cache, so both harts can use
 * it at the same time.
 */
SOC_RESERVE_MEMORY_REGION(ESP32S31_IPC_SRAM_ADDR,
                          ESP32S31_IPC_SRAM_ADDR + ESP32S31_IPC_SRAM_SIZE,
                          wifi_ipc);

_Static_assert(sizeof(struct esp32s31_ipc) <= ESP32S31_IPC_SRAM_SIZE,
               "Wi-Fi IPC block exceeds its reserved SRAM window");

#define IPC_DOORBELL_TO_LINUX_REG    HP_SYSTEM_CPU_INT_FROM_CPU_0_REG
#define IPC_DOORBELL_TO_FIRMWARE     ETS_CPU_INTR_FROM_CPU_1_SOURCE
#define IPC_DOORBELL_TO_FIRMWARE_REG HP_SYSTEM_CPU_INT_FROM_CPU_1_REG

static const char *TAG = "s31-linux-wifi";

static struct esp32s31_ipc *const ipc =
    (struct esp32s31_ipc *)ESP32S31_IPC_SRAM_ADDR;

static bool ipc_ring_pop(struct esp32s31_ipc_ring *ring, void *buf, uint32_t *len)
{
    uint32_t tail = ring->tail;
    struct esp32s31_ipc_slot *slot;

    if (tail == ring->head) {
        return false;
    }

    slot = &ring->slot[tail % ESP32S31_IPC_SLOTS];
    *len = slot->len;
    if (*len > ESP32S31_IPC_SLOT_DATA) {
        *len = ESP32S31_IPC_SLOT_DATA;
    }
    memcpy(buf, slot->data, *len);

    __atomic_store_n(&ring->tail, tail + 1, __ATOMIC_RELEASE);
    return true;
}

static bool ipc_ring_push(struct esp32s31_ipc_ring *ring, const void *buf,
                          uint32_t len)
{
    uint32_t head = ring->head;
    struct esp32s31_ipc_slot *slot;

    if (len > ESP32S31_IPC_SLOT_DATA ||
        head - __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE) >=
        ESP32S31_IPC_SLOTS) {
        return false;
    }

    slot = &ring->slot[head % ESP32S31_IPC_SLOTS];
    memcpy(slot->data, buf, len);
    slot->len = len;

    __atomic_store_n(&ring->head, head + 1, __ATOMIC_RELEASE);
    return true;
}

/* Received frames: give them to Linux and free the Wi-Fi buffer. */
static esp_err_t ipc_wifi_rx(void *buffer, uint16_t len, void *eb)
{
    bool queued = ipc_ring_push(&ipc->to_linux, buffer, len);

    if (eb) {
        esp_wifi_internal_free_rx_buffer(eb);
    }
    if (queued) {
        REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
    }
    return ESP_OK;
}

static TaskHandle_t ipc_tx_task_handle;

/* esp_wifi_internal_tx() can block, so the doorbell only wakes the task. */
static void ipc_from_linux_isr(void *arg)
{
    BaseType_t higher_priority_woken = pdFALSE;

    REG_WRITE(IPC_DOORBELL_TO_FIRMWARE_REG, 0);
    vTaskNotifyGiveFromISR(ipc_tx_task_handle, &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

/* Try to associate again only while Linux wants a connection. */
static bool ipc_want_connection;

static void ipc_publish_scan(uint32_t count);

static void ipc_run_command(void)
{
    wifi_config_t cfg = { 0 };
    esp_err_t err;
    uint32_t code;

    code = __atomic_exchange_n(&ipc->cmd.code, ESP32S31_IPC_CMD_NONE,
                               __ATOMIC_ACQUIRE);

    switch (code) {
    case ESP32S31_IPC_CMD_CONNECT:
        memcpy(cfg.sta.ssid, ipc->cmd.ssid, sizeof(cfg.sta.ssid));
        memcpy(cfg.sta.password, ipc->cmd.psk, sizeof(cfg.sta.password));

        /* Set the auth mode that the passphrase shows.  Otherwise esp_wifi
         * finds it and logs a warning.
         */
        cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK
                                                         : WIFI_AUTH_OPEN;

        /*
         * A station that associates does not accept a new config.  Stop the
         * old attempt first and wait for it to stop.
         */
        ipc_want_connection = false;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));

        ESP_LOGI(TAG, "connecting to \"%s\"", (const char *)cfg.sta.ssid);
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "set config failed: %s", esp_err_to_name(err));
            break;
        }
        ipc_want_connection = true;
        esp_wifi_connect();
        break;
    case ESP32S31_IPC_CMD_DISCONNECT:
        ipc_want_connection = false;
        esp_wifi_disconnect();
        break;
    case ESP32S31_IPC_CMD_SCAN: {
        /* Asynchronous: the results come in the scan-done event.  Thus the
         * transmit path does not wait for the seconds that a scan takes. */
        wifi_scan_config_t scan = { .show_hidden = false };

        err = esp_wifi_scan_start(&scan, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
            /* Publish an empty result, so that Linux does not wait. */
            ipc_publish_scan(0);
        }
        break;
    }
    }
}

static void ipc_tx_task(void *arg)
{
    static uint8_t frame[ESP32S31_IPC_SLOT_DATA];
    uint32_t len;
    bool drained;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ipc_run_command();
        drained = false;
        while (ipc_ring_pop(&ipc->to_firmware, frame, &len)) {
            esp_wifi_internal_tx(WIFI_IF_STA, frame, len);
            drained = true;
        }
        /* Free slots tell Linux that the transmission is complete. */
        if (drained) {
            REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
        }
    }
}

/* Write count, then seq: Linux reads seq to know that the table is valid. */
static void ipc_publish_scan(uint32_t count)
{
    ipc->scan.count = count;
    __atomic_store_n(&ipc->scan.seq, ipc->scan.seq + 1, __ATOMIC_RELEASE);
    REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
}

static void ipc_collect_scan(void)
{
    static wifi_ap_record_t records[ESP32S31_IPC_SCAN_MAX];
    uint16_t num = ESP32S31_IPC_SCAN_MAX;
    uint16_t i;

    if (esp_wifi_scan_get_ap_records(&num, records) != ESP_OK) {
        num = 0;
    }

    for (i = 0; i < num; i++) {
        struct esp32s31_ipc_bss *bss = &ipc->scan.bss[i];
        size_t len = strnlen((const char *)records[i].ssid,
                             ESP32S31_IPC_SSID_MAX);

        memset(bss, 0, sizeof(*bss));
        memcpy(bss->bssid, records[i].bssid, sizeof(bss->bssid));
        memcpy(bss->ssid, records[i].ssid, len);
        bss->ssid_len = len;
        bss->channel = records[i].primary;
        bss->rssi = records[i].rssi;
        bss->authmode = records[i].authmode == WIFI_AUTH_OPEN ?
                        ESP32S31_IPC_AUTH_OPEN : ESP32S31_IPC_AUTH_SECURED;
    }

    ipc_publish_scan(num);
}

static void ipc_set_link(uint32_t up)
{
    __atomic_store_n(&ipc->link_up, up, __ATOMIC_RELEASE);
    REG_WRITE(IPC_DOORBELL_TO_LINUX_REG, 1);
}

static void ipc_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                           void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }

    switch (id) {
    case WIFI_EVENT_SCAN_DONE:
        ipc_collect_scan();
        break;
    case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t *ev = data;

        memcpy((void *)ipc->bssid, ev->bssid, sizeof(ipc->bssid));
        ipc->channel = ev->channel;
        ESP_LOGI(TAG, "associated");
        ipc_set_link(1);
        break;
    }
    case WIFI_EVENT_STA_DISCONNECTED:
        ipc_set_link(0);
        if (ipc_want_connection) {
            esp_wifi_connect();
        }
        break;
    }
}

static esp_err_t start_ipc(void)
{
    esp_err_t err;

    memset(ipc, 0, sizeof(*ipc));
    err = esp_wifi_get_mac(WIFI_IF_STA, ipc->mac);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(ipc_tx_task, "wifi_ipc_tx", 4096, NULL, 5,
                    &ipc_tx_task_handle) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_intr_alloc(IPC_DOORBELL_TO_FIRMWARE, 0, ipc_from_linux_isr,
                         NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     ipc_wifi_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    esp_wifi_internal_reg_rxcb(WIFI_IF_STA, ipc_wifi_rx);

    ipc->version = ESP32S31_IPC_VERSION;
    __atomic_store_n(&ipc->magic, ESP32S31_IPC_MAGIC, __ATOMIC_RELEASE);

    ESP_LOGI(TAG, "wifi ipc at 0x%08" PRIx32 ", mac %02x:%02x:%02x:%02x:%02x:%02x",
             (uint32_t)ESP32S31_IPC_SRAM_ADDR, ipc->mac[0], ipc->mac[1],
             ipc->mac[2], ipc->mac[3], ipc->mac[4], ipc->mac[5]);
    return ESP_OK;
}

/*
 * The radio start reads the flash.  A flash transaction disables the cache
 * that Linux executes from.  Thus this must be complete before hart 1 starts.
 * NVS is off for the same reason: nothing must write the flash while the
 * kernel runs.  The cost is a phy_init error and a full RF calibration at each
 * boot.
 */
void start_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    cfg.nvs_enable = false;

    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        err = esp_wifi_init(&cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        err = start_ipc();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(err));
    }
}

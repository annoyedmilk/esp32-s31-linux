// SPDX-License-Identifier: Apache-2.0
/*
 * ESP-Hosted transport for the ESP32-S31 when the Linux host runs on hart 1
 * of the same chip.  The frames go through two byte rings in internal SRAM,
 * and the cross-core interrupts are the doorbells.  The frame format is the
 * one of the USB transport.  shared/esp32s31-hosted-shm.h has the layout.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <rom/rtc.h>
#include "endian.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_fw_version.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "heap_memory_layout.h"
#include "soc/hp_system_reg.h"
#include "soc/interrupts.h"
#include "soc/soc.h"
#include "interface.h"
#include "esp32s31-hosted-shm.h"
#include "shm_nvmem.h"

/*
 * The largest frames.  To the host: the same as on USB.  To the firmware: the
 * RX buffer size of the boot event, which the host checks against the USB
 * rules (a multiple of 512, at most 16 KiB).
 */
#define SHM_TO_HOST_FRAME_MAX	(16 * 1024)
#define SHM_TO_FW_FRAME_MAX	(8 * 1024)
#define SHM_TX_TIMEOUT_MS	1000

#define DOORBELL_TO_HOST_REG	HP_SYSTEM_CPU_INT_FROM_CPU_2_REG
#define DOORBELL_FROM_HOST_REG	HP_SYSTEM_CPU_INT_FROM_CPU_1_REG
#define DOORBELL_FROM_HOST_SRC	ETS_CPU_INTR_FROM_CPU_1_SOURCE

/* The block is outside the heap: Linux uses it after the loader is done. */
SOC_RESERVE_MEMORY_REGION(ESP_SHM_ADDR, ESP_SHM_ADDR + ESP_SHM_SIZE, hosted_shm);

_Static_assert(ESP_SHM_TO_HOST_SIZE >= ESP_SHM_RECORD_LEN(SHM_TO_HOST_FRAME_MAX),
	       "the ring to the host must hold a maximum frame");
_Static_assert(ESP_SHM_TO_FW_SIZE >= ESP_SHM_RECORD_LEN(SHM_TO_FW_FRAME_MAX),
	       "the ring to the firmware must hold a maximum frame");

struct shm_ring {
	uint32_t ctrl;
	uint32_t data;
	uint32_t size;
};

static const struct shm_ring to_host = {
	ESP_SHM_TO_HOST_CTRL, ESP_SHM_TO_HOST_DATA, ESP_SHM_TO_HOST_SIZE
};
static const struct shm_ring to_fw = {
	ESP_SHM_TO_FW_CTRL, ESP_SHM_TO_FW_DATA, ESP_SHM_TO_FW_SIZE
};

static const char *TAG = "FW_SHM";

static interface_context_t s_context;
static interface_handle_t s_handle;
static intr_handle_t s_intr;
/* The host doorbell means new frames or free space, so it gives both. */
static SemaphoreHandle_t s_rx_sem;
static SemaphoreHandle_t s_space_sem;
static uint32_t s_boot_cap;
static uint32_t s_boot_ext_cap;
static bool s_boot_cap_valid;

static uint32_t *shm_word(uint32_t off)
{
	return (uint32_t *)(ESP_SHM_ADDR + off);
}

static uint32_t shm_load(uint32_t off)
{
	return __atomic_load_n(shm_word(off), __ATOMIC_ACQUIRE);
}

static void shm_store(uint32_t off, uint32_t val)
{
	__atomic_store_n(shm_word(off), val, __ATOMIC_RELEASE);
}

/*
 * Order a store before a later load.  Each side stores its own index or the
 * wait flag, then loads the index of the other side.  Without the fence, the
 * two sides can each miss the store of the other, and no doorbell comes.
 */
static void shm_fence(void)
{
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void ring_copy_in(const struct shm_ring *r, uint32_t pos,
			 const void *src, uint32_t len)
{
	uint32_t off = pos & (r->size - 1);
	uint32_t first = len < r->size - off ? len : r->size - off;
	uint8_t *data = (uint8_t *)(ESP_SHM_ADDR + r->data);

	memcpy(data + off, src, first);
	memcpy(data, (const uint8_t *)src + first, len - first);
}

static void ring_copy_out(const struct shm_ring *r, uint32_t pos,
			  void *dst, uint32_t len)
{
	uint32_t off = pos & (r->size - 1);
	uint32_t first = len < r->size - off ? len : r->size - off;
	const uint8_t *data = (const uint8_t *)(ESP_SHM_ADDR + r->data);

	memcpy(dst, data + off, first);
	memcpy((uint8_t *)dst + first, data, len - first);
}

/*
 * Add one frame: the header and the payload.  Returns false when it is full.
 * Sets *kick when the consumer had read all frames: then it can be idle and
 * needs the doorbell.  Else it reads this frame before it stops.
 */
static bool ring_push(const struct shm_ring *r, const void *hdr,
		      uint32_t hdr_len, const void *payload, uint32_t len,
		      bool *kick)
{
	uint32_t head = *shm_word(r->ctrl + ESP_SHM_HEAD_OFF);
	uint32_t tail = shm_load(r->ctrl + ESP_SHM_TAIL_OFF);
	uint32_t frame_len = hdr_len + len;
	uint32_t need = ESP_SHM_RECORD_LEN(frame_len);

	if (need > r->size - (head - tail))
		return false;

	ring_copy_in(r, head + 4, hdr, hdr_len);
	ring_copy_in(r, head + 4 + hdr_len, payload, len);
	ring_copy_in(r, head, &frame_len, 4);
	shm_store(r->ctrl + ESP_SHM_HEAD_OFF, head + need);
	shm_fence();
	*kick = shm_load(r->ctrl + ESP_SHM_TAIL_OFF) == head;
	return true;
}

static void IRAM_ATTR shm_doorbell_isr(void *arg)
{
	BaseType_t woken = pdFALSE;

	(void)arg;
	REG_WRITE(DOORBELL_FROM_HOST_REG, 0);
	xSemaphoreGiveFromISR(s_rx_sem, &woken);
	xSemaphoreGiveFromISR(s_space_sem, &woken);
	portYIELD_FROM_ISR(woken);
}

static void shm_free_frame(void *buf)
{
	heap_caps_free(buf);
}

/*
 * The host asks for a boot event when it opens or closes the data path.  On
 * USB, close is a soft reset: the firmware calls esp_restart().  Here that
 * restarts the full chip, also Linux on hart 1.  Thus the firmware only sends
 * the boot event again.  The host starts a new session, but the Wi-Fi state
 * of the firmware stays.  The fatal error paths of the firmware still call
 * esp_restart(), and that restarts Linux too.
 */
static void shm_service_host_requests(void)
{
	uint32_t req = __atomic_exchange_n(shm_word(ESP_SHM_CTRL + ESP_SHM_HOST_REQ_OFF),
					   0, __ATOMIC_ACQ_REL);

	if ((req & ESP_SHM_REQ_READY_REPLAY) && s_boot_cap_valid &&
	    send_bootup_event_to_host(s_boot_cap, s_boot_ext_cap) != ESP_OK)
		ESP_LOGW(TAG, "READY replay enqueue failed");
	shm_nvmem_service();
	/* Linux is off.  Without a wake-up source, only a reset starts the chip. */
	if (req & ESP_SHM_REQ_POWEROFF) {
		esp_wifi_stop();
		esp_deep_sleep_start();
	}
}

static interface_handle_t *shm_init(void)
{
	s_rx_sem = xSemaphoreCreateBinary();
	s_space_sem = xSemaphoreCreateBinary();
	if (!s_rx_sem || !s_space_sem)
		goto fail;

	memset((void *)ESP_SHM_ADDR, 0, ESP_SHM_SIZE);
	shm_nvmem_init();
	shm_store(ESP_SHM_CTRL + ESP_SHM_VERSION_OFF, ESP_SHM_VERSION);
	shm_store(ESP_SHM_CTRL + ESP_SHM_MAGIC_OFF, ESP_SHM_MAGIC);

	if (esp_intr_alloc(DOORBELL_FROM_HOST_SRC, 0, shm_doorbell_isr, NULL,
			   &s_intr) != ESP_OK)
		goto fail;

	s_handle.state = INIT;
	ESP_LOGI(TAG, "shared-memory transport at 0x%08x", ESP_SHM_ADDR);
	return &s_handle;

fail:
	if (s_rx_sem)
		vSemaphoreDelete(s_rx_sem);
	if (s_space_sem)
		vSemaphoreDelete(s_space_sem);
	s_rx_sem = s_space_sem = NULL;
	ESP_LOGE(TAG, "shared-memory transport init failed");
	return NULL;
}

static int32_t shm_write(interface_handle_t *handle,
			 interface_buffer_handle_t *buf_handle)
{
	struct esp_payload_header hdr = { 0 };
	TickType_t start = xTaskGetTickCount();
	bool kick;

	if (!handle || !buf_handle || !buf_handle->payload ||
	    !buf_handle->payload_len ||
	    sizeof(hdr) + buf_handle->payload_len > SHM_TO_HOST_FRAME_MAX)
		return ESP_FAIL;

	hdr.if_type = buf_handle->if_type;
	hdr.if_num = buf_handle->if_num;
	hdr.flags = buf_handle->flag;
	hdr.packet_type = buf_handle->pkt_type;
	hdr.len = htole16(buf_handle->payload_len);
	hdr.offset = htole16(sizeof(hdr));

	/*
	 * Ask for a doorbell before the second try, so that space which the
	 * host frees in between is not missed.
	 */
	while (!ring_push(&to_host, &hdr, sizeof(hdr), buf_handle->payload,
			  buf_handle->payload_len, &kick)) {
		shm_store(to_host.ctrl + ESP_SHM_WAIT_OFF, 1);
		shm_fence();
		if (ring_push(&to_host, &hdr, sizeof(hdr), buf_handle->payload,
			      buf_handle->payload_len, &kick))
			break;
		if (xTaskGetTickCount() - start > pdMS_TO_TICKS(SHM_TX_TIMEOUT_MS))
			return ESP_FAIL;
		xSemaphoreTake(s_space_sem, pdMS_TO_TICKS(SHM_TX_TIMEOUT_MS));
	}

	if (kick)
		REG_WRITE(DOORBELL_TO_HOST_REG, 1);
	return buf_handle->payload_len;
}

static int shm_read(interface_handle_t *handle,
		    interface_buffer_handle_t *buf_handle)
{
	const struct esp_payload_header *hdr;
	uint32_t head, tail, len;
	uint8_t *buf;

	if (!handle || !buf_handle)
		return ESP_FAIL;

	/*
	 * Wait for a frame.  recv_task sleeps 10 ms after each empty read, so
	 * do not return without one.  The host rings for each request.
	 */
	tail = *shm_word(to_fw.ctrl + ESP_SHM_TAIL_OFF);
	for (;;) {
		shm_service_host_requests();
		shm_fence();
		head = shm_load(to_fw.ctrl + ESP_SHM_HEAD_OFF);
		if (head != tail)
			break;
		xSemaphoreTake(s_rx_sem, portMAX_DELAY);
	}

	ring_copy_out(&to_fw, tail, &len, 4);
	if (len < sizeof(*hdr) || len > SHM_TO_FW_FRAME_MAX ||
	    ESP_SHM_RECORD_LEN(len) > head - tail) {
		/* A broken record: drop all frames, the host recovers. */
		ESP_LOGE(TAG, "bad frame length %u", (unsigned)len);
		shm_store(to_fw.ctrl + ESP_SHM_TAIL_OFF, head);
		return 0;
	}

	buf = heap_caps_malloc(len, MALLOC_CAP_8BIT);
	if (!buf)
		return 0;
	ring_copy_out(&to_fw, tail + 4, buf, len);
	shm_store(to_fw.ctrl + ESP_SHM_TAIL_OFF, tail + ESP_SHM_RECORD_LEN(len));
	shm_fence();

	if (shm_load(to_fw.ctrl + ESP_SHM_WAIT_OFF)) {
		shm_store(to_fw.ctrl + ESP_SHM_WAIT_OFF, 0);
		REG_WRITE(DOORBELL_TO_HOST_REG, 1);
	}

	hdr = (const struct esp_payload_header *)buf;
	buf_handle->if_type = hdr->if_type;
	buf_handle->if_num = hdr->if_num;
	buf_handle->pkt_type = hdr->packet_type;
	buf_handle->flag = hdr->flags;
	buf_handle->payload = buf;
	buf_handle->payload_len = len;
	buf_handle->priv_buffer_handle = buf;
	buf_handle->free_buf_handle = shm_free_frame;
	return len;
}

/* The USB transport restarts the chip here.  See shm_service_host_requests(). */
static esp_err_t shm_reset(interface_handle_t *handle)
{
	(void)handle;
	ESP_LOGW(TAG, "reset request ignored: Linux runs on this chip");
	return ESP_OK;
}

static void shm_deinit(interface_handle_t *handle)
{
	(void)handle;
	shm_store(ESP_SHM_CTRL + ESP_SHM_MAGIC_OFF, 0);
	if (s_intr)
		esp_intr_free(s_intr);
	if (s_rx_sem)
		vSemaphoreDelete(s_rx_sem);
	if (s_space_sem)
		vSemaphoreDelete(s_space_sem);
	s_intr = NULL;
	s_rx_sem = s_space_sem = NULL;
}

static if_ops_t s_if_ops = {
	.init = shm_init,
	.write = shm_write,
	.read = shm_read,
	.reset = shm_reset,
	.deinit = shm_deinit,
};

interface_context_t *interface_insert_driver(int (*event_handler)(uint8_t val))
{
	memset(&s_context, 0, sizeof(s_context));
	/* There is no transport type for shared memory.  The frames are USB frames. */
	s_context.type = USB;
	s_context.if_ops = &s_if_ops;
	s_context.event_handler = event_handler;
	ESP_LOGI(TAG, "Using the ESP32-S31 shared-memory interface");
	return &s_context;
}

int interface_remove_driver(void)
{
	shm_deinit(&s_handle);
	memset(&s_context, 0, sizeof(s_context));
	return 0;
}

/* The same boot event as the USB transport sends. */
esp_err_t send_bootup_event_to_host(uint32_t cap, uint32_t ext_cap)
{
	struct esp_internal_bootup_event *event;
	interface_buffer_handle_t buf_handle = { 0 };
	struct fw_data *fw_p;
	uint32_t val_le;
	uint16_t len = 0;
	uint8_t *pos;

	s_boot_cap = cap;
	s_boot_ext_cap = ext_cap;
	s_boot_cap_valid = true;

	buf_handle.payload = heap_caps_calloc(1, ESP_BOOTUP_EVENT_BUF_SIZE,
					      MALLOC_CAP_8BIT);
	if (!buf_handle.payload)
		return ESP_ERR_NO_MEM;
	buf_handle.priv_buffer_handle = buf_handle.payload;
	buf_handle.free_buf_handle = heap_caps_free;
	buf_handle.if_type = ESP_INTERNAL_IF;
	buf_handle.pkt_type = PACKET_TYPE_EVENT;

	event = (struct esp_internal_bootup_event *)buf_handle.payload;
	event->header.event_code = ESP_INTERNAL_BOOTUP_EVENT;
	pos = event->data;

	*pos++ = ESP_BOOTUP_FIRMWARE_CHIP_ID;
	*pos++ = LENGTH_1_BYTE;
	*pos++ = CONFIG_IDF_FIRMWARE_CHIP_ID;
	len += 3;

	*pos++ = ESP_BOOTUP_CAPABILITY;
	*pos++ = LENGTH_4_BYTE;
	val_le = htole32(cap);
	memcpy(pos, &val_le, sizeof(val_le));
	pos += sizeof(val_le);
	len += 2 + sizeof(val_le);

	if (ext_cap) {
		*pos++ = ESP_BOOTUP_EXT_CAPABILITY;
		*pos++ = LENGTH_4_BYTE;
		val_le = htole32(ext_cap);
		memcpy(pos, &val_le, sizeof(val_le));
		pos += sizeof(val_le);
		len += 2 + sizeof(val_le);
	}

	esp_hosted_add_boot_mac_tlv(&pos, &len);

	*pos++ = ESP_BOOTUP_RX_BUF_SIZE;
	*pos++ = LENGTH_4_BYTE;
	val_le = htole32(SHM_TO_FW_FRAME_MAX);
	memcpy(pos, &val_le, sizeof(val_le));
	pos += sizeof(val_le);
	len += 2 + sizeof(val_le);

	*pos++ = ESP_BOOTUP_FW_DATA;
	*pos++ = sizeof(struct fw_data);
	len += 2;
	fw_p = (struct fw_data *)pos;
	fw_p->last_reset_reason = htole32(rtc_get_reset_reason(0));
	memcpy(fw_p->version.project_name, PROJECT_NAME, strlen(PROJECT_NAME));
	fw_p->version.major1 = PROJECT_VERSION_MAJOR_1;
	fw_p->version.major2 = PROJECT_VERSION_MAJOR_2;
	fw_p->version.minor = PROJECT_VERSION_MINOR;
	fw_p->version.revision_patch_1 = PROJECT_REVISION_PATCH_1;
	fw_p->version.revision_patch_2 = PROJECT_REVISION_PATCH_2;
	len += sizeof(struct fw_data);

	event->len = len;
	event->header.len = htole16(len + 1);
	buf_handle.payload_len = len + sizeof(struct esp_internal_bootup_event);

	if (send_to_host(PRIO_Q_HIGH, &buf_handle) != pdTRUE) {
		heap_caps_free(buf_handle.payload);
		return ESP_FAIL;
	}
	return ESP_OK;
}

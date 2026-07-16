// SPDX-License-Identifier: BSD-2-Clause
// Author: Marco Müller <hello@annoyedmilk.ch>

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_sys.h"
#include "hal/axi_dma_ll.h"
#include "hal/dma_types.h"
#include "hal/lcd_ll.h"
#include "heap_memory_layout.h"
#include "soc/axi_dma_struct.h"
#include "soc/lcd_cam_struct.h"
#include "display.h"

#define LCD_FB_ADDR            0x50F40000U
#define LCD_H_RES              800U
#define LCD_V_RES              480U
#define LCD_FB_SIZE            (LCD_H_RES * LCD_V_RES * 2U)
#define LCD_PCLK_HZ            18000000U
#define LCD_DMA_LINK_ADDR      0x2F079000U
#define LCD_DMA_LINK_SIZE      0x1000U
#define LCD_DMA_CHUNK_SIZE     4032U
#define LCD_DMA_NODE_COUNT     ((LCD_FB_SIZE + LCD_DMA_CHUNK_SIZE - 1U) / LCD_DMA_CHUNK_SIZE)
#define LCD_AXI_DMA_PERIPH_ID  0
#define LCD_AXI_DMA_CHANNELS   3

_Static_assert(LCD_DMA_NODE_COUNT * sizeof(dma_descriptor_align8_t) <= LCD_DMA_LINK_SIZE,
               "LCD DMA link exceeds its reserved SRAM region");

SOC_RESERVE_MEMORY_REGION(LCD_DMA_LINK_ADDR, LCD_DMA_LINK_ADDR + LCD_DMA_LINK_SIZE, lcd_dma_link);

static const char *TAG = "s31-linux-display";

static void fill_color_bars(void)
{
    static const uint16_t colors[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    volatile uint16_t *fb = (volatile uint16_t *)LCD_FB_ADDR;

    for (uint32_t y = 0; y < LCD_V_RES; y++) {
        for (uint32_t x = 0; x < LCD_H_RES; x++) {
            fb[y * LCD_H_RES + x] = colors[x * 8U / LCD_H_RES];
        }
    }
}

static void build_dma_link(void)
{
    dma_descriptor_align8_t *link = (dma_descriptor_align8_t *)LCD_DMA_LINK_ADDR;
    uint32_t offset = 0;

    memset(link, 0, LCD_DMA_NODE_COUNT * sizeof(*link));
    for (uint32_t i = 0; i < LCD_DMA_NODE_COUNT; i++) {
        uint32_t chunk = LCD_FB_SIZE - offset;

        if (chunk > LCD_DMA_CHUNK_SIZE) {
            chunk = LCD_DMA_CHUNK_SIZE;
        }
        link[i].dw0.size = chunk;
        link[i].dw0.length = chunk;
        link[i].dw0.suc_eof = (i == LCD_DMA_NODE_COUNT - 1U);
        link[i].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
        link[i].buffer = (void *)(LCD_FB_ADDR + offset);
        link[i].next = &link[(i + 1U) % LCD_DMA_NODE_COUNT];
        offset += chunk;
    }
}

static int find_lcd_dma_channel(void)
{
    for (int channel = 0; channel < LCD_AXI_DMA_CHANNELS; channel++) {
        if (AXI_DMA.out[channel].conf.out_peri_sel.peri_out_sel_chn ==
            LCD_AXI_DMA_PERIPH_ID) {
            return channel;
        }
    }
    return -1;
}

static void silence_lcd_interrupts(int channel)
{
    PERIPH_RCC_ATOMIC() {
        lcd_ll_enable_interrupt(&LCD_CAM, LCD_LL_EVENT_RGB, false);
    }
    lcd_ll_clear_interrupt_status(&LCD_CAM, UINT32_MAX);
    axi_dma_ll_tx_enable_interrupt(&AXI_DMA, channel, UINT32_MAX, false);
    axi_dma_ll_tx_clear_interrupt_status(&AXI_DMA, channel, UINT32_MAX);
}

static void start_dma_link(int channel)
{
    lcd_ll_enable_auto_next_frame(&LCD_CAM, true);
    lcd_ll_reset(&LCD_CAM);
    lcd_ll_fifo_reset(&LCD_CAM);
    axi_dma_ll_tx_reset_channel(&AXI_DMA, channel);
    axi_dma_ll_tx_set_desc_addr(&AXI_DMA, channel, LCD_DMA_LINK_ADDR);
    axi_dma_ll_tx_start(&AXI_DMA, channel);
    esp_rom_delay_us(1);
    lcd_ll_start(&LCD_CAM);
}

bool display_init(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    int channel;
    esp_err_t err;

    fill_color_bars();

    const esp_lcd_rgb_panel_config_t config = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_pulse_width = 40,
            .hsync_back_porch = 40,
            .hsync_front_porch = 48,
            .vsync_pulse_width = 23,
            .vsync_back_porch = 32,
            .vsync_front_porch = 13,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 1,
        .user_fbs = { (void *)LCD_FB_ADDR },
        .dma_burst_size = 64,
        .hsync_gpio_num = 44,
        .vsync_gpio_num = 45,
        .de_gpio_num = 43,
        .pclk_gpio_num = 40,
        .disp_gpio_num = 38,
        .data_gpio_nums = {
            8, 9, 10, 11, 12, 13, 14, 15,
            16, 17, 18, 19, 33, 34, 35, 36,
        },
        .flags = {
            .fb_in_psram = true,
            .refresh_on_demand = true,
        },
    };

    err = esp_lcd_new_rgb_panel(&config, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        return false;
    }

    channel = find_lcd_dma_channel();
    if (channel < 0) {
        ESP_LOGE(TAG, "no AXI DMA TX channel bound to LCD_CAM");
        return false;
    }

    build_dma_link();
    silence_lcd_interrupts(channel);
    start_dma_link(channel);

    ESP_LOGI(TAG, "framebuffer live: %ux%u RGB565 fb=0x%08" PRIx32
                  " dma-link=0x%08" PRIx32 " channel=%d",
             (unsigned)LCD_H_RES, (unsigned)LCD_V_RES,
             (uint32_t)LCD_FB_ADDR, (uint32_t)LCD_DMA_LINK_ADDR, channel);
    return true;
}

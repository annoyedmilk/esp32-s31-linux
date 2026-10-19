// SPDX-License-Identifier: BSD-2-Clause
// Author: Roan Brand <brandroan@gmail.com>

#include <inttypes.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_clk_tree.h"
#include "esp_mac.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_private/gpio.h"
#include "esp_private/periph_ctrl.h"
#include "hal/emac_ll.h"
#include "soc/gpio_sig_map.h"
#include "loader.h"

/*
 * The RGMII Ethernet of the CoreBoard-1: a YT8531 PHY on GPIO8-19, MDC on
 * GPIO5, MDIO on GPIO6 and the PHY reset on GPIO7.  Hart 0 also writes the
 * clock and reset registers, so set them here, before hart 1 starts.  Linux
 * then changes only the TX clock divider.
 */
#define EMAC_MDC_GPIO       GPIO_NUM_5
#define EMAC_MDIO_GPIO      GPIO_NUM_6
#define EMAC_PHY_RST_GPIO   GPIO_NUM_7
#define EMAC_IOMUX_FUNC     2

/* 500 MHz is the first MPLL frequency that 125 MHz divides. */
#define EMAC_MPLL_HZ        500000000U
#define EMAC_TX_CLK_HZ      125000000U

/* TXD0-3, TX_CTL and TX_CLK. */
static const gpio_num_t emac_rgmii_out[] = {
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12, GPIO_NUM_13,
};

static const struct {
    gpio_num_t gpio;
    uint32_t signal;
} emac_rgmii_in[] = {
    { GPIO_NUM_14, GMAC_RX_CLK_PAD_IN_IDX },
    { GPIO_NUM_15, GMAC_PHY_RXDV_PAD_IN_IDX },
    { GPIO_NUM_16, GMAC_PHY_RXD3_PAD_IN_IDX },
    { GPIO_NUM_17, GMAC_PHY_RXD2_PAD_IN_IDX },
    { GPIO_NUM_18, GMAC_PHY_RXD1_PAD_IN_IDX },
    { GPIO_NUM_19, GMAC_PHY_RXD0_PAD_IN_IDX },
};

static const char *TAG = "s31-linux-emac";

static void init_emac_pins(void)
{
    for (size_t i = 0; i < sizeof(emac_rgmii_out) / sizeof(emac_rgmii_out[0]); i++) {
        gpio_iomux_output(emac_rgmii_out[i], EMAC_IOMUX_FUNC);
        gpio_set_pull_mode(emac_rgmii_out[i], GPIO_FLOATING);
    }
    for (size_t i = 0; i < sizeof(emac_rgmii_in) / sizeof(emac_rgmii_in[0]); i++) {
        gpio_iomux_input(emac_rgmii_in[i].gpio, EMAC_IOMUX_FUNC, emac_rgmii_in[i].signal);
        gpio_set_pull_mode(emac_rgmii_in[i].gpio, GPIO_FLOATING);
    }
    /* GPIO13-19 take their pad settings from CNNT_IO_MUX. */
    PERIPH_RCC_ATOMIC() {
        emac_ll_dedicated_pad_ctrl_enable(NULL, true);
    }

    /* MDC and MDIO have no IOMUX function. */
    gpio_set_direction(EMAC_MDC_GPIO, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(EMAC_MDC_GPIO, GMII_MDC_PAD_OUT_IDX, false, false);
    gpio_set_direction(EMAC_MDIO_GPIO, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(EMAC_MDIO_GPIO, GMII_MDO_PAD_OUT_IDX, false, false);
    esp_rom_gpio_connect_in_signal(EMAC_MDIO_GPIO, GMII_MDI_PAD_IN_IDX, false);
    gpio_set_pull_mode(EMAC_MDC_GPIO, GPIO_FLOATING);
    gpio_set_pull_mode(EMAC_MDIO_GPIO, GPIO_FLOATING);
    gpio_func_sel(EMAC_MDC_GPIO, PIN_FUNC_GPIO);
    gpio_func_sel(EMAC_MDIO_GPIO, PIN_FUNC_GPIO);
}

void init_emac(void)
{
    uint32_t mpll_hz = 0;
    uint8_t mac[6];
    esp_err_t err;

    err = esp_clk_tree_enable_src(SOC_MOD_CLK_MPLL, true);
    if (err == ESP_OK) {
        err = esp_clk_tree_src_set_freq_hz(SOC_MOD_CLK_MPLL, EMAC_MPLL_HZ, &mpll_hz);
    }
    if (err != ESP_OK || mpll_hz % EMAC_TX_CLK_HZ) {
        ESP_LOGW(TAG, "MPLL at %" PRIu32 " Hz: %s, no Ethernet", mpll_hz,
                 esp_err_to_name(err));
        return;
    }

    init_emac_pins();

    /* The GMAC takes the interface mode at its reset. */
    PERIPH_RCC_ATOMIC() {
        emac_ll_enable_bus_clock(0, true);
        emac_ll_clock_enable_rgmii(NULL);
        emac_ll_reset_register(0);
    }
    emac_ll_ref_clock_select(NULL, 0);
    emac_ll_ref_clock_div(NULL, mpll_hz / EMAC_TX_CLK_HZ - 1);
    emac_ll_ref_clock_enable(NULL, true);

    /* A chip reset leaves the PHY linked but deaf, so reset it too. */
    gpio_set_direction(EMAC_PHY_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(EMAC_PHY_RST_GPIO, 0);
    esp_rom_delay_us(10000);
    gpio_set_level(EMAC_PHY_RST_GPIO, 1);
    esp_rom_delay_us(50000);

    /* stmmac takes the address that it finds in the MAC. */
    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK) {
        emac_ll_set_addr(&EMAC_MAC, mac);
    }
    ESP_LOGI(TAG, "RGMII on GPIO5-19");
}

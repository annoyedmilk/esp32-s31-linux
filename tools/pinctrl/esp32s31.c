// SPDX-License-Identifier: MIT
/*
 * Decode and change the state of a pin from the registers of the IO MUX and
 * the GPIO matrix.
 */

#include <stdio.h>
#include <string.h>

#include "esp32s31.h"

/* The IO MUX functions F0-F4: ESP32-S31 datasheet, Table 2-3. */
static const char *const iomux_funcs[NPINS][NFUNCS] = {
	[0] = { "GPIO0", "GPIO0" },
	[1] = { "GPIO1", "GPIO1" },
	[2] = { "GPIO2", "GPIO2", NULL, "LCD_DATA19_OUT_PAD" },
	[3] = { "GPIO3", "GPIO3", NULL, "LCD_DATA20_OUT_PAD" },
	[4] = { "GPIO4", "GPIO4", NULL, "LCD_DATA21_OUT_PAD" },
	[5] = { "GPIO5", "GPIO5", NULL, "LCD_DATA22_OUT_PAD" },
	[6] = { "GPIO6", "GPIO6" },
	[7] = { "GPIO7", "GPIO7", NULL, "LCD_DATA23_OUT_PAD" },
	[8] = { "GPIO8", "GPIO8", "GMAC_PHY_TXD0_PAD", "LCD_DATA0_OUT_PAD" },
	[9] = { "SPI2_HOLD_PAD", "GPIO9", "GMAC_PHY_TXD1_PAD", "LCD_DATA1_OUT_PAD" },
	[10] = { "SPI2_CS_PAD", "GPIO10", "GMAC_PHY_TXD2_PAD", "LCD_DATA2_OUT_PAD" },
	[11] = { "SPI2_D_PAD", "GPIO11", "GMAC_PHY_TXD3_PAD", "LCD_DATA3_OUT_PAD" },
	[12] = { "SPI2_CK_PAD", "GPIO12", "GMAC_PHY_TXEN_PAD", "LCD_DATA4_OUT_PAD" },
	[13] = { "SPI2_Q_PAD", "GPIO13", "GMAC_RMII_CLK_PAD", "LCD_DATA5_OUT_PAD" },
	[14] = { "SPI2_WP_PAD", "GPIO14", "GMAC_RX_CLK_PAD", "LCD_DATA6_OUT_PAD" },
	[15] = { "SPI2_IO4_PAD", "GPIO15", "GMAC_PHY_RXDV_PAD", "LCD_DATA7_OUT_PAD" },
	[16] = { "SPI2_IO5_PAD", "GPIO16", "GMAC_PHY_RXD3_PAD", "LCD_DATA8_OUT_PAD" },
	[17] = { "SPI2_IO6_PAD", "GPIO17", "GMAC_PHY_RXD2_PAD", "LCD_DATA9_OUT_PAD" },
	[18] = { "SPI2_IO7_PAD", "GPIO18", "GMAC_PHY_RXD1_PAD", "LCD_DATA10_OUT_PAD" },
	[19] = { "SPI2_DQS_PAD", "GPIO19", "GMAC_PHY_RXD0_PAD", "LCD_DATA11_OUT_PAD" },
	[20] = { "SDIO_DATA0", "GPIO20", "SPI2_CK_PAD" },
	[21] = { "SDIO_DATA1", "GPIO21", "SPI2_D_PAD" },
	[22] = { "SDIO_DATA2", "GPIO22", "SPI2_Q_PAD" },
	[23] = { "SDIO_DATA3", "GPIO23", "SPI2_CS_PAD" },
	[24] = { "SDIO_CLK", "GPIO24", "SPI2_HOLD_PAD" },
	[25] = { "SDIO_CMD", "GPIO25", "SPI2_WP_PAD" },
	[26] = { "SPICS", "GPIO26" },
	[27] = { "SPIQ", "GPIO27" },
	[28] = { "SPIWP", "GPIO28" },
	[30] = { "SPIHD", "GPIO30" },
	[31] = { "SPICLK", "GPIO31" },
	[32] = { "SPID", "GPIO32" },
	[33] = { "GPIO33", "GPIO33", NULL, "LCD_DATA12_OUT_PAD" },
	[34] = { "GPIO34", "GPIO34", NULL, "LCD_DATA13_OUT_PAD" },
	[35] = { "GPIO35", "GPIO35", "REF_GMAC_CLK_PAD", "LCD_DATA14_OUT_PAD", "SD2_CDATA0_PAD" },
	[36] = { "GPIO36", "GPIO36", NULL, "LCD_DATA15_OUT_PAD", "SD2_CDATA1_PAD" },
	[37] = { "GPIO37", "GPIO37", NULL, "LCD_DATA16_OUT_PAD", "SD2_CDATA2_PAD" },
	[38] = { "GPIO38", "GPIO38", NULL, "LCD_DATA17_OUT_PAD", "SD2_CDATA3_PAD" },
	[39] = { "GPIO39", "GPIO39", NULL, "LCD_DATA18_OUT_PAD", "SD2_CCLK_PAD" },
	[40] = { "GPIO40", "GPIO40", NULL, "LCD_PCLK_PAD", "SD2_CCMD_PAD" },
	[42] = { "GPIO42", "GPIO42" },
	[43] = { "GPIO43", "GPIO43", NULL, "LCD_H_ENABLE_PAD" },
	[44] = { "GPIO44", "GPIO44", NULL, "LCD_H_SYNC_PAD" },
	[45] = { "GPIO45", "GPIO45", NULL, "LCD_V_SYNC_PAD" },
	[46] = { "GPIO46", "GPIO46", NULL, "CAM_DATA0_IN_PAD" },
	[47] = { "GPIO47", "GPIO47", NULL, "CAM_DATA1_IN_PAD" },
	[48] = { "GPIO48", "GPIO48", NULL, "CAM_DATA2_IN_PAD" },
	[49] = { "GPIO49", "GPIO49", NULL, "CAM_DATA3_IN_PAD" },
	[50] = { "GPIO50", "GPIO50", NULL, "CAM_DATA4_IN_PAD" },
	[51] = { "GPIO51", "GPIO51", NULL, "CAM_DATA5_IN_PAD" },
	[52] = { "GPIO52", "GPIO52", "SPI2_CS_PAD", "CAM_DATA6_IN_PAD" },
	[53] = { "GPIO53", "GPIO53", "SPI2_CK_PAD", "CAM_DATA7_IN_PAD" },
	[54] = { "MTDO", "GPIO54", "SPI2_D_PAD", "CAM_PCLK_PAD" },
	[55] = { "MTCK", "GPIO55", "SPI2_Q_PAD", "CAM_XCLK_PAD" },
	[56] = { "MTDI", "GPIO56", "SPI2_HOLD_PAD", "CAM_V_SYNC_PAD" },
	[57] = { "MTMS", "GPIO57", "SPI2_WP_PAD", "CAM_H_SYNC_PAD" },
	[58] = { "UART0_TXD_PAD", "GPIO58" },
	[59] = { "UART0_RXD_PAD", "GPIO59" },
	[60] = { "GPIO60", "GPIO60" },
	[61] = { "GPIO61", "GPIO61" },
};

static const unsigned int drive_ma[] = { 5, 10, 20, 40 };

/*
 * The CNNT pads: their IO MUX function also needs a select bit.  The
 * datasheet has no F4 name for the SD host, so the block names it.
 */
static const struct {
	unsigned int first, last, func;
	uint32_t sel;
	const char *block;
} cnnt_pads[] = {
	{ 13, 19, 2, 1u << 1, "GMAC" },
	{ 20, 25, 4, 1u << 0, "SDHOST" },
};

int pin_exists(unsigned int pin)
{
	return pin < NPINS && iomux_funcs[pin][1] != NULL;
}

int pin_is_flash(unsigned int pin)
{
	return pin >= 26 && pin <= 32;
}

int pin_is_strapping(unsigned int pin)
{
	return pin == 36 || pin == 37 || pin == 60 || pin == 61;
}

const char *iomux_func_name(unsigned int pin, unsigned int func)
{
	if (pin >= NPINS || func >= NFUNCS)
		return NULL;
	return iomux_funcs[pin][func];
}

/* Leave out the suffix that every pad signal has, in each part of "a/b". */
void short_name(const char *raw, char *buf, unsigned int len)
{
	static const char *const suffixes[] = { "_PAD_IN", "_PAD_OUT", "_PAD" };
	unsigned int n = 0;

	buf[0] = '\0';
	while (*raw && n + 1 < len) {
		unsigned int part = strcspn(raw, "/");
		unsigned int i;

		for (i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
			unsigned int sl = strlen(suffixes[i]);

			if (part > sl && !strncmp(raw + part - sl, suffixes[i], sl)) {
				part -= sl;
				break;
			}
		}
		n += snprintf(buf + n, len - n, "%s%.*s", n ? "/" : "", (int)part, raw);
		raw += strcspn(raw, "/");
		raw += *raw == '/';
	}
}

static uint32_t gpio_rd(struct regs *r, unsigned int off)
{
	return r->gpio[off / 4];
}

static void gpio_wr(struct regs *r, unsigned int off, uint32_t val)
{
	r->gpio[off / 4] = val;
}

/* Bank 1 has pins 32 and higher, at a fixed offset from bank 0. */
static unsigned int bank(unsigned int pin, unsigned int reg0, unsigned int reg1)
{
	return pin < 32 ? reg0 : reg1;
}

static int gpio_bit(struct regs *r, unsigned int pin, unsigned int reg0,
		    unsigned int reg1)
{
	return gpio_rd(r, bank(pin, reg0, reg1)) >> (pin % 32) & 1;
}

static void gpio_w1(struct regs *r, unsigned int pin, unsigned int reg0,
		    unsigned int reg1, int set)
{
	r->w1(r, bank(pin, reg0, reg1), 1u << (pin % 32), set);
}

int pin_level(struct regs *r, unsigned int pin)
{
	if (!(r->iomux[pin] & IOMUX_IE))
		return -1;
	return gpio_bit(r, pin, GPIO_IN, GPIO_IN1);
}

/* Add a name to a comma-separated list, once. */
static void append(char *buf, unsigned int len, const char *raw)
{
	char name[96];
	unsigned int n = strlen(buf), nl;
	const char *p;

	short_name(raw ? raw : "-", name, sizeof(name));
	nl = strlen(name);
	for (p = buf; *p; p += strcspn(p, ","), p += *p == ',')
		if (strcspn(p, ",") == nl && !strncmp(p, name, nl))
			return;
	snprintf(buf + n, len - n, "%s%s", n ? "," : "", name);
}

static const char *cnnt_block(struct regs *r, unsigned int pin, unsigned int func)
{
	unsigned int i;

	for (i = 0; i < sizeof(cnnt_pads) / sizeof(cnnt_pads[0]); i++) {
		if (pin >= cnnt_pads[i].first && pin <= cnnt_pads[i].last &&
		    func == cnnt_pads[i].func && (r->cnnt_read(r) & cnnt_pads[i].sel))
			return cnnt_pads[i].block;
	}
	return NULL;
}

/* The matrix signals on a pin: its output signal and the inputs that read it. */
static int matrix_signals(struct regs *r, unsigned int pin, char *buf,
			  unsigned int len)
{
	uint32_t out = gpio_rd(r, GPIO_OUT_SEL(pin)) & OUT_SEL_SIG;
	unsigned int sig;
	int found = 0;

	buf[0] = '\0';
	if (out != SIG_GPIO_OUT) {
		append(buf, len, out < NSIGS ? sig_out_names[out] : NULL);
		found = 1;
	}
	for (sig = 0; sig < NSIGS; sig++) {
		uint32_t in = gpio_rd(r, GPIO_IN_SEL(sig));

		if ((in & IN_SEL_MATRIX) && (in & IN_SEL_PIN) == pin) {
			append(buf, len, sig_in_names[sig]);
			found = 1;
		}
	}
	return found;
}

void pin_get(struct regs *r, unsigned int pin, struct pin_state *st)
{
	uint32_t mux = r->iomux[pin];
	unsigned int func = (mux & IOMUX_MCU_MASK) >> IOMUX_MCU_SHIFT;
	int ie = !!(mux & IOMUX_IE);
	int oe = gpio_bit(r, pin, GPIO_ENABLE, GPIO_ENABLE1);

	st->pull = (mux & IOMUX_WPU ? PULL_UP : 0) | (mux & IOMUX_WPD ? PULL_DOWN : 0);
	st->out = gpio_bit(r, pin, GPIO_OUT, GPIO_OUT1);
	st->level = pin_level(r, pin);
	st->drive_ma = drive_ma[(mux & IOMUX_DRV_MASK) >> IOMUX_DRV_SHIFT];
	st->open_drain = !!(gpio_rd(r, GPIO_PIN(pin)) & PIN_PAD_DRIVER);

	if (func != FUNC_GPIO) {
		const char *name = iomux_func_name(pin, func);

		if (!name)
			name = cnnt_block(r, pin, func);

		st->mode = func < NFUNCS ? MODE_A0 + func : MODE_NO;
		st->func[0] = '\0';
		append(st->func, sizeof(st->func), name);
	} else if (matrix_signals(r, pin, st->func, sizeof(st->func))) {
		st->mode = MODE_MX;
	} else if (oe) {
		st->mode = MODE_OP;
		strcpy(st->func, "output");
	} else if (ie) {
		st->mode = MODE_IP;
		strcpy(st->func, "input");
	} else {
		st->mode = MODE_NO;
		strcpy(st->func, "none");
	}
}

static void iomux_update(struct regs *r, unsigned int pin, uint32_t clr, uint32_t set)
{
	r->iomux[pin] = (r->iomux[pin] & ~clr) | set;
}

static void cnnt_select(struct regs *r, unsigned int pin, unsigned int func)
{
	unsigned int i;

	for (i = 0; i < sizeof(cnnt_pads) / sizeof(cnnt_pads[0]); i++) {
		if (pin >= cnnt_pads[i].first && pin <= cnnt_pads[i].last &&
		    func == cnnt_pads[i].func)
			r->cnnt_write(r, r->cnnt_read(r) | cnnt_pads[i].sel);
	}
}

/* As the kernel driver: ip, op and no use the GPIO registers through the matrix. */
void pin_set_mode(struct regs *r, unsigned int pin, enum mode mode)
{
	uint32_t gpio_func = (uint32_t)FUNC_GPIO << IOMUX_MCU_SHIFT;

	if (mode <= MODE_A4) {
		cnnt_select(r, pin, mode - MODE_A0);
		iomux_update(r, pin, IOMUX_MCU_MASK,
			     (uint32_t)(mode - MODE_A0) << IOMUX_MCU_SHIFT | IOMUX_IE);
		return;
	}

	gpio_wr(r, GPIO_OUT_SEL(pin), SIG_GPIO_OUT);
	gpio_w1(r, pin, GPIO_ENABLE, GPIO_ENABLE1, mode == MODE_OP);
	iomux_update(r, pin, IOMUX_MCU_MASK | IOMUX_IE,
		     gpio_func | (mode == MODE_NO ? 0 : IOMUX_IE));
}

void pin_set_pull(struct regs *r, unsigned int pin, enum pull pull)
{
	iomux_update(r, pin, IOMUX_WPU | IOMUX_WPD,
		     (pull & PULL_UP ? IOMUX_WPU : 0) | (pull & PULL_DOWN ? IOMUX_WPD : 0));
}

void pin_set_out(struct regs *r, unsigned int pin, int high)
{
	gpio_w1(r, pin, GPIO_OUT, GPIO_OUT1, high);
}

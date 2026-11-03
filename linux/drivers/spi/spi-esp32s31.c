// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 GPSPI master. PIO, mode 0, 1/4-bit I/O and native memory operations. */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi-mem.h>

#define S31_SPI_CMD		0x00
#define S31_SPI_UPDATE		BIT(23)
#define S31_SPI_USR		BIT(24)
#define S31_SPI_ADDR		0x04
#define S31_SPI_CTRL		0x08
#define S31_SPI_FADDR_QUAD	BIT(6)
#define S31_SPI_FREAD_QUAD	BIT(15)
#define S31_SPI_DATA_IDLE_HIGH	GENMASK(21, 18)
#define S31_SPI_CLOCK		0x0c
#define S31_SPI_CLK_L		GENMASK(5, 0)
#define S31_SPI_CLK_H		GENMASK(11, 6)
#define S31_SPI_CLK_N		GENMASK(17, 12)
#define S31_SPI_CLK_PRE		GENMASK(21, 18)
#define S31_SPI_USER		0x10
#define S31_SPI_CS_HOLD		BIT(6)
#define S31_SPI_QUAD		BIT(13)
#define S31_SPI_MOSI		BIT(27)
#define S31_SPI_MISO		BIT(28)
#define S31_SPI_DUMMY		BIT(29)
#define S31_SPI_ADDRESS		BIT(30)
#define S31_SPI_COMMAND		BIT(31)
#define S31_SPI_USER1		0x14
#define S31_SPI_DUMMY_CYCLES	GENMASK(7, 0)
#define S31_SPI_HOLD_CYCLES	GENMASK(26, 22)
#define S31_SPI_ADDR_BITS	GENMASK(31, 27)
#define S31_SPI_USER2		0x18
#define S31_SPI_COMMAND_BITS	GENMASK(31, 28)
#define S31_SPI_DLEN		0x1c
#define S31_SPI_MISC		0x20
#define S31_SPI_CS_DISABLE	GENMASK(5, 0)
#define S31_SPI_CS_KEEP_ACTIVE	BIT(30)
#define S31_SPI_DMA_CONF		0x30
#define S31_SPI_INT_ENA		0x34
#define S31_SPI_W0		0x98
#define S31_SPI_SLAVE		0xe0
#define S31_SPI_CLK_GATE		0xe8
#define S31_SPI_CLK_ENABLE	(BIT(0) | BIT(1))
#define S31_SPI_FIFO_SIZE	64
#define S31_SPI_POLL_BUSY_US	1000

struct esp32s31_spi {
	void __iomem *base;
	unsigned long rate;
	u32 speed_hz;
	u32 effective_speed_hz;
	u32 cs;
};

static int esp32s31_spi_set_rate(struct esp32s31_spi *es, u32 speed_hz)
{
	unsigned int pre, n;
	u32 clock;

	if (!speed_hz)
		return -EINVAL;
	if (es->speed_hz == speed_hz)
		return 0;

	/* Round the divisor up: never exceed the peripheral's requested rate. */
	for (pre = 1; pre <= 16; pre++) {
		n = DIV_ROUND_UP(es->rate, (unsigned long)speed_hz * pre);
		if (n <= 64)
			break;
	}
	if (pre > 16)
		return -EINVAL;
	n = max(n, 2U);
	clock = FIELD_PREP(S31_SPI_CLK_L, n - 1) |
		FIELD_PREP(S31_SPI_CLK_H, n / 2 - 1) |
		FIELD_PREP(S31_SPI_CLK_N, n - 1) |
		FIELD_PREP(S31_SPI_CLK_PRE, pre - 1);
	writel(clock, es->base + S31_SPI_CLOCK);
	es->speed_hz = speed_hz;
	es->effective_speed_hz = es->rate / (pre * n);
	return 0;
}

static int esp32s31_spi_start(struct esp32s31_spi *es)
{
	u32 value;
	int ret;

	writel(S31_SPI_UPDATE, es->base + S31_SPI_CMD);
	ret = readl_poll_timeout(es->base + S31_SPI_CMD, value,
				 !(value & S31_SPI_UPDATE), 0, 10000);
	if (ret)
		return ret;
	writel(S31_SPI_USR, es->base + S31_SPI_CMD);
	return readl_poll_timeout(es->base + S31_SPI_CMD, value,
				  !(value & S31_SPI_USR), 0, 10000);
}

static void esp32s31_spi_write_fifo(struct esp32s31_spi *es, const u8 *buf,
				    unsigned int count)
{
	unsigned int i;
	u32 value;

	for (i = 0; i < count; i += sizeof(value)) {
		value = 0;
		memcpy(&value, buf + i,
		       min_t(unsigned int, sizeof(value), count - i));
		writel(value, es->base + S31_SPI_W0 + i);
	}
}

static void esp32s31_spi_read_fifo(struct esp32s31_spi *es, u8 *buf,
				   unsigned int count)
{
	unsigned int i;
	u32 value;

	for (i = 0; i < count; i += sizeof(value)) {
		value = readl(es->base + S31_SPI_W0 + i);
		memcpy(buf + i, &value,
		       min_t(unsigned int, sizeof(value), count - i));
	}
}

static int esp32s31_spi_transfer_one(struct spi_controller *ctlr,
				     struct spi_device *spi,
				     struct spi_transfer *xfer)
{
	struct esp32s31_spi *es = spi_controller_get_devdata(ctlr);
	const u8 *tx = xfer->tx_buf;
	u8 *rx = xfer->rx_buf;
	unsigned int offset, count;
	u32 user;
	int ret;

	ret = esp32s31_spi_set_rate(es, xfer->speed_hz);
	if (ret)
		return ret;
	xfer->effective_speed_hz = es->effective_speed_hz;
	user = S31_SPI_CS_HOLD | (tx ? S31_SPI_MOSI : S31_SPI_MISO);
	if (xfer->tx_nbits == SPI_NBITS_QUAD)
		user |= S31_SPI_QUAD;
	writel(user, es->base + S31_SPI_USER);
	writel(FIELD_PREP(S31_SPI_HOLD_CYCLES, 1), es->base + S31_SPI_USER1);
	/* WP and HOLD must stay high until the flash enables quad I/O. */
	writel(S31_SPI_DATA_IDLE_HIGH |
	       (xfer->rx_nbits == SPI_NBITS_QUAD ? S31_SPI_FREAD_QUAD : 0),
	       es->base + S31_SPI_CTRL);

	for (offset = 0; offset < xfer->len; offset += count) {
		count = min_t(unsigned int, xfer->len - offset, S31_SPI_FIFO_SIZE);
		writel(es->cs, es->base + S31_SPI_MISC);
		if (tx)
			esp32s31_spi_write_fifo(es, tx + offset, count);
		writel(count * 8 - 1, es->base + S31_SPI_DLEN);
		ret = esp32s31_spi_start(es);
		if (ret)
			return ret;
		if (rx)
			esp32s31_spi_read_fifo(es, rx + offset, count);
		cond_resched();
	}
	return 0;
}

static void esp32s31_spi_set_cs(struct spi_device *spi, bool inactive)
{
	struct esp32s31_spi *es = spi_controller_get_devdata(spi->controller);

	es->cs = S31_SPI_CS_DISABLE;
	if (!inactive)
		es->cs = (es->cs & ~BIT(0)) | S31_SPI_CS_KEEP_ACTIVE;
	writel(es->cs, es->base + S31_SPI_MISC);
}

/* Native CS allows spi-mem to skip message allocation and phase transfers. */
static bool esp32s31_spi_supports_op(struct spi_mem *mem,
				     const struct spi_mem_op *op)
{
	if (op->cmd.nbytes != 1 || op->cmd.buswidth != 1 || op->addr.nbytes > 4 ||
	    op->cmd.dtr || op->addr.dtr || op->dummy.dtr || op->data.dtr)
		return false;
	if (op->addr.nbytes && op->addr.buswidth != 1 && op->addr.buswidth != 4)
		return false;
	if (op->data.nbytes && op->data.buswidth != 1 && op->data.buswidth != 4)
		return false;
	if (op->dummy.nbytes &&
	    (op->dummy.buswidth != (op->addr.nbytes ? op->addr.buswidth : 1) ||
	     op->dummy.nbytes * 8 / op->dummy.buswidth > 256))
		return false;

	return spi_mem_default_supports_op(mem, op);
}

static int esp32s31_spi_exec_op(struct spi_mem *mem,
				const struct spi_mem_op *op)
{
	struct esp32s31_spi *es = spi_controller_get_devdata(mem->spi->controller);
	const u8 *tx = op->data.dir == SPI_MEM_DATA_OUT ? op->data.buf.out : NULL;
	u8 *rx = op->data.dir == SPI_MEM_DATA_IN ? op->data.buf.in : NULL;
	unsigned int offset = 0, count, dummy;
	u32 ctrl = S31_SPI_DATA_IDLE_HIGH, user = S31_SPI_CS_HOLD;
	u32 user1 = FIELD_PREP(S31_SPI_HOLD_CYCLES, 1);
	u32 phases = S31_SPI_COMMAND;
	int ret;

	ret = esp32s31_spi_set_rate(es, op->max_freq);
	if (ret)
		return ret;
	if (op->addr.nbytes) {
		phases |= S31_SPI_ADDRESS;
		user1 |= FIELD_PREP(S31_SPI_ADDR_BITS, op->addr.nbytes * 8 - 1);
		writel(op->addr.val << (32 - op->addr.nbytes * 8),
		       es->base + S31_SPI_ADDR);
		if (op->addr.buswidth == 4)
			ctrl |= S31_SPI_FADDR_QUAD;
	}
	if (op->dummy.nbytes) {
		phases |= S31_SPI_DUMMY;
		dummy = op->dummy.nbytes * 8 / op->dummy.buswidth;
		user1 |= FIELD_PREP(S31_SPI_DUMMY_CYCLES, dummy - 1);
	}
	if (op->data.nbytes) {
		user |= tx ? S31_SPI_MOSI : S31_SPI_MISO;
		if (op->data.buswidth == 4) {
			if (tx)
				user |= S31_SPI_QUAD;
			else
				ctrl |= S31_SPI_FREAD_QUAD;
		}
	}
	writel(ctrl, es->base + S31_SPI_CTRL);
	writel(user1, es->base + S31_SPI_USER1);
	writel(FIELD_PREP(S31_SPI_COMMAND_BITS, 7) | op->cmd.opcode,
	       es->base + S31_SPI_USER2);

	/* Send command/address/dummy once; continue data under the same CS. */
	do {
		count = min_t(unsigned int, op->data.nbytes - offset,
			      S31_SPI_FIFO_SIZE);
		writel(user | phases, es->base + S31_SPI_USER);
		writel((S31_SPI_CS_DISABLE & ~BIT(0)) |
		       (offset + count < op->data.nbytes ? S31_SPI_CS_KEEP_ACTIVE : 0),
		       es->base + S31_SPI_MISC);
		if (tx && count)
			esp32s31_spi_write_fifo(es, tx + offset, count);
		if (count)
			writel(count * 8 - 1, es->base + S31_SPI_DLEN);
		ret = esp32s31_spi_start(es);
		if (ret)
			break;
		if (rx && count)
			esp32s31_spi_read_fifo(es, rx + offset, count);
		offset += count;
		phases = 0;
		cond_resched();
	} while (offset < op->data.nbytes);

	writel(S31_SPI_CS_DISABLE, es->base + S31_SPI_MISC);
	es->cs = S31_SPI_CS_DISABLE;
	return ret;
}

static int esp32s31_spi_poll_status(struct spi_mem *mem,
				    const struct spi_mem_op *op, u16 mask, u16 match,
				    unsigned long initial_delay_us,
				    unsigned long polling_delay_us,
				    unsigned long timeout_ms)
{
	struct spi_mem_op poll_op = *op;
	const u8 *buf = op->data.buf.in;
	ktime_t now = ktime_get();
	ktime_t deadline = ktime_add_ms(now, timeout_ms);
	ktime_t busy_deadline = ktime_add_us(now, S31_SPI_POLL_BUSY_US);
	u16 status;
	int ret;

	spi_mem_adjust_op_freq(mem, &poll_op);
	if (!esp32s31_spi_supports_op(mem, &poll_op))
		return -EOPNOTSUPP;

	/* Short NAND waits must not become whole ticks without high-res timers. */
	if (initial_delay_us <= S31_SPI_POLL_BUSY_US)
		udelay(initial_delay_us);
	else
		usleep_range(initial_delay_us, initial_delay_us + 1);

	for (;;) {
		ret = esp32s31_spi_exec_op(mem, &poll_op);
		if (ret)
			return ret;
		status = op->data.nbytes == 2 ? (buf[0] << 8) | buf[1] : buf[0];
		if ((status & mask) == match)
			return 0;
		now = ktime_get();
		if (timeout_ms && ktime_compare(now, deadline) >= 0)
			return -ETIMEDOUT;

		/* Bound active polling; slow erases and faulty devices may sleep. */
		if (ktime_compare(now, busy_deadline) < 0 &&
		    polling_delay_us <= S31_SPI_POLL_BUSY_US)
			udelay(polling_delay_us);
		else
			usleep_range(polling_delay_us, polling_delay_us + 1);
	}
}

static const struct spi_controller_mem_ops esp32s31_spi_mem_ops = {
	.supports_op = esp32s31_spi_supports_op,
	.exec_op = esp32s31_spi_exec_op,
	.poll_status = esp32s31_spi_poll_status,
};

static int esp32s31_spi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct spi_controller *ctlr;
	struct esp32s31_spi *es;
	struct reset_control *reset;
	struct clk *core;
	int ret;

	ctlr = devm_spi_alloc_host(dev, sizeof(*es));
	if (!ctlr)
		return -ENOMEM;
	es = spi_controller_get_devdata(ctlr);
	es->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(es->base))
		return PTR_ERR(es->base);

	/* Keep both clocks enabled for the lifetime of the controller. */
	core = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(core))
		return dev_err_probe(dev, PTR_ERR(core), "APB clock\n");
	core = devm_clk_get_enabled(dev, "core");
	if (IS_ERR(core))
		return dev_err_probe(dev, PTR_ERR(core), "core clock\n");
	es->rate = clk_get_rate(core);
	if (!es->rate)
		return -EINVAL;

	reset = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "reset\n");
	ret = reset_control_assert(reset);
	if (ret)
		return ret;
	ret = reset_control_deassert(reset);
	if (ret)
		return ret;

	writel(S31_SPI_CLK_ENABLE, es->base + S31_SPI_CLK_GATE);
	writel(0, es->base + S31_SPI_SLAVE);
	writel(S31_SPI_DATA_IDLE_HIGH, es->base + S31_SPI_CTRL);
	writel(0, es->base + S31_SPI_USER);
	writel(0, es->base + S31_SPI_USER1);
	writel(0, es->base + S31_SPI_DMA_CONF);
	writel(0, es->base + S31_SPI_INT_ENA);
	writel(S31_SPI_CS_DISABLE, es->base + S31_SPI_MISC);
	es->cs = S31_SPI_CS_DISABLE;

	ctlr->dev.of_node = dev->of_node;
	ctlr->mode_bits = SPI_TX_QUAD | SPI_RX_QUAD;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	ctlr->flags = SPI_CONTROLLER_HALF_DUPLEX;
	ctlr->use_gpio_descriptors = device_property_present(dev, "cs-gpios");
	if (!ctlr->use_gpio_descriptors)
		ctlr->mem_ops = &esp32s31_spi_mem_ops;
	ctlr->num_chipselect = 1;
	ctlr->max_speed_hz = es->rate / 2;
	ctlr->min_speed_hz = DIV_ROUND_UP(es->rate, 16 * 64);
	ctlr->transfer_one = esp32s31_spi_transfer_one;
	ctlr->set_cs = esp32s31_spi_set_cs;

	return devm_spi_register_controller(dev, ctlr);
}

static const struct of_device_id esp32s31_spi_of_match[] = {
	{ .compatible = "esp,esp32s31-spi" },
	{}
};
MODULE_DEVICE_TABLE(of, esp32s31_spi_of_match);

static struct platform_driver esp32s31_spi_driver = {
	.probe = esp32s31_spi_probe,
	.driver = {
		.name = "esp32s31-spi",
		.of_match_table = esp32s31_spi_of_match,
	},
};
module_platform_driver(esp32s31_spi_driver);

MODULE_DESCRIPTION("ESP32-S31 PIO SPI master");
MODULE_LICENSE("GPL");

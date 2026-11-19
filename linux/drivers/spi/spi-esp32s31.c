// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 GPSPI2/GPSPI3 host and target driver.
 *
 * Originally ported from GrieferPig/linux-esp32-s31, branch v6.18-esp32-s31
 * (drivers/spi/spi-esp32s31.c), into annoyedmilk/esp32-s31-linux 7.3.4.
 * Adapted for this port's reset controller, GPIO CS and PIO initialization.
 * Kconfig depends on SOC_ESP32S31 (see patch 0025) instead of
 * ARCH_ESPRESSIF.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>

#define S31_SPI_FIFO_SIZE	64
#define S31_SPI_CMD		0x00
#define S31_SPI_CMD_UPDATE	BIT(23)
#define S31_SPI_CMD_USR		BIT(24)
#define S31_SPI_CTRL		0x08
#define S31_SPI_CTRL_RD_LSB	GENMASK(24, 23)
#define S31_SPI_CTRL_WR_LSB	GENMASK(26, 25)
#define S31_SPI_CTRL_FREAD_DUAL	BIT(14)
#define S31_SPI_CTRL_FREAD_QUAD	BIT(15)
#define S31_SPI_CTRL_FREAD_OCT	BIT(16)
#define S31_SPI_CTRL_DATA_IDLE	GENMASK(21, 18)
#define S31_SPI_CLOCK		0x0c
#define S31_SPI_CLKCNT_L		GENMASK(5, 0)
#define S31_SPI_CLKCNT_H		GENMASK(11, 6)
#define S31_SPI_CLKCNT_N		GENMASK(17, 12)
#define S31_SPI_CLKDIV_PRE	GENMASK(21, 18)
#define S31_SPI_CLK_EQU_SYSCLK	BIT(31)
#define S31_SPI_USER		0x10
#define S31_SPI_DOUTDIN		BIT(0)
#define S31_SPI_TSCK_I_EDGE	BIT(5)
#define S31_SPI_CS_HOLD		BIT(6)
#define S31_SPI_RSCK_I_EDGE	BIT(8)
#define S31_SPI_CK_OUT_EDGE	BIT(9)
#define S31_SPI_FWRITE_DUAL	BIT(12)
#define S31_SPI_FWRITE_QUAD	BIT(13)
#define S31_SPI_FWRITE_OCT	BIT(14)
#define S31_SPI_USR_MOSI		BIT(27)
#define S31_SPI_USR_MISO		BIT(28)
#define S31_SPI_USER1		0x14
#define S31_SPI_MST_WFULL_ERR_END_EN	BIT(16)
#define S31_SPI_CS_SETUP_TIME	GENMASK(21, 17)
#define S31_SPI_ADDR_BITLEN	GENMASK(31, 27)
#define S31_SPI_USER2		0x18
#define S31_SPI_MST_REMPTY_ERR_END_EN	BIT(27)
#define S31_SPI_COMMAND_BITLEN	GENMASK(31, 28)
#define S31_SPI_MS_DLEN		0x1c
#define S31_SPI_MISC		0x20
#define S31_SPI_CS_DIS_MASK	GENMASK(5, 0)
#define S31_SPI_SLAVE_CS_POL	BIT(23)
#define S31_SPI_CK_IDLE_EDGE	BIT(29)
#define S31_SPI_CS_KEEP_ACTIVE	BIT(30)
#define S31_SPI_DMA_CONF		0x30
#define S31_SPI_RX_EOF_EN		BIT(21)
#define S31_SPI_SLV_RX_SEG_TRANS_CLR_EN	BIT(19)
#define S31_SPI_SLV_TX_SEG_TRANS_CLR_EN	BIT(20)
#define S31_SPI_DMA_RX_EN	BIT(27)
#define S31_SPI_DMA_TX_EN	BIT(28)
#define S31_SPI_RX_AFIFO_RST	BIT(29)
#define S31_SPI_BUF_AFIFO_RST	BIT(30)
#define S31_SPI_DMA_AFIFO_RST	BIT(31)
#define S31_SPI_INT_ENA		0x34
#define S31_SPI_INT_CLR		0x38
#define S31_SPI_MST_RX_AFIFO_WFULL_ERR	BIT(17)
#define S31_SPI_MST_TX_AFIFO_REMPTY_ERR	BIT(18)
#define S31_SPI_INT_RAW		0x3c
#define S31_SPI_INT_ST		0x40
#define S31_SPI_TRANS_DONE	BIT(12)
#define S31_SPI_SLAVE		0xe0
#define S31_SPI_CLK_MODE_13	BIT(2)
#define S31_SPI_SLAVE_MODE	BIT(26)
#define S31_SPI_SOFT_RESET	BIT(27)
#define S31_SPI_SLAVE1		0xe4
#define S31_SPI_SLV_DATA_BITLEN	GENMASK(17, 0)
#define S31_SPI_CLK_GATE		0xe8
#define S31_SPI_W0		0x98

struct esp32s31_spi {
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct completion done;
	struct completion dma_tx_done;
	struct completion dma_rx_done;
	struct dma_chan *tx_dma;
	struct dma_chan *rx_dma;
	void *tx_dma_buf;
	void *rx_dma_buf;
	dma_addr_t tx_dma_addr;
	dma_addr_t rx_dma_addr;
	dma_addr_t fifo_addr;
	u32 max_data_lines;
	u32 gpio_cs_drive_strength;
	int irq;
	bool target;
	bool polling;
	bool target_aborted;
};

static irqreturn_t esp32s31_spi_irq(int irq, void *data)
{
	struct esp32s31_spi *s = data;
	u32 status = readl(s->base + S31_SPI_INT_ST);

	if (!(status & S31_SPI_TRANS_DONE))
		return IRQ_NONE;
	writel(status, s->base + S31_SPI_INT_CLR);
	complete(&s->done);

	return IRQ_HANDLED;
}

static u32 esp32s31_spi_set_clock(struct esp32s31_spi *s, u32 hz)
{
	u32 parent = clk_get_rate(s->clk);
	u32 div, pre, n, h;
	u32 val;

	if (hz >= parent) {
		val = S31_SPI_CLK_EQU_SYSCLK;
		writel(val, s->base + S31_SPI_CLOCK);
		return parent;
	}

	div = DIV_ROUND_UP(parent, hz);
	pre = clamp_t(u32, DIV_ROUND_UP(div, 64), 1, 16);
	n = clamp_t(u32, DIV_ROUND_UP(div, pre), 2, 64);
	h = max_t(u32, n / 2, 1);
	val = FIELD_PREP(S31_SPI_CLKCNT_L, n - 1) |
	      FIELD_PREP(S31_SPI_CLKCNT_H, h - 1) |
	      FIELD_PREP(S31_SPI_CLKCNT_N, n - 1) |
	      FIELD_PREP(S31_SPI_CLKDIV_PRE, pre - 1);
	writel(val, s->base + S31_SPI_CLOCK);

	return parent / (pre * n);
}

static size_t esp32s31_spi_max_transfer_size(struct spi_device *spi)
{
	struct esp32s31_spi *s = spi_controller_get_devdata(spi->controller);

	return s->target && !s->rx_dma ? S31_SPI_FIFO_SIZE : SZ_4K;
}

static void esp32s31_spi_target_reset(struct esp32s31_spi *s)
{
	u32 slave = readl(s->base + S31_SPI_SLAVE);

	writel(slave | S31_SPI_SOFT_RESET, s->base + S31_SPI_SLAVE);
	writel(slave & ~S31_SPI_SOFT_RESET, s->base + S31_SPI_SLAVE);
}

static int esp32s31_spi_target_abort(struct spi_controller *ctlr)
{
	struct esp32s31_spi *s = spi_controller_get_devdata(ctlr);

	WRITE_ONCE(s->target_aborted, true);
	complete(&s->done);
	return 0;
}

/* Linux buffers contain native-endian words.  The FIFO consumes bytes; turn
 * 16/32-bit MSB-first words into wire byte order (and back on receive). */
static void esp32s31_spi_target_copy(void *dst, const void *src, size_t len,
				   unsigned int bits, bool lsb)
{
	const u8 *in = src;
	u8 *out = dst;
	unsigned int bytes = bits / 8;
	size_t i;
	unsigned int j;

	if (lsb || bits == 8) {
		memcpy(dst, src, len);
		return;
	}
	for (i = 0; i < len; i += bytes)
		for (j = 0; j < bytes; j++)
			out[i + j] = in[i + bytes - 1 - j];
}

static void esp32s31_spi_dma_complete(void *arg);

static int esp32s31_spi_target_dma_prepare(struct esp32s31_spi *s,
					 struct spi_device *spi,
					 struct spi_transfer *xfer)
{
	struct dma_slave_config config = {
		.src_addr = s->fifo_addr, .dst_addr = s->fifo_addr,
		.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES,
		.src_maxburst = 4, .dst_maxburst = 4,
	};
	struct dma_async_tx_descriptor *desc;
	dma_cookie_t cookie;
	u32 conf = readl(s->base + S31_SPI_DMA_CONF);
	int ret;

	/* Use CS deassertion as RX EOF, including short transactions. */
	conf &= ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN | S31_SPI_RX_EOF_EN);
	writel(conf | S31_SPI_RX_AFIFO_RST | S31_SPI_DMA_AFIFO_RST,
	       s->base + S31_SPI_DMA_CONF);
	writel(conf, s->base + S31_SPI_DMA_CONF);
	if (xfer->rx_buf) {
		config.direction = DMA_DEV_TO_MEM;
		ret = dmaengine_slave_config(s->rx_dma, &config);
		if (ret)
			return ret;
		dma_sync_single_for_device(s->dev, s->rx_dma_addr,
					 ALIGN(xfer->len, 4), DMA_FROM_DEVICE);
		desc = dmaengine_prep_slave_single(s->rx_dma, s->rx_dma_addr,
				ALIGN(xfer->len, 4), DMA_DEV_TO_MEM,
				DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		if (!desc)
			return -ENOMEM;
		reinit_completion(&s->dma_rx_done);
		desc->callback = esp32s31_spi_dma_complete;
		desc->callback_param = &s->dma_rx_done;
		cookie = dmaengine_submit(desc);
		ret = dma_submit_error(cookie);
		if (ret)
			return ret;
		conf |= S31_SPI_DMA_RX_EN;
	}
	if (xfer->tx_buf) {
		config.direction = DMA_MEM_TO_DEV;
		ret = dmaengine_slave_config(s->tx_dma, &config);
		if (ret)
			return ret;
		esp32s31_spi_target_copy(s->tx_dma_buf, xfer->tx_buf, xfer->len,
				xfer->bits_per_word, spi->mode & SPI_LSB_FIRST);
		dma_sync_single_for_device(s->dev, s->tx_dma_addr,
					 xfer->len, DMA_TO_DEVICE);
		desc = dmaengine_prep_slave_single(s->tx_dma, s->tx_dma_addr,
				xfer->len, DMA_MEM_TO_DEV,
				DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		if (!desc)
			return -ENOMEM;
		reinit_completion(&s->dma_tx_done);
		desc->callback = esp32s31_spi_dma_complete;
		desc->callback_param = &s->dma_tx_done;
		cookie = dmaengine_submit(desc);
		ret = dma_submit_error(cookie);
		if (ret)
			return ret;
		conf |= S31_SPI_DMA_TX_EN;
	}
	writel(conf, s->base + S31_SPI_DMA_CONF);
	if (xfer->rx_buf)
		dma_async_issue_pending(s->rx_dma);
	if (xfer->tx_buf)
		dma_async_issue_pending(s->tx_dma);
	return 0;
}

static int esp32s31_spi_target_transfer(struct spi_controller *ctlr,
					struct spi_device *spi,
					struct spi_transfer *xfer)
{
	struct esp32s31_spi *s = spi_controller_get_devdata(ctlr);
	u32 bitlen, ctrl, misc, slave, user, word;
	u8 fifo[S31_SPI_FIFO_SIZE];
	bool dma = s->rx_dma && s->tx_dma;
	unsigned int count, i, bytes;
	int ret;

	if (!xfer->len || xfer->len > esp32s31_spi_max_transfer_size(spi))
		return xfer->len ? -EMSGSIZE : 0;
	if (xfer->bits_per_word != 8 && xfer->bits_per_word != 16 &&
	    xfer->bits_per_word != 32)
		return -EINVAL;
	if (xfer->len % (xfer->bits_per_word / 8))
		return -EINVAL;
	writel(0, s->base + S31_SPI_INT_ENA);
	synchronize_irq(s->irq);
	esp32s31_spi_target_reset(s);
	reinit_completion(&s->done);
	WRITE_ONCE(s->target_aborted, false);

	ctrl = readl(s->base + S31_SPI_CTRL);
	ctrl &= ~(S31_SPI_CTRL_RD_LSB | S31_SPI_CTRL_WR_LSB);
	if (spi->mode & SPI_LSB_FIRST)
		ctrl |= FIELD_PREP(S31_SPI_CTRL_RD_LSB, 1) |
			FIELD_PREP(S31_SPI_CTRL_WR_LSB, 1);
	writel(ctrl, s->base + S31_SPI_CTRL);

	user = S31_SPI_DOUTDIN;
	if (xfer->rx_buf)
		user |= S31_SPI_USR_MOSI;
	if (xfer->tx_buf)
		user |= S31_SPI_USR_MISO;
	if (!!(spi->mode & SPI_CPHA) != !!(spi->mode & SPI_CPOL))
		user |= S31_SPI_TSCK_I_EDGE | S31_SPI_RSCK_I_EDGE;
	writel(user, s->base + S31_SPI_USER);

	slave = S31_SPI_SLAVE_MODE;
	if (spi->mode & SPI_CPHA)
		slave |= S31_SPI_CLK_MODE_13;
	writel(slave, s->base + S31_SPI_SLAVE);
	misc = (spi->mode & SPI_CPOL) ? S31_SPI_CK_IDLE_EDGE : 0;
	if (spi->mode & SPI_CS_HIGH)
		misc |= S31_SPI_SLAVE_CS_POL;
	writel(misc, s->base + S31_SPI_MISC);

	if (dma) {
		ret = esp32s31_spi_target_dma_prepare(s, spi, xfer);
		if (ret)
			goto stop;
	} else if (xfer->tx_buf) {
		const u8 *tx = fifo;

		esp32s31_spi_target_copy(fifo, xfer->tx_buf, xfer->len,
				xfer->bits_per_word, spi->mode & SPI_LSB_FIRST);

		for (i = 0; i < xfer->len; i += 4) {
			word = 0;
			count = min_t(unsigned int, 4, xfer->len - i);
			memcpy(&word, tx + i, count);
			writel(word, s->base + S31_SPI_W0 + i);
		}
	}

	writel(S31_SPI_TRANS_DONE, s->base + S31_SPI_INT_CLR);
	writel(S31_SPI_TRANS_DONE, s->base + S31_SPI_INT_ENA);
	writel(S31_SPI_CMD_USR, s->base + S31_SPI_CMD);
	ret = wait_for_completion_interruptible(&s->done);
	writel(0, s->base + S31_SPI_INT_ENA);
	if (ret || READ_ONCE(s->target_aborted)) {
		ret = -EINTR;
		goto stop;
	}

	bitlen = FIELD_GET(S31_SPI_SLV_DATA_BITLEN,
			   readl(s->base + S31_SPI_SLAVE1));
	if (bitlen == xfer->len * 8 - 1)
		bitlen++;
	bytes = min_t(unsigned int, DIV_ROUND_UP(bitlen, 8), xfer->len);
	if (bitlen != xfer->len * 8) {
		ret = -EMSGSIZE;
		goto stop;
	}
	if (dma) {
		if ((xfer->rx_buf && !wait_for_completion_timeout(&s->dma_rx_done,
								msecs_to_jiffies(100))) ||
		    (xfer->tx_buf && !wait_for_completion_timeout(&s->dma_tx_done,
								msecs_to_jiffies(100)))) {
			ret = -ETIMEDOUT;
			goto stop;
		}
		if (xfer->rx_buf) {
			dma_sync_single_for_cpu(s->dev, s->rx_dma_addr,
					       ALIGN(xfer->len, 4), DMA_FROM_DEVICE);
			esp32s31_spi_target_copy(xfer->rx_buf, s->rx_dma_buf, bytes,
					xfer->bits_per_word, spi->mode & SPI_LSB_FIRST);
		}
	} else if (xfer->rx_buf) {
		u8 *rx = fifo;

		for (i = 0; i < bytes; i += 4) {
			word = readl(s->base + S31_SPI_W0 + i);
			count = min_t(unsigned int, 4, bytes - i);
			memcpy(rx + i, &word, count);
		}
		esp32s31_spi_target_copy(xfer->rx_buf, fifo, bytes,
				xfer->bits_per_word, spi->mode & SPI_LSB_FIRST);
	}
	ret = 0;
stop:
	writel(0, s->base + S31_SPI_INT_ENA);
	esp32s31_spi_target_reset(s);
	if (dma) {
		writel(readl(s->base + S31_SPI_DMA_CONF) &
		       ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN),
		       s->base + S31_SPI_DMA_CONF);
		dmaengine_terminate_sync(s->rx_dma);
		dmaengine_terminate_sync(s->tx_dma);
		if (ret && xfer->rx_buf)
			dma_sync_single_for_cpu(s->dev, s->rx_dma_addr,
					       ALIGN(xfer->len, 4), DMA_FROM_DEVICE);
		if (xfer->tx_buf)
			dma_sync_single_for_cpu(s->dev, s->tx_dma_addr,
					       xfer->len, DMA_TO_DEVICE);
	}
	return ret;
}

static int esp32s31_spi_transaction_error(struct esp32s31_spi *s,
					const char *phase)
{
	dev_err_ratelimited(s->dev,
		"%s timeout: CMD=%08x RAW=%08x ST=%08x USER=%08x CTRL=%08x CLOCK=%08x MISC=%08x DMA=%08x\n",
		phase, readl(s->base + S31_SPI_CMD),
		readl(s->base + S31_SPI_INT_RAW), readl(s->base + S31_SPI_INT_ST),
		readl(s->base + S31_SPI_USER), readl(s->base + S31_SPI_CTRL),
		readl(s->base + S31_SPI_CLOCK), readl(s->base + S31_SPI_MISC),
		readl(s->base + S31_SPI_DMA_CONF));
	writel(0, s->base + S31_SPI_INT_ENA);
	synchronize_irq(s->irq);
	/* Stop the state machine before the core releases the physical CS. */
	esp32s31_spi_target_reset(s);
	writel(~0U, s->base + S31_SPI_INT_CLR);
	return -ETIMEDOUT;
}

static int esp32s31_spi_wait_transaction(struct esp32s31_spi *s,
					 unsigned int timeout_ms)
{
	u32 cmd;
	unsigned long timeout;

	/* A late IRQ from an earlier chunk must not complete the next one. */
	writel(0, s->base + S31_SPI_INT_ENA);
	synchronize_irq(s->irq);
	reinit_completion(&s->done);
	writel(~0U, s->base + S31_SPI_INT_CLR);
	writel(S31_SPI_CMD_UPDATE, s->base + S31_SPI_CMD);
	if (readl_poll_timeout(s->base + S31_SPI_CMD, cmd,
			       !(cmd & S31_SPI_CMD_UPDATE), 1, 1000))
		return esp32s31_spi_transaction_error(s, "UPDATE");

	if (!s->polling)
		writel(S31_SPI_TRANS_DONE, s->base + S31_SPI_INT_ENA);
	writel(S31_SPI_CMD_USR, s->base + S31_SPI_CMD);
	if (s->polling) {
		if (readl_poll_timeout(s->base + S31_SPI_INT_RAW, cmd,
				       cmd & S31_SPI_TRANS_DONE, 10,
				       timeout_ms * 1000))
			return esp32s31_spi_transaction_error(s, "TRANS_DONE");
		timeout = 1;
	} else {
		timeout = wait_for_completion_timeout(&s->done,
					       msecs_to_jiffies(timeout_ms));
	}
	if (!timeout) {
		/* Diagnose a lost interrupt separately from a stalled shifter. */
		if (!(readl(s->base + S31_SPI_INT_RAW) & S31_SPI_TRANS_DONE))
			return esp32s31_spi_transaction_error(s, "TRANS_DONE");
		dev_warn_once(s->dev,
			"TRANS_DONE pending without IRQ; using polling, check CLIC route\n");
		s->polling = true;
	}
	writel(0, s->base + S31_SPI_INT_ENA);
	/* The IRQ can precede CMD.USR falling. Keep GPIO CS low until idle. */
	if (readl_poll_timeout(s->base + S31_SPI_CMD, cmd,
			       !(cmd & S31_SPI_CMD_USR), 1, 100000))
		return esp32s31_spi_transaction_error(s, "USR");
	writel(S31_SPI_TRANS_DONE, s->base + S31_SPI_INT_CLR);
	return 0;
}

static void esp32s31_spi_dma_complete(void *data)
{
	complete(data);
}

static int esp32s31_spi_dma_transfer(struct esp32s31_spi *s,
				     struct spi_controller *ctlr,
				     struct spi_transfer *xfer, u32 misc)
{
	struct dma_async_tx_descriptor *desc;
	struct dma_slave_config config = { };
	enum dma_transfer_direction direction;
	struct completion *done;
	struct dma_chan *chan;
	dma_addr_t dma_addr;
	void *dma_buf;
	dma_cookie_t cookie;
	unsigned long completed;
	u32 dma_conf;
	unsigned int timeout_ms = spi_controller_xfer_timeout(ctlr, xfer);
	int ret;

	direction = xfer->tx_buf ? DMA_MEM_TO_DEV : DMA_DEV_TO_MEM;
	if (xfer->tx_buf) {
		chan = s->tx_dma;
		done = &s->dma_tx_done;
		dma_addr = s->tx_dma_addr;
		dma_buf = s->tx_dma_buf;
		memcpy(dma_buf, xfer->tx_buf, xfer->len);
		dma_sync_single_for_device(s->dev, dma_addr, xfer->len,
					   DMA_TO_DEVICE);
	} else {
		chan = s->rx_dma;
		done = &s->dma_rx_done;
		dma_addr = s->rx_dma_addr;
		dma_buf = s->rx_dma_buf;
		dma_sync_single_for_device(s->dev, dma_addr, xfer->len,
					   DMA_FROM_DEVICE);
	}
	if (!chan)
		return -ENODEV;
	config.direction = direction;
	config.src_addr = s->fifo_addr;
	config.dst_addr = s->fifo_addr;
	config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.src_maxburst = 8;
	config.dst_maxburst = 8;
	ret = dmaengine_slave_config(chan, &config);
	if (ret)
		return ret;

	desc = dmaengine_prep_slave_single(chan, dma_addr, xfer->len,
					   direction,
					   DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!desc)
		return -ENOMEM;
	reinit_completion(done);
	desc->callback = esp32s31_spi_dma_complete;
	desc->callback_param = done;
	cookie = dmaengine_submit(desc);
	ret = dma_submit_error(cookie);
	if (ret)
		return ret;

	dma_conf = readl(s->base + S31_SPI_DMA_CONF);
	dma_conf &= ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN);
	writel(dma_conf | S31_SPI_RX_AFIFO_RST | S31_SPI_DMA_AFIFO_RST,
	       s->base + S31_SPI_DMA_CONF);
	writel(dma_conf, s->base + S31_SPI_DMA_CONF);
	writel(S31_SPI_MST_RX_AFIFO_WFULL_ERR |
	       S31_SPI_MST_TX_AFIFO_REMPTY_ERR,
	       s->base + S31_SPI_INT_CLR);
	dma_conf |= xfer->tx_buf ? S31_SPI_DMA_TX_EN : S31_SPI_DMA_RX_EN;
	writel(dma_conf, s->base + S31_SPI_DMA_CONF);
	writel(misc, s->base + S31_SPI_MISC);
	writel(xfer->len * 8 - 1, s->base + S31_SPI_MS_DLEN);
	dma_async_issue_pending(chan);
	ret = esp32s31_spi_wait_transaction(s, timeout_ms);
	if (ret)
		goto terminate;
	completed = wait_for_completion_timeout(done,
						msecs_to_jiffies(timeout_ms));
	if (!completed) {
		ret = -ETIMEDOUT;
		goto terminate;
	}
	if (xfer->rx_buf)
		dma_sync_single_for_cpu(s->dev, dma_addr, xfer->len,
					DMA_FROM_DEVICE);
	if (xfer->rx_buf)
		memcpy(xfer->rx_buf, dma_buf, xfer->len);
	writel(dma_conf & ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN),
	       s->base + S31_SPI_DMA_CONF);
	return 0;

terminate:
	dmaengine_terminate_sync(chan);
	writel(dma_conf & ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN),
	       s->base + S31_SPI_DMA_CONF);
	return ret;
}

static int esp32s31_spi_dma_duplex_transfer(struct esp32s31_spi *s,
					     struct spi_controller *ctlr,
					     struct spi_transfer *xfer,
					     u32 misc)
{
	struct dma_async_tx_descriptor *tx_desc, *rx_desc;
	struct dma_slave_config config = { };
	dma_cookie_t tx_cookie, rx_cookie;
	unsigned long tx_completed, rx_completed;
	u32 dma_conf = readl(s->base + S31_SPI_DMA_CONF);
	unsigned int timeout_ms = spi_controller_xfer_timeout(ctlr, xfer);
	int ret;

	memcpy(s->tx_dma_buf, xfer->tx_buf, xfer->len);
	dma_sync_single_for_device(s->dev, s->tx_dma_addr, xfer->len,
				   DMA_TO_DEVICE);
	dma_sync_single_for_device(s->dev, s->rx_dma_addr, xfer->len,
				   DMA_FROM_DEVICE);
	config.src_addr = s->fifo_addr;
	config.dst_addr = s->fifo_addr;
	config.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
	config.src_maxburst = 8;
	config.dst_maxburst = 8;
	config.direction = DMA_MEM_TO_DEV;
	ret = dmaengine_slave_config(s->tx_dma, &config);
	if (ret)
		return ret;
	config.direction = DMA_DEV_TO_MEM;
	ret = dmaengine_slave_config(s->rx_dma, &config);
	if (ret)
		return ret;

	tx_desc = dmaengine_prep_slave_single(s->tx_dma, s->tx_dma_addr,
					      xfer->len, DMA_MEM_TO_DEV,
					      DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	rx_desc = dmaengine_prep_slave_single(s->rx_dma,
					      s->rx_dma_addr,
					      xfer->len, DMA_DEV_TO_MEM,
					      DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
	if (!tx_desc || !rx_desc)
		return -ENOMEM;
	reinit_completion(&s->dma_tx_done);
	reinit_completion(&s->dma_rx_done);
	tx_desc->callback = esp32s31_spi_dma_complete;
	tx_desc->callback_param = &s->dma_tx_done;
	rx_desc->callback = esp32s31_spi_dma_complete;
	rx_desc->callback_param = &s->dma_rx_done;
	rx_cookie = dmaengine_submit(rx_desc);
	ret = dma_submit_error(rx_cookie);
	if (ret)
		return ret;
	tx_cookie = dmaengine_submit(tx_desc);
	ret = dma_submit_error(tx_cookie);
	if (ret)
		goto terminate;

	dma_conf &= ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN);
	writel(dma_conf | S31_SPI_RX_AFIFO_RST | S31_SPI_DMA_AFIFO_RST,
	       s->base + S31_SPI_DMA_CONF);
	writel(dma_conf, s->base + S31_SPI_DMA_CONF);
	writel(S31_SPI_MST_RX_AFIFO_WFULL_ERR |
	       S31_SPI_MST_TX_AFIFO_REMPTY_ERR,
	       s->base + S31_SPI_INT_CLR);
	dma_conf |= S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN;
	writel(dma_conf, s->base + S31_SPI_DMA_CONF);
	writel(misc, s->base + S31_SPI_MISC);
	writel(xfer->len * 8 - 1, s->base + S31_SPI_MS_DLEN);
	/* Match IDF: arm the receive path before data can enter the TX path. */
	dma_async_issue_pending(s->rx_dma);
	dma_async_issue_pending(s->tx_dma);
	ret = esp32s31_spi_wait_transaction(s, timeout_ms);
	if (ret)
		goto terminate;
	rx_completed = wait_for_completion_timeout(&s->dma_rx_done,
						   msecs_to_jiffies(timeout_ms));
	tx_completed = wait_for_completion_timeout(&s->dma_tx_done,
						   msecs_to_jiffies(timeout_ms));
	if (!rx_completed || !tx_completed) {
		ret = -ETIMEDOUT;
		goto terminate;
	}
	dma_sync_single_for_cpu(s->dev, s->rx_dma_addr, xfer->len,
				DMA_FROM_DEVICE);
	memcpy(xfer->rx_buf, s->rx_dma_buf, xfer->len);
	writel(dma_conf & ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN),
	       s->base + S31_SPI_DMA_CONF);
	return 0;

terminate:
	dmaengine_terminate_sync(s->rx_dma);
	dmaengine_terminate_sync(s->tx_dma);
	writel(dma_conf & ~(S31_SPI_DMA_RX_EN | S31_SPI_DMA_TX_EN),
	       s->base + S31_SPI_DMA_CONF);
	return ret;
}

static int esp32s31_spi_transfer_one(struct spi_controller *ctlr,
				     struct spi_device *spi,
				     struct spi_transfer *xfer)
{
	struct esp32s31_spi *s = spi_controller_get_devdata(ctlr);
	u32 ctrl, user, misc;
	u32 cs = spi_get_chipselect(spi, 0);
	bool gpio_cs = spi_get_csgpiod(spi, 0);
	u32 word;
	unsigned int i, count, offset;
	int ret;

	if (s->target)
		return esp32s31_spi_target_transfer(ctlr, spi, xfer);

	if (!xfer->len || xfer->len > esp32s31_spi_max_transfer_size(spi))
		return xfer->len ? -EMSGSIZE : 0;
	/* Multi-line SPI is half duplex on this controller. */
	if (xfer->tx_buf && xfer->rx_buf &&
	    (xfer->tx_nbits > 1 || xfer->rx_nbits > 1))
		return -EINVAL;
	if (xfer->tx_nbits > s->max_data_lines ||
	    xfer->rx_nbits > s->max_data_lines)
		return -EINVAL;

	xfer->effective_speed_hz = esp32s31_spi_set_clock(s,
					xfer->speed_hz ?: spi->max_speed_hz);

	ctrl = readl(s->base + S31_SPI_CTRL);
	ctrl &= ~(S31_SPI_CTRL_RD_LSB | S31_SPI_CTRL_WR_LSB |
		  S31_SPI_CTRL_FREAD_DUAL | S31_SPI_CTRL_FREAD_QUAD |
		  S31_SPI_CTRL_FREAD_OCT | S31_SPI_CTRL_DATA_IDLE);
	if (spi->mode & SPI_LSB_FIRST)
		ctrl |= FIELD_PREP(S31_SPI_CTRL_RD_LSB, 1) |
			FIELD_PREP(S31_SPI_CTRL_WR_LSB, 1);
	if (xfer->rx_nbits == 2)
		ctrl |= S31_SPI_CTRL_FREAD_DUAL;
	else if (xfer->rx_nbits == 4)
		ctrl |= S31_SPI_CTRL_FREAD_QUAD;
	else if (xfer->rx_nbits == 8)
		ctrl |= S31_SPI_CTRL_FREAD_OCT;
	writel(ctrl, s->base + S31_SPI_CTRL);

	user = S31_SPI_CS_HOLD;
	if (xfer->tx_buf)
		user |= S31_SPI_USR_MOSI;
	if (xfer->rx_buf)
		user |= S31_SPI_USR_MISO;
	if (xfer->tx_buf && xfer->rx_buf)
		user |= S31_SPI_DOUTDIN;
	if (!!(spi->mode & SPI_CPHA) != !!(spi->mode & SPI_CPOL))
		user |= S31_SPI_CK_OUT_EDGE;
	if (xfer->tx_nbits == 2)
		user |= S31_SPI_FWRITE_DUAL;
	else if (xfer->tx_nbits == 4)
		user |= S31_SPI_FWRITE_QUAD;
	else if (xfer->tx_nbits == 8)
		user |= S31_SPI_FWRITE_OCT;
	writel(user, s->base + S31_SPI_USER);
	/* Match IDF's zero-length phase programming.  The phases stay disabled in
	 * USER, while their encoded length fields contain (0 - 1). */
	writel((readl(s->base + S31_SPI_USER1) &
		~(S31_SPI_CS_SETUP_TIME | S31_SPI_ADDR_BITLEN)) |
	       S31_SPI_CS_SETUP_TIME | S31_SPI_ADDR_BITLEN,
	       s->base + S31_SPI_USER1);
	writel((readl(s->base + S31_SPI_USER2) &
		~S31_SPI_COMMAND_BITLEN) | S31_SPI_COMMAND_BITLEN,
	       s->base + S31_SPI_USER2);

	/*
	 * A descriptor-backed CS is owned by the SPI core.  Do not also enable
	 * the controller's native CS output: the matrix can retain an older CS
	 * route across dynamic pinctrl lifecycles, causing the native waveform to
	 * fight or replace the GPIO level while the data clock keeps running.
	 */
	/*
	 * The master state machine still needs one internal CS selected for its
	 * prepare/data/done timing even when gpiolib drives the physical CS.  Use
	 * the last native CS, which this pinctrl state never routes to a pad.
	 */
	misc = S31_SPI_CS_DIS_MASK &
		~BIT(gpio_cs ? ctlr->num_chipselect - 1 : cs);
	if (!gpio_cs && (spi->mode & SPI_CS_HIGH))
		misc |= BIT(cs + 7);
	if (spi->mode & SPI_CPOL)
		misc |= S31_SPI_CK_IDLE_EDGE;
	if (s->tx_dma && s->rx_dma && xfer->len > S31_SPI_FIFO_SIZE &&
	    (!!xfer->tx_buf != !!xfer->rx_buf))
		return esp32s31_spi_dma_transfer(s, ctlr, xfer, misc);
	if (s->tx_dma && s->rx_dma && xfer->len > S31_SPI_FIFO_SIZE &&
	    xfer->tx_buf && xfer->rx_buf)
		return esp32s31_spi_dma_duplex_transfer(s, ctlr, xfer, misc);

	for (offset = 0; offset < xfer->len; offset += S31_SPI_FIFO_SIZE) {
		unsigned int chunk = min_t(unsigned int, S31_SPI_FIFO_SIZE,
					       xfer->len - offset);
		bool keep_active = !gpio_cs && (offset + chunk < xfer->len ||
			(!xfer->cs_change && !spi_transfer_is_last(ctlr, xfer)));

		writel(misc | (keep_active ? S31_SPI_CS_KEEP_ACTIVE : 0),
		       s->base + S31_SPI_MISC);
		writel(chunk * 8 - 1, s->base + S31_SPI_MS_DLEN);
		/* Reset the CPU-controlled TX/RX FIFOs before refilling them. */
		writel(S31_SPI_SLV_RX_SEG_TRANS_CLR_EN |
		       S31_SPI_SLV_TX_SEG_TRANS_CLR_EN |
		       S31_SPI_RX_AFIFO_RST | S31_SPI_BUF_AFIFO_RST,
		       s->base + S31_SPI_DMA_CONF);
		writel(S31_SPI_SLV_RX_SEG_TRANS_CLR_EN |
		       S31_SPI_SLV_TX_SEG_TRANS_CLR_EN,
		       s->base + S31_SPI_DMA_CONF);
		if (xfer->tx_buf) {
			const u8 *tx = xfer->tx_buf;

			for (i = 0; i < chunk; i += 4) {
				word = 0;
				count = min_t(unsigned int, 4, chunk - i);
				memcpy(&word, tx + offset + i, count);
				writel(word, s->base + S31_SPI_W0 + i);
			}
		}

		ret = esp32s31_spi_wait_transaction(s,
				spi_controller_xfer_timeout(ctlr, xfer));
		if (ret)
			return ret;

		if (xfer->rx_buf) {
			u8 *rx = xfer->rx_buf;

			for (i = 0; i < chunk; i += 4) {
				word = readl(s->base + S31_SPI_W0 + i);
				count = min_t(unsigned int, 4, chunk - i);
				memcpy(rx + offset + i, &word, count);
			}
		}
	}

	return 0;
}

static int esp32s31_spi_setup(struct spi_device *spi)
{
	struct esp32s31_spi *s = spi_controller_get_devdata(spi->controller);
	struct gpio_desc *cs = spi_get_csgpiod(spi, 0);

	if (!cs || !s->gpio_cs_drive_strength)
		return 0;
	return gpiod_set_config(cs, pinconf_to_config_packed(
				PIN_CONFIG_DRIVE_STRENGTH,
				s->gpio_cs_drive_strength));
}

static void esp32s31_spi_free_dma_buffer(void *data)
{
	struct esp32s31_spi *s = data;

	if (s->tx_dma_buf)
		dma_free_noncoherent(s->dev, SZ_4K, s->tx_dma_buf,
				     s->tx_dma_addr, DMA_TO_DEVICE);
	if (s->rx_dma_buf)
		dma_free_noncoherent(s->dev, SZ_4K, s->rx_dma_buf,
				     s->rx_dma_addr, DMA_FROM_DEVICE);
}

static int esp32s31_spi_probe(struct platform_device *pdev)
{
	struct spi_controller *ctlr;
	struct esp32s31_spi *s;
	struct reset_control *rst;
	struct resource *res;
	u32 num_chipselect = 3;
	bool target;
	int irq, ret;

	target = device_property_read_bool(&pdev->dev, "spi-slave");
	if (target)
		ctlr = devm_spi_alloc_target(&pdev->dev, sizeof(*s));
	else
		ctlr = devm_spi_alloc_host(&pdev->dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;
	ctlr->dev.of_node = pdev->dev.of_node;
	s = spi_controller_get_devdata(ctlr);
	s->dev = &pdev->dev;
	s->target = target;
	s->max_data_lines = 4;
	device_property_read_u32(&pdev->dev, "espressif,max-data-lines",
				 &s->max_data_lines);
	if (s->max_data_lines != 4 && s->max_data_lines != 8)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "max-data-lines must be 4 or 8\n");
	device_property_read_u32(&pdev->dev,
				 "espressif,gpio-cs-drive-strength",
				 &s->gpio_cs_drive_strength);
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res)
		return -ENODEV;
	s->fifo_addr = res->start + S31_SPI_W0;
	s->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(s->base))
		return PTR_ERR(s->base);
	s->clk = devm_clk_get_enabled(&pdev->dev, NULL);
	if (IS_ERR(s->clk))
		return PTR_ERR(s->clk);
	rst = devm_reset_control_get_optional_exclusive(&pdev->dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(&pdev->dev, PTR_ERR(rst),
				     "reset unavailable\n");
	/*
	 * Annoyedmilk's reset controller only implements assert/deassert
	 * (no .reset op, like its I2C driver uses), so reset_control_reset()
	 * fails here with -ENOTSUPP (-524). Assert then deassert is the
	 * same pulse, and matches drivers/i2c/busses/i2c-esp32s31.c.
	 */
	ret = reset_control_assert(rst);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "reset failed\n");
	ret = reset_control_deassert(rst);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "reset failed\n");
	irq = platform_get_irq(pdev, 0);
	s->irq = irq;
	if (irq < 0)
		return irq;
	init_completion(&s->done);
	init_completion(&s->dma_tx_done);
	init_completion(&s->dma_rx_done);
	ret = devm_request_irq(&pdev->dev, irq, esp32s31_spi_irq, 0,
			       dev_name(&pdev->dev), s);
	if (ret)
		return ret;
	s->tx_dma = devm_dma_request_chan(&pdev->dev, "tx");
	if (IS_ERR(s->tx_dma)) {
		ret = PTR_ERR(s->tx_dma);
		if (ret == -ENODEV)
			s->tx_dma = NULL;
		else
			return dev_err_probe(&pdev->dev, ret,
					     "TX DMA channel unavailable\n");
	}
	s->rx_dma = devm_dma_request_chan(&pdev->dev, "rx");
	if (IS_ERR(s->rx_dma)) {
		ret = PTR_ERR(s->rx_dma);
		if (ret == -ENODEV)
			s->rx_dma = NULL;
		else
			return dev_err_probe(&pdev->dev, ret,
					     "RX DMA channel unavailable\n");
	}
	if (!!s->tx_dma != !!s->rx_dma)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "both TX and RX DMA channels are required\n");
	if (s->tx_dma) {
		s->tx_dma_buf = dma_alloc_noncoherent(&pdev->dev, SZ_4K,
						     &s->tx_dma_addr,
						     DMA_TO_DEVICE,
						     GFP_KERNEL);
		if (!s->tx_dma_buf)
			return -ENOMEM;
		ret = devm_add_action_or_reset(&pdev->dev,
					       esp32s31_spi_free_dma_buffer, s);
		if (ret)
			return ret;
		s->rx_dma_buf = dma_alloc_noncoherent(&pdev->dev, SZ_4K,
						     &s->rx_dma_addr,
						     DMA_FROM_DEVICE,
						     GFP_KERNEL);
		if (!s->rx_dma_buf)
			return -ENOMEM;
	}

	/* CPU FIFO mode; leave all DMA paths disabled. */
	writel(target ? S31_SPI_SLAVE_MODE : 0, s->base + S31_SPI_SLAVE);
	writel(0, s->base + S31_SPI_USER);
	/* Match spi_ll_master_init(): clear CS timing and AFIFO-error endings. */
	writel(0, s->base + S31_SPI_USER1);
	writel(0, s->base + S31_SPI_USER2);
	writel(0, s->base + S31_SPI_INT_ENA);
	writel(~0U, s->base + S31_SPI_INT_CLR);
	writel(target ? 0 : (S31_SPI_SLV_RX_SEG_TRANS_CLR_EN |
			      S31_SPI_SLV_TX_SEG_TRANS_CLR_EN),
	       s->base + S31_SPI_DMA_CONF);
	/* The clock provider owns the S31 GPSPI HS/MST clock path. */
	writel(0, s->base + S31_SPI_CLK_GATE);

	ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_LSB_FIRST |
			  SPI_TX_DUAL | SPI_RX_DUAL | SPI_TX_QUAD | SPI_RX_QUAD;
	if (s->max_data_lines == 8)
		ctlr->mode_bits |= SPI_TX_OCTAL | SPI_RX_OCTAL;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	if (target) {
		ctlr->bits_per_word_mask |= SPI_BPW_MASK(16) | SPI_BPW_MASK(32);
		/* Only advertise the target wire modes configured above. */
		ctlr->mode_bits = SPI_CPOL | SPI_CPHA | SPI_CS_HIGH | SPI_LSB_FIRST;
	}
	/* Allow boards to use a GPIO chip select when the native CS output is
	 * unsuitable for a matrix-routed or long-wire fixture.  The SPI core owns
	 * assertion across the complete message, including our FIFO chunks. */
	ctlr->use_gpio_descriptors = true;
	device_property_read_u32(&pdev->dev, "num-cs", &num_chipselect);
	if (target)
		num_chipselect = 1;
	if (!num_chipselect || num_chipselect > 6)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "num-cs must be between 1 and 6\n");
	ctlr->num_chipselect = num_chipselect;
	ctlr->min_speed_hz = DIV_ROUND_UP(clk_get_rate(s->clk), 16 * 64);
	ctlr->max_speed_hz = clk_get_rate(s->clk);
	ctlr->max_transfer_size = esp32s31_spi_max_transfer_size;
	ctlr->setup = esp32s31_spi_setup;
	ctlr->transfer_one = esp32s31_spi_transfer_one;
	if (target)
		ctlr->target_abort = esp32s31_spi_target_abort;

	return devm_spi_register_controller(&pdev->dev, ctlr);
}

static const struct of_device_id esp32s31_spi_of_match[] = {
	{ .compatible = "espressif,esp32s31-gpspi" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_spi_of_match);

static struct platform_driver esp32s31_spi_driver = {
	.probe = esp32s31_spi_probe,
	.driver = {
		.name = "esp32s31-gpspi",
		.of_match_table = esp32s31_spi_of_match,
	},
};
module_platform_driver(esp32s31_spi_driver);

MODULE_DESCRIPTION("ESP32-S31 GPSPI host and target driver");
MODULE_LICENSE("GPL");

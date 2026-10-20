// SPDX-License-Identifier: GPL-2.0
/*
 * Author: Marco Mueller <hello@annoyedmilk.ch>
 *
 * ESP32-S31 I2C master
 *
 * The controller runs a list of up to 8 commands (RSTART, WRITE, READ, STOP,
 * END) with a 32-byte FIFO for each direction.  The driver puts as much of
 * the messages as fits into one list.  A list that ends with END pauses the
 * bus with SCL low, and the interrupt handler then reads the received bytes
 * and starts the next list.  The last list ends with STOP.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/log2.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#define I2C_SCL_LOW_PERIOD		0x00
#define I2C_CTR				0x04
#define I2C_TO				0x0c
#define I2C_FIFO_CONF			0x18
#define I2C_DATA			0x1c
#define I2C_INT_CLR			0x24
#define I2C_INT_ENA			0x28
#define I2C_INT_STATUS			0x2c
#define I2C_SDA_HOLD			0x30
#define I2C_SDA_SAMPLE			0x34
#define I2C_SCL_HIGH_PERIOD		0x38
#define I2C_SCL_START_HOLD		0x40
#define I2C_SCL_RSTART_SETUP		0x44
#define I2C_SCL_STOP_HOLD		0x48
#define I2C_SCL_STOP_SETUP		0x4c
#define I2C_FILTER_CFG			0x50
#define I2C_COMD(n)			(0x58 + (n) * 4)
#define I2C_SCL_SP_CONF			0x80

#define I2C_CTR_MS_MODE			BIT(4)
#define I2C_CTR_TRANS_START		BIT(5)
#define I2C_CTR_FSM_RST			BIT(10)
#define I2C_CTR_CONF_UPGATE		BIT(11)

#define I2C_TO_VALUE			GENMASK(4, 0)
#define I2C_TO_EN			BIT(5)

#define I2C_FIFO_RX_RST			BIT(12)
#define I2C_FIFO_TX_RST			BIT(13)

#define I2C_SCL_WAIT_HIGH		GENMASK(15, 9)

#define I2C_FILTER_SCL_THRES		GENMASK(3, 0)
#define I2C_FILTER_SDA_THRES		GENMASK(7, 4)
#define I2C_FILTER_SCL_EN		BIT(8)
#define I2C_FILTER_SDA_EN		BIT(9)

#define I2C_SCL_RST_SLV_EN		BIT(0)
#define I2C_SCL_RST_SLV_NUM		GENMASK(5, 1)

#define I2C_INT_END_DETECT		BIT(3)
#define I2C_INT_ARB_LOST		BIT(5)
#define I2C_INT_TRANS_COMPLETE		BIT(7)
#define I2C_INT_TIME_OUT		BIT(8)
#define I2C_INT_NACK			BIT(10)
#define I2C_INT_SCL_ST_TO		BIT(13)
#define I2C_INT_SCL_MAIN_ST_TO		BIT(14)
#define I2C_INT_TIMEOUTS		(I2C_INT_TIME_OUT | I2C_INT_SCL_ST_TO | \
					 I2C_INT_SCL_MAIN_ST_TO)
#define I2C_INT_MASK			(I2C_INT_END_DETECT | I2C_INT_ARB_LOST | \
					 I2C_INT_TRANS_COMPLETE | I2C_INT_NACK | \
					 I2C_INT_TIMEOUTS)

#define I2C_CMD_BYTES			GENMASK(7, 0)
#define I2C_CMD_ACK_CHECK		BIT(8)
#define I2C_CMD_ACK_VALUE		BIT(10)
#define I2C_CMD_OP			GENMASK(13, 11)
#define I2C_OP_WRITE			1
#define I2C_OP_STOP			2
#define I2C_OP_READ			3
#define I2C_OP_END			4
#define I2C_OP_RSTART			6

#define I2C_NCMDS			8
#define I2C_FIFO_SIZE			32
/* Glitches shorter than this many source clock cycles are filtered. */
#define I2C_FILTER_CYCLES		7

struct esp32s31_i2c_rx {
	u8 *buf;
	unsigned int len;
};

struct esp32s31_i2c {
	struct i2c_adapter adap;
	struct device *dev;
	void __iomem *base;
	struct clk *clk;
	struct reset_control *rst;
	u32 bus_freq;
	struct completion done;
	int err;

	/* The messages of the transfer, and the next byte to send or receive */
	struct i2c_msg *msgs;
	unsigned int num;
	unsigned int msg;
	unsigned int pos;
	bool addressed;

	/* The read commands of the running list */
	struct esp32s31_i2c_rx rx[I2C_NCMDS];
	unsigned int nrx;
};

static void esp32s31_i2c_cmd(struct esp32s31_i2c *i2c, unsigned int *n,
			     u32 op, u32 bytes, u32 flags)
{
	writel(FIELD_PREP(I2C_CMD_OP, op) | FIELD_PREP(I2C_CMD_BYTES, bytes) |
	       flags, i2c->base + I2C_COMD((*n)++));
}

static void esp32s31_i2c_next_msg(struct esp32s31_i2c *i2c)
{
	i2c->msg++;
	i2c->pos = 0;
	i2c->addressed = false;
}

/* The commands and FIFO bytes that the running list uses */
struct esp32s31_i2c_batch {
	unsigned int cmd;
	unsigned int tx;
	unsigned int rx;
};

/* A RESTART and the address, with room for one data command after them */
static bool esp32s31_i2c_queue_addr(struct esp32s31_i2c *i2c,
				    struct esp32s31_i2c_batch *b,
				    struct i2c_msg *m)
{
	if (b->cmd + 3 > I2C_NCMDS - 1 || b->tx == I2C_FIFO_SIZE)
		return false;

	esp32s31_i2c_cmd(i2c, &b->cmd, I2C_OP_RSTART, 0, 0);
	writel(i2c_8bit_addr_from_msg(m), i2c->base + I2C_DATA);
	esp32s31_i2c_cmd(i2c, &b->cmd, I2C_OP_WRITE, 1, I2C_CMD_ACK_CHECK);
	b->tx++;
	i2c->addressed = true;
	return true;
}

/* The last byte of a message gets a NACK, so it is a READ of its own. */
static bool esp32s31_i2c_queue_read(struct esp32s31_i2c *i2c,
				    struct esp32s31_i2c_batch *b,
				    struct i2c_msg *m)
{
	unsigned int left = m->len - i2c->pos;
	unsigned int n = min(left, I2C_FIFO_SIZE - b->rx);
	bool last = n == left;
	unsigned int bulk = n - last;

	if (!n || b->cmd + !!bulk + last > I2C_NCMDS - 1)
		return false;

	i2c->rx[i2c->nrx++] = (struct esp32s31_i2c_rx){ m->buf + i2c->pos, n };
	if (bulk)
		esp32s31_i2c_cmd(i2c, &b->cmd, I2C_OP_READ, bulk, 0);
	if (last)
		esp32s31_i2c_cmd(i2c, &b->cmd, I2C_OP_READ, 1,
				 I2C_CMD_ACK_VALUE);
	b->rx += n;
	i2c->pos += n;
	return true;
}

static bool esp32s31_i2c_queue_write(struct esp32s31_i2c *i2c,
				     struct esp32s31_i2c_batch *b,
				     struct i2c_msg *m)
{
	unsigned int n = min(m->len - i2c->pos, I2C_FIFO_SIZE - b->tx);

	if (!n || b->cmd + 1 > I2C_NCMDS - 1)
		return false;

	esp32s31_i2c_cmd(i2c, &b->cmd, I2C_OP_WRITE, n, I2C_CMD_ACK_CHECK);
	b->tx += n;
	while (n--)
		writel(m->buf[i2c->pos++], i2c->base + I2C_DATA);
	return true;
}

/*
 * Put the next part of the messages into the command list.  One command is
 * kept for the END or STOP.
 */
static void esp32s31_i2c_fill(struct esp32s31_i2c *i2c)
{
	struct esp32s31_i2c_batch b = { };

	i2c->nrx = 0;
	while (i2c->msg < i2c->num) {
		struct i2c_msg *m = &i2c->msgs[i2c->msg];
		bool queued;

		if (!i2c->addressed)
			queued = esp32s31_i2c_queue_addr(i2c, &b, m);
		else if (m->flags & I2C_M_RD)
			queued = esp32s31_i2c_queue_read(i2c, &b, m);
		else
			queued = esp32s31_i2c_queue_write(i2c, &b, m);
		if (!queued)
			break;
		if (i2c->pos == m->len)
			esp32s31_i2c_next_msg(i2c);
	}

	esp32s31_i2c_cmd(i2c, &b.cmd, i2c->msg < i2c->num ? I2C_OP_END :
			 I2C_OP_STOP, 0, 0);
}

static void esp32s31_i2c_start(struct esp32s31_i2c *i2c)
{
	writel(readl(i2c->base + I2C_CTR) | I2C_CTR_TRANS_START,
	       i2c->base + I2C_CTR);
}

static void esp32s31_i2c_drain(struct esp32s31_i2c *i2c)
{
	unsigned int i, j;

	for (i = 0; i < i2c->nrx; i++)
		for (j = 0; j < i2c->rx[i].len; j++)
			i2c->rx[i].buf[j] = readl(i2c->base + I2C_DATA);
	i2c->nrx = 0;
}

static irqreturn_t esp32s31_i2c_irq(int irq, void *dev_id)
{
	struct esp32s31_i2c *i2c = dev_id;
	u32 status = readl(i2c->base + I2C_INT_STATUS);

	if (!status)
		return IRQ_NONE;

	writel(status, i2c->base + I2C_INT_CLR);

	if (status & I2C_INT_NACK)
		i2c->err = -ENXIO;
	else if (status & I2C_INT_ARB_LOST)
		i2c->err = -EAGAIN;
	else if (status & I2C_INT_TIMEOUTS)
		i2c->err = -ETIMEDOUT;

	if (!i2c->err && (status & I2C_INT_END_DETECT)) {
		esp32s31_i2c_drain(i2c);
		esp32s31_i2c_fill(i2c);
		esp32s31_i2c_start(i2c);
		return IRQ_HANDLED;
	}

	if (i2c->err || (status & I2C_INT_TRANS_COMPLETE)) {
		if (!i2c->err)
			esp32s31_i2c_drain(i2c);
		writel(0, i2c->base + I2C_INT_ENA);
		complete(&i2c->done);
	}

	return IRQ_HANDLED;
}

static void esp32s31_i2c_reset_fifos(struct esp32s31_i2c *i2c)
{
	u32 conf = readl(i2c->base + I2C_FIFO_CONF);

	writel(conf | I2C_FIFO_RX_RST | I2C_FIFO_TX_RST,
	       i2c->base + I2C_FIFO_CONF);
	writel(conf & ~(I2C_FIFO_RX_RST | I2C_FIFO_TX_RST),
	       i2c->base + I2C_FIFO_CONF);
}

/*
 * The timing registers count source clock cycles.  half is half an SCL
 * period.  SCL stays low for half; after the controller releases SCL, it
 * waits up to WAIT_HIGH for the rise, then counts HIGH_PERIOD.
 */
static void esp32s31_i2c_set_timing(struct esp32s31_i2c *i2c)
{
	u32 half = DIV_ROUND_UP(clk_get_rate(i2c->clk), 2 * i2c->bus_freq);
	u32 wait = i2c->bus_freq >= 80000 ? half / 2 - 2 : half / 4;
	void __iomem *base = i2c->base;

	writel(half - 1, base + I2C_SCL_LOW_PERIOD);
	writel((half - wait) | FIELD_PREP(I2C_SCL_WAIT_HIGH, wait),
	       base + I2C_SCL_HIGH_PERIOD);
	writel(half / 4 - 1, base + I2C_SDA_HOLD);
	writel(half / 2 - 1, base + I2C_SDA_SAMPLE);
	writel(half - 1, base + I2C_SCL_START_HOLD);
	writel(half - 1, base + I2C_SCL_RSTART_SETUP);
	writel(half - 1, base + I2C_SCL_STOP_HOLD);
	writel(half - 1, base + I2C_SCL_STOP_SETUP);
	/* 2^n source clock cycles: approximately 10 SCL periods */
	writel(FIELD_PREP(I2C_TO_VALUE, order_base_2(20 * half)) | I2C_TO_EN,
	       base + I2C_TO);
}

/*
 * Reset the controller and set it up.  After an error, also give 9 SCL
 * pulses to free a bus that a device holds.  Not on an idle bus: the GT1151
 * touch controller NACKs a transfer right after the pulses.
 */
static int esp32s31_i2c_init(struct esp32s31_i2c *i2c, bool clear_bus)
{
	void __iomem *base = i2c->base;
	u32 val;
	int ret;

	ret = reset_control_assert(i2c->rst);
	if (ret)
		return ret;
	ret = reset_control_deassert(i2c->rst);
	if (ret)
		return ret;

	/* Open drain, no arbitration, MSB first, ACK at a full RX FIFO */
	writel(I2C_CTR_MS_MODE, base + I2C_CTR);
	writel(FIELD_PREP(I2C_FILTER_SCL_THRES, I2C_FILTER_CYCLES) |
	       FIELD_PREP(I2C_FILTER_SDA_THRES, I2C_FILTER_CYCLES) |
	       I2C_FILTER_SCL_EN | I2C_FILTER_SDA_EN, base + I2C_FILTER_CFG);
	esp32s31_i2c_set_timing(i2c);
	writel(I2C_INT_MASK, base + I2C_INT_CLR);
	writel(0, base + I2C_INT_ENA);
	writel(I2C_CTR_MS_MODE | I2C_CTR_CONF_UPGATE, base + I2C_CTR);
	if (!clear_bus)
		return 0;

	/* The pulses start only in an idle master, so after the update above. */
	writel(I2C_SCL_RST_SLV_EN | FIELD_PREP(I2C_SCL_RST_SLV_NUM, 9),
	       base + I2C_SCL_SP_CONF);
	writel(I2C_CTR_MS_MODE | I2C_CTR_CONF_UPGATE, base + I2C_CTR);
	return readl_poll_timeout(base + I2C_SCL_SP_CONF, val,
				  !(val & I2C_SCL_RST_SLV_EN), 10, 10000);
}

static int esp32s31_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			     int num)
{
	struct esp32s31_i2c *i2c = i2c_get_adapdata(adap);
	int i;

	for (i = 0; i < num; i++)
		if (msgs[i].flags & I2C_M_TEN)
			return -EOPNOTSUPP;

	i2c->msgs = msgs;
	i2c->num = num;
	i2c->msg = 0;
	i2c->pos = 0;
	i2c->addressed = false;
	i2c->err = 0;
	reinit_completion(&i2c->done);

	esp32s31_i2c_reset_fifos(i2c);
	writel(I2C_INT_MASK, i2c->base + I2C_INT_CLR);
	esp32s31_i2c_fill(i2c);
	writel(I2C_INT_MASK, i2c->base + I2C_INT_ENA);
	esp32s31_i2c_start(i2c);

	if (!wait_for_completion_timeout(&i2c->done, adap->timeout)) {
		writel(0, i2c->base + I2C_INT_ENA);
		i2c->err = -ETIMEDOUT;
	}

	/* After a NACK the bus is free: a reset of the state machine is enough. */
	if (i2c->err == -ENXIO) {
		writel(readl(i2c->base + I2C_CTR) | I2C_CTR_FSM_RST,
		       i2c->base + I2C_CTR);
	} else if (i2c->err && esp32s31_i2c_init(i2c, true)) {
		dev_err(i2c->dev, "failed to reset the controller\n");
	}

	return i2c->err ?: num;
}

static u32 esp32s31_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm esp32s31_i2c_algo = {
	.xfer = esp32s31_i2c_xfer,
	.functionality = esp32s31_i2c_func,
};

static int esp32s31_i2c_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct i2c_timings timings;
	struct esp32s31_i2c *i2c;
	unsigned long rate;
	struct clk *apb;
	int irq, ret;

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->dev = dev;
	i2c->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	apb = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(apb))
		return dev_err_probe(dev, PTR_ERR(apb), "failed to get APB clock\n");

	i2c->clk = devm_clk_get_enabled(dev, "core");
	if (IS_ERR(i2c->clk))
		return dev_err_probe(dev, PTR_ERR(i2c->clk),
				     "failed to get core clock\n");

	i2c->rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(i2c->rst))
		return dev_err_probe(dev, PTR_ERR(i2c->rst),
				     "failed to get reset\n");

	i2c_parse_fw_timings(dev, &timings, true);
	i2c->bus_freq = timings.bus_freq_hz;
	if (!i2c->bus_freq || i2c->bus_freq > I2C_MAX_FAST_MODE_PLUS_FREQ)
		return dev_err_probe(dev, -EINVAL, "invalid clock-frequency\n");

	/* The 9-bit timing fields need 8 to 1023 source cycles per period. */
	ret = clk_set_rate(i2c->clk, i2c->bus_freq * 1023);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set the clock\n");
	rate = clk_get_rate(i2c->clk);
	if (rate < i2c->bus_freq * 8 || rate > i2c->bus_freq * 1023)
		return dev_err_probe(dev, -EINVAL,
				     "core clock %lu Hz out of range\n", rate);

	init_completion(&i2c->done);
	ret = esp32s31_i2c_init(i2c, false);
	if (ret)
		return dev_err_probe(dev, ret, "failed to reset the controller\n");

	ret = devm_request_irq(dev, irq, esp32s31_i2c_irq, 0, dev_name(dev),
			       i2c);
	if (ret)
		return ret;

	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &esp32s31_i2c_algo;
	i2c->adap.dev.parent = dev;
	i2c->adap.dev.of_node = dev->of_node;
	strscpy(i2c->adap.name, dev_name(dev), sizeof(i2c->adap.name));
	i2c_set_adapdata(&i2c->adap, i2c);

	return devm_i2c_add_adapter(dev, &i2c->adap);
}

static const struct of_device_id esp32s31_i2c_of_match[] = {
	{ .compatible = "esp,esp32s31-i2c" },
	{ }
};
MODULE_DEVICE_TABLE(of, esp32s31_i2c_of_match);

static struct platform_driver esp32s31_i2c_driver = {
	.driver = {
		.name = "esp32s31-i2c",
		.of_match_table = esp32s31_i2c_of_match,
	},
	.probe = esp32s31_i2c_probe,
};
module_platform_driver(esp32s31_i2c_driver);

MODULE_DESCRIPTION("ESP32-S31 I2C master driver");
MODULE_AUTHOR("Marco Mueller <hello@annoyedmilk.ch>");
MODULE_LICENSE("GPL");

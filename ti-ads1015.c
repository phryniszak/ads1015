/*
 * ADS1015 - Texas Instruments Analog-to-Digital Converter
 *
 * Copyright (c) 2016, Intel Corporation.
 *
 * This file is subject to the terms and conditions of version 2 of
 * the GNU General Public License.  See the file COPYING in the main
 * directory of this archive for more details.
 *
 * IIO driver for ADS1015 ADC 7-bit I2C slave address:
 *	* 0x48 - ADDR connected to Ground
 *	* 0x49 - ADDR connected to Vdd
 *	* 0x4A - ADDR connected to SDA
 *	* 0x4B - ADDR connected to SCL
 */

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/i2c.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/interrupt.h>

#include <linux/platform_data/ads1015.h>

#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/kfifo_buf.h>

#define ADS1015_DRV_NAME "ads1015"
#define ADS1015_IRQ_NAME "ads1015_rdy"

#define ADS1015_CONV_REG 0x00
#define ADS1015_CFG_REG 0x01
#define ADS1015_LO_THRESH_REG 0x02
#define ADS1015_HI_THRESH_REG 0x03

#define ADS1015_CFG_COMP_QUE_SHIFT 0
#define ADS1015_CFG_COMP_LAT_SHIFT 2
#define ADS1015_CFG_COMP_POL_SHIFT 3
#define ADS1015_CFG_COMP_MODE_SHIFT 4
#define ADS1015_CFG_DR_SHIFT 5
#define ADS1015_CFG_MOD_SHIFT 8
#define ADS1015_CFG_PGA_SHIFT 9
#define ADS1015_CFG_MUX_SHIFT 12

#define ADS1015_CFG_COMP_QUE_MASK GENMASK(1, 0)
#define ADS1015_CFG_COMP_LAT_MASK BIT(2)
#define ADS1015_CFG_COMP_POL_MASK BIT(3)
#define ADS1015_CFG_COMP_MODE_MASK BIT(4)
#define ADS1015_CFG_DR_MASK GENMASK(7, 5)
#define ADS1015_CFG_MOD_MASK BIT(8)
#define ADS1015_CFG_PGA_MASK GENMASK(11, 9)
#define ADS1015_CFG_MUX_MASK GENMASK(14, 12)

/* Comparator queue and disable field */
#define ADS1015_CFG_COMP_DISABLE 3

/* Comparator polarity field */
#define ADS1015_CFG_COMP_POL_LOW 0
#define ADS1015_CFG_COMP_POL_HIGH 1

/* Comparator mode field */
#define ADS1015_CFG_COMP_MODE_TRAD 0
#define ADS1015_CFG_COMP_MODE_WINDOW 1

/* device operating modes */
#define ADS1015_CONTINUOUS 0
#define ADS1015_SINGLESHOT 1

#define ADS1015_SLEEP_DELAY_MS 2000
#define ADS1015_DEFAULT_PGA 2
#define ADS1015_DEFAULT_DATA_RATE 4
#define ADS1015_DEFAULT_CHAN 0

enum chip_ids
{
	ADS1015,
	ADS1115,
};

enum ads1015_channels
{
	ADS1015_AIN0_AIN1 = 0,
	ADS1015_AIN0_AIN3,
	ADS1015_AIN1_AIN3,
	ADS1015_AIN2_AIN3,
	ADS1015_AIN0,
	ADS1015_AIN1,
	ADS1015_AIN2,
	ADS1015_AIN3,
	ADS1015_TIMESTAMP,
};

static const unsigned int ads1015_data_rate[] = {
	128, 250, 490, 920, 1600, 2400, 3300, 3300};

static const unsigned int ads1115_data_rate[] = {
	8, 16, 32, 64, 128, 250, 475, 860};

/*
 * Translation from PGA bits to full-scale positive and negative input voltage
 * range in mV
 */
static int ads1015_fullscale_range[] = {
	6144, 4096, 2048, 1024, 512, 256, 256, 256};

#define ADS1015_V_CHAN(_chan, _addr)                        \
	{                                                       \
		.type = IIO_VOLTAGE,                                \
		.indexed = 1,                                       \
		.address = _addr,                                   \
		.channel = _chan,                                   \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |      \
							  BIT(IIO_CHAN_INFO_SCALE) |    \
							  BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index = _addr,                                \
		.scan_type = {                                      \
			.sign = 's',                                    \
			.realbits = 12,                                 \
			.storagebits = 16,                              \
			.shift = 4,                                     \
			.endianness = IIO_CPU,                          \
		},                                                  \
		.datasheet_name = "AIN" #_chan,                     \
	}

#define ADS1015_V_DIFF_CHAN(_chan, _chan2, _addr)           \
	{                                                       \
		.type = IIO_VOLTAGE,                                \
		.differential = 1,                                  \
		.indexed = 1,                                       \
		.address = _addr,                                   \
		.channel = _chan,                                   \
		.channel2 = _chan2,                                 \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |      \
							  BIT(IIO_CHAN_INFO_SCALE) |    \
							  BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index = _addr,                                \
		.scan_type = {                                      \
			.sign = 's',                                    \
			.realbits = 12,                                 \
			.storagebits = 16,                              \
			.shift = 4,                                     \
			.endianness = IIO_CPU,                          \
		},                                                  \
		.datasheet_name = "AIN" #_chan "-AIN" #_chan2,      \
	}

#define ADS1115_V_CHAN(_chan, _addr)                        \
	{                                                       \
		.type = IIO_VOLTAGE,                                \
		.indexed = 1,                                       \
		.address = _addr,                                   \
		.channel = _chan,                                   \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |      \
							  BIT(IIO_CHAN_INFO_SCALE) |    \
							  BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index = _addr,                                \
		.scan_type = {                                      \
			.sign = 's',                                    \
			.realbits = 16,                                 \
			.storagebits = 16,                              \
			.endianness = IIO_CPU,                          \
		},                                                  \
		.datasheet_name = "AIN" #_chan,                     \
	}

#define ADS1115_V_DIFF_CHAN(_chan, _chan2, _addr)           \
	{                                                       \
		.type = IIO_VOLTAGE,                                \
		.differential = 1,                                  \
		.indexed = 1,                                       \
		.address = _addr,                                   \
		.channel = _chan,                                   \
		.channel2 = _chan2,                                 \
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |      \
							  BIT(IIO_CHAN_INFO_SCALE) |    \
							  BIT(IIO_CHAN_INFO_SAMP_FREQ), \
		.scan_index = _addr,                                \
		.scan_type = {                                      \
			.sign = 's',                                    \
			.realbits = 16,                                 \
			.storagebits = 16,                              \
			.endianness = IIO_CPU,                          \
		},                                                  \
		.datasheet_name = "AIN" #_chan "-AIN" #_chan2,      \
	}

struct ads1015_data
{
	/* Underlying I2C / SPI bus adapter used to abstract
	 * slave register accesses
	 */
	struct regmap *regmap;
	/*
	 * Protects ADC ops, e.g: concurrent sysfs/buffered
	 * data reads, configuration updates
	 */
	struct mutex lock;
	struct ads1015_channel_data channel_data[ADS1015_CHANNELS];

	unsigned int *data_rate;

	/*
	 * Set to true when the ADC is switched to the continuous-conversion
	 * mode and exits from a power-down state.  This flag is used to avoid
	 * getting the stale result from the conversion register.
	 */
	bool conv_invalid;

	/*
	 * Optional interrupt line: negative or zero if not declared 
	 */
	int irq;

	s64 timestamp;

	bool use_buffer;
};

static bool ads1015_is_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg)
	{
	case ADS1015_CFG_REG:
	case ADS1015_LO_THRESH_REG:
	case ADS1015_HI_THRESH_REG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config ads1015_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = ADS1015_HI_THRESH_REG,
	.writeable_reg = ads1015_is_writeable_reg,
};

static const struct iio_chan_spec ads1015_channels[] = {
	ADS1015_V_DIFF_CHAN(0, 1, ADS1015_AIN0_AIN1),
	ADS1015_V_DIFF_CHAN(0, 3, ADS1015_AIN0_AIN3),
	ADS1015_V_DIFF_CHAN(1, 3, ADS1015_AIN1_AIN3),
	ADS1015_V_DIFF_CHAN(2, 3, ADS1015_AIN2_AIN3),
	ADS1015_V_CHAN(0, ADS1015_AIN0),
	ADS1015_V_CHAN(1, ADS1015_AIN1),
	ADS1015_V_CHAN(2, ADS1015_AIN2),
	ADS1015_V_CHAN(3, ADS1015_AIN3),
	IIO_CHAN_SOFT_TIMESTAMP(ADS1015_TIMESTAMP),
};

static const struct iio_chan_spec ads1115_channels[] = {
	ADS1115_V_DIFF_CHAN(0, 1, ADS1015_AIN0_AIN1),
	ADS1115_V_DIFF_CHAN(0, 3, ADS1015_AIN0_AIN3),
	ADS1115_V_DIFF_CHAN(1, 3, ADS1015_AIN1_AIN3),
	ADS1115_V_DIFF_CHAN(2, 3, ADS1015_AIN2_AIN3),
	ADS1115_V_CHAN(0, ADS1015_AIN0),
	ADS1115_V_CHAN(1, ADS1015_AIN1),
	ADS1115_V_CHAN(2, ADS1015_AIN2),
	ADS1115_V_CHAN(3, ADS1015_AIN3),
	IIO_CHAN_SOFT_TIMESTAMP(ADS1015_TIMESTAMP),
};

static int ads1015_set_power_state(struct ads1015_data *data, bool on)
{
	int ret;
	struct device *dev = regmap_get_device(data->regmap);

	dev_dbg(dev, "power state=%d", on);

	if (on)
	{
		ret = pm_runtime_get_sync(dev);
		if (ret < 0)
			pm_runtime_put_noidle(dev);
	}
	else
	{
		pm_runtime_mark_last_busy(dev);
		ret = pm_runtime_put_autosuspend(dev);
	}

	return ret < 0 ? ret : 0;
}

static int ads1015_buffer_preenable(struct iio_dev *indio_dev)
{
	// struct ads1015_data *data = iio_priv(indio_dev);
	// struct device *dev = regmap_get_device(data->regmap);
	// enable_irq(data->irq);

	return ads1015_set_power_state(iio_priv(indio_dev), true);
}

static int ads1015_buffer_postdisable(struct iio_dev *indio_dev)
{
	// struct ads1015_data *data = iio_priv(indio_dev);
	// struct device *dev = regmap_get_device(data->regmap);
	// disable_irq(data->irq);

	return ads1015_set_power_state(iio_priv(indio_dev), false);
}

static const struct iio_buffer_setup_ops ads1015_buffer_setup_ops = {
	/*
	 * iio_triggered_buffer_postenable:
	 * Generic function that simply attaches the pollfunc to the trigger.
	 * Replace this to mess with hardware state before we attach the
	 * trigger.
	 */
	.preenable = ads1015_buffer_preenable,
	/*
	 * iio_triggered_buffer_predisable:
	 * Generic function that simple detaches the pollfunc from the trigger.
	 * Replace this to put hardware state back again after the trigger is
	 * detached but before userspace knows we have disabled the ring.
	 */
	.postdisable = ads1015_buffer_postdisable,
	.validate_scan_mask = &iio_validate_scan_mask_onehot,
};

static int ads1015_get_adc_result(struct ads1015_data *data, int chan, int *val)
{
	int ret, pga, dr, dr_old, conv_time;
	unsigned int old, mask, cfg;

	if (chan < 0 || chan >= ADS1015_CHANNELS)
		return -EINVAL;

	ret = regmap_read(data->regmap, ADS1015_CFG_REG, &old);
	if (ret)
		return ret;

	pga = data->channel_data[chan].pga;
	dr = data->channel_data[chan].data_rate;
	mask = ADS1015_CFG_MUX_MASK | ADS1015_CFG_PGA_MASK |
		   ADS1015_CFG_DR_MASK;
	cfg = chan << ADS1015_CFG_MUX_SHIFT | pga << ADS1015_CFG_PGA_SHIFT |
		  dr << ADS1015_CFG_DR_SHIFT;

	cfg = (old & ~mask) | (cfg & mask);
	if (old != cfg)
	{
		ret = regmap_write(data->regmap, ADS1015_CFG_REG, cfg);
		if (ret)
			return ret;
		data->conv_invalid = true;
	}
	if (data->conv_invalid)
	{
		dr_old = (old & ADS1015_CFG_DR_MASK) >> ADS1015_CFG_DR_SHIFT;
		conv_time = DIV_ROUND_UP(USEC_PER_SEC, data->data_rate[dr_old]);
		conv_time += DIV_ROUND_UP(USEC_PER_SEC, data->data_rate[dr]);
		conv_time += conv_time / 10; /* 10% internal clock inaccuracy */
		usleep_range(conv_time, conv_time + 1);
		data->conv_invalid = false;
	}

	return regmap_read(data->regmap, ADS1015_CONV_REG, val);
}

static int ads1015_set_scale(struct ads1015_data *data,
							 struct iio_chan_spec const *chan,
							 int scale, int uscale)
{
	int i;
	int fullscale = div_s64((scale * 1000000LL + uscale) << (chan->scan_type.realbits - 1), 1000000);

	for (i = 0; i < ARRAY_SIZE(ads1015_fullscale_range); i++)
	{
		if (ads1015_fullscale_range[i] == fullscale)
		{
			data->channel_data[chan->address].pga = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int ads1015_set_data_rate(struct ads1015_data *data, int chan, int rate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ads1015_data_rate); i++)
	{
		if (data->data_rate[i] == rate)
		{
			data->channel_data[chan].data_rate = i;
			return 0;
		}
	}

	return -EINVAL;
}

static int ads1015_read_raw(struct iio_dev *indio_dev,
							struct iio_chan_spec const *chan, int *val,
							int *val2, long mask)
{
	int ret, idx;
	struct ads1015_data *data = iio_priv(indio_dev);

	mutex_lock(&data->lock);
	switch (mask)
	{
	case IIO_CHAN_INFO_RAW:
	{
		int shift;

		// cannot read directly if buffered capture enabled.
		if (iio_buffer_enabled(indio_dev))
		{
			ret = -EAGAIN;
			break;
		}

		shift = chan->scan_type.shift;

		ret = iio_device_claim_direct_mode(indio_dev);
		if (ret)
			break;

		ret = ads1015_set_power_state(data, true);
		if (ret < 0)
			goto release_direct;

		ret = ads1015_get_adc_result(data, chan->address, val);
		if (ret < 0)
		{
			ads1015_set_power_state(data, false);
			goto release_direct;
		}

		*val = sign_extend32(*val >> shift, 15 - shift);

		ret = ads1015_set_power_state(data, false);
		if (ret < 0)
			goto release_direct;

		ret = IIO_VAL_INT;
	release_direct:
		iio_device_release_direct_mode(indio_dev);
		break;
	}
	case IIO_CHAN_INFO_SCALE:
		idx = data->channel_data[chan->address].pga;
		*val = ads1015_fullscale_range[idx];
		*val2 = chan->scan_type.realbits - 1;
		ret = IIO_VAL_FRACTIONAL_LOG2;
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		idx = data->channel_data[chan->address].data_rate;
		*val = data->data_rate[idx];
		ret = IIO_VAL_INT;
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&data->lock);

	return ret;
}

static int ads1015_write_raw(struct iio_dev *indio_dev,
							 struct iio_chan_spec const *chan, int val,
							 int val2, long mask)
{
	struct ads1015_data *data = iio_priv(indio_dev);
	int ret;

	mutex_lock(&data->lock);
	switch (mask)
	{
	case IIO_CHAN_INFO_SCALE:
		ret = ads1015_set_scale(data, chan, val, val2);
		break;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = ads1015_set_data_rate(data, chan->address, val);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	mutex_unlock(&data->lock);

	return ret;
}

static IIO_CONST_ATTR_NAMED(ads1015_scale_available, scale_available,
							"3 2 1 0.5 0.25 0.125");
static IIO_CONST_ATTR_NAMED(ads1115_scale_available, scale_available,
							"0.1875 0.125 0.0625 0.03125 0.015625 0.007813");

static IIO_CONST_ATTR_NAMED(ads1015_sampling_frequency_available,
							sampling_frequency_available, "128 250 490 920 1600 2400 3300");
static IIO_CONST_ATTR_NAMED(ads1115_sampling_frequency_available,
							sampling_frequency_available, "8 16 32 64 128 250 475 860");

static struct attribute *ads1015_attributes[] = {
	&iio_const_attr_ads1015_scale_available.dev_attr.attr,
	&iio_const_attr_ads1015_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group ads1015_attribute_group = {
	.attrs = ads1015_attributes,
};

static struct attribute *ads1115_attributes[] = {
	&iio_const_attr_ads1115_scale_available.dev_attr.attr,
	&iio_const_attr_ads1115_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group ads1115_attribute_group = {
	.attrs = ads1115_attributes,
};

static const struct iio_info ads1015_info = {
	.read_raw = ads1015_read_raw,
	.write_raw = ads1015_write_raw,
	.attrs = &ads1015_attribute_group,
};

static const struct iio_info ads1115_info = {
	.read_raw = ads1015_read_raw,
	.write_raw = ads1015_write_raw,
	.attrs = &ads1115_attribute_group,
};

#ifdef CONFIG_OF
static int ads1015_get_channels_config_of(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ads1015_data *data = iio_priv(indio_dev);
	struct device_node *node;

	if (!client->dev.of_node ||
		!of_get_next_child(client->dev.of_node, NULL))
		return -EINVAL;

	for_each_child_of_node(client->dev.of_node, node)
	{
		u32 pval;
		unsigned int channel;
		unsigned int pga = ADS1015_DEFAULT_PGA;
		unsigned int data_rate = ADS1015_DEFAULT_DATA_RATE;

		if (of_property_read_u32(node, "reg", &pval))
		{
			dev_err(&client->dev, "invalid reg on %pOF\n",
					node);
			continue;
		}

		channel = pval;
		if (channel >= ADS1015_CHANNELS)
		{
			dev_err(&client->dev,
					"invalid channel index %d on %pOF\n",
					channel, node);
			continue;
		}

		if (!of_property_read_u32(node, "ti,gain", &pval))
		{
			pga = pval;
			if (pga > 6)
			{
				dev_err(&client->dev, "invalid gain on %pOF\n",
						node);
				of_node_put(node);
				return -EINVAL;
			}
		}

		if (!of_property_read_u32(node, "ti,datarate", &pval))
		{
			data_rate = pval;
			if (data_rate > 7)
			{
				dev_err(&client->dev,
						"invalid data_rate on %pOF\n",
						node);
				of_node_put(node);
				return -EINVAL;
			}
		}

		data->channel_data[channel].pga = pga;
		data->channel_data[channel].data_rate = data_rate;
		dev_dbg(&client->dev, "channel=%d pga=%d data_rate=%d", channel, pga, data_rate);
	}

	return 0;
}
#endif

static void ads1015_get_channels_config(struct i2c_client *client)
{
	unsigned int k;

	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ads1015_data *data = iio_priv(indio_dev);
	struct ads1015_platform_data *pdata = dev_get_platdata(&client->dev);

	/* prefer platform data */
	if (pdata)
	{
		memcpy(data->channel_data, pdata->channel_data,
			   sizeof(data->channel_data));
		return;
	}

#ifdef CONFIG_OF
	if (!ads1015_get_channels_config_of(client))
		return;
#endif
	/* fallback on default configuration */
	for (k = 0; k < ADS1015_CHANNELS; ++k)
	{
		data->channel_data[k].pga = ADS1015_DEFAULT_PGA;
		data->channel_data[k].data_rate = ADS1015_DEFAULT_DATA_RATE;
	}
}

static int ads1015_set_conv_mode(struct ads1015_data *data, int mode)
{
	return regmap_update_bits(data->regmap, ADS1015_CFG_REG,
							  ADS1015_CFG_MOD_MASK,
							  mode << ADS1015_CFG_MOD_SHIFT);
}

static int ads1015_set_conv_ready_pin(struct ads1015_data *data)
{
	int ret;

	ret = regmap_write(data->regmap, ADS1015_LO_THRESH_REG, 0);
	if (ret)
		return ret;

	ret = regmap_write(data->regmap, ADS1015_HI_THRESH_REG, 0xFFFF);
	if (ret)
		return ret;

	return regmap_update_bits(data->regmap, ADS1015_CFG_REG, ADS1015_CFG_COMP_QUE_MASK, 0);
}

static irqreturn_t __attribute__((optimize("O0"))) ads1015_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ads1015_data *data = iio_priv(indio_dev);

	data->timestamp = iio_get_time_ns(indio_dev);
	return IRQ_WAKE_THREAD;
}

static irqreturn_t __attribute__((optimize("O0"))) ads1015_irq_handler_thread(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ads1015_data *data = iio_priv(indio_dev);

	struct device *dev = regmap_get_device(data->regmap);
	s16 buf[8]; /* 1x s16 ADC val + 3x s16 padding +  4x s16 timestamp */
	int ret, res, chan;

#ifdef ADS1015_SHOW_DELTA
	s64 timestamp;
	u32 tdelta;
#endif

	/* return if buffer not anabled */
	if (!iio_buffer_enabled(indio_dev))
	{
		dev_dbg_ratelimited(dev, "buffer not enabled");
		data->use_buffer = false;
		goto err;
	}

	memset(buf, 0, sizeof(buf));

	mutex_lock(&data->lock);

	if (data->use_buffer)
	{
		/* fast conversion*/
		ret = regmap_read(data->regmap, ADS1015_CONV_REG, &res);
		if (ret < 0)
		{
			dev_dbg_ratelimited(dev, "regmap_read ret=%d", ret);
			mutex_unlock(&data->lock);
			goto err;
		}
	}
	else
	{
		/* conversion with config update */
		chan = find_first_bit(indio_dev->active_scan_mask,
							  indio_dev->masklength);
		dev_dbg(dev, "config conversion chan=%d", chan);
		ret = ads1015_get_adc_result(data, chan, &res);
		if (ret < 0)
		{
			dev_dbg_ratelimited(dev, "ads1015_get_adc_result ret=%d", ret);
			mutex_unlock(&data->lock);
			goto err;
		}
	}

	buf[0] = res;

	mutex_unlock(&data->lock);

#ifdef ADS1015_SHOW_DELTA
	timestamp = iio_get_time_ns(indio_dev);
#endif

	ret = iio_push_to_buffers_with_timestamp(indio_dev, buf, data->timestamp);

#ifdef ADS1015_SHOW_DELTA
	tdelta = (u32)(timestamp - data->timestamp) / 1000;
	dev_dbg_ratelimited(dev, "iio_push_to_buffers ret=%d delta=%d", ret, tdelta);
#endif

	data->use_buffer = true;

err:

	return IRQ_HANDLED;
}

static int __attribute__((optimize("O0"))) ads1015_probe_irq(struct iio_dev *indio_dev)
{
	struct ads1015_data *data = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(data->regmap);
	unsigned long irq_type;
	int ret;

	irq_type = irqd_get_trigger_type(irq_get_irq_data(data->irq));

	switch (irq_type)
	{
	case IRQF_TRIGGER_RISING:
	case IRQF_TRIGGER_FALLING:
		break;
	default:
		dev_info(dev, "mode %lx unsupported\n", irq_type);
		return -EINVAL;
	}

	ret = devm_request_threaded_irq(dev, data->irq, &ads1015_irq_handler,
									&ads1015_irq_handler_thread,
									irq_type | IRQF_ONESHOT,
									indio_dev->name,
									indio_dev);
	if (ret)
	{
		dev_err(dev, "failed to request trigger irq %d\n", data->irq);
	}

	return ret;
}

static int __attribute__((optimize("O0"))) ads1015_probe(struct i2c_client *client,
														 const struct i2c_device_id *id)
{
	struct iio_dev *indio_dev;
	struct iio_buffer *buffer;
	struct ads1015_data *data;
	enum chip_ids chip;
	int ret;

	// allocate private data
	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	i2c_set_clientdata(client, indio_dev);

	mutex_init(&data->lock);

	indio_dev->dev.parent = &client->dev;
	indio_dev->dev.of_node = client->dev.of_node;
	indio_dev->name = ADS1015_DRV_NAME;
	indio_dev->modes = (INDIO_BUFFER_SOFTWARE | INDIO_DIRECT_MODE);

	/*
	 * Tell the core what device type specific functions should
	 * be run on either side of buffer capture enable / disable.
	 */
	indio_dev->setup_ops = &ads1015_buffer_setup_ops;

	if (client->dev.of_node)
		chip = (enum chip_ids)of_device_get_match_data(&client->dev);
	else
		chip = id->driver_data;

	switch (chip)
	{
	case ADS1015:
		indio_dev->channels = ads1015_channels;
		indio_dev->num_channels = ARRAY_SIZE(ads1015_channels);
		indio_dev->info = &ads1015_info;
		data->data_rate = (unsigned int *)&ads1015_data_rate;
		break;
	case ADS1115:
		indio_dev->channels = ads1115_channels;
		indio_dev->num_channels = ARRAY_SIZE(ads1115_channels);
		indio_dev->info = &ads1115_info;
		data->data_rate = (unsigned int *)&ads1115_data_rate;
		break;
	}

	/* we need to keep this ABI the same as used by hwmon ADS1015 driver */
	ads1015_get_channels_config(client);

	data->regmap = devm_regmap_init_i2c(client, &ads1015_regmap_config);
	if (IS_ERR(data->regmap))
	{
		dev_err(&client->dev, "Failed to allocate register map\n");
		return PTR_ERR(data->regmap);
	}

	data->use_buffer = false;

	/* Allocate a buffer to use - here a kfifo */
	buffer = devm_iio_kfifo_allocate(&client->dev);
	if (!buffer)
	{
		dev_err(&client->dev, "iio kfifo buffer setup failed\n");
		return -ENOMEM;
	}

	iio_device_attach_buffer(indio_dev, buffer);

	/* set conversion ready pin */
	ret = ads1015_set_conv_ready_pin(data);
	if (ret)
		return ret;

	/* continuous conversion mode */
	ret = ads1015_set_conv_mode(data, ADS1015_CONTINUOUS);
	if (ret)
		return ret;

	if (client->irq > 0)
	{
		dev_dbg(&client->dev, "irq= %d", client->irq);

		data->irq = client->irq;

		/*
		 * The device generates interrupts as long as it is powered up.
		 * Some platforms might not allow the option to power it down so
		 * disable the interrupt to avoid extra load on the system
		 */
		// disable_irq(client->irq);

		ret = ads1015_probe_irq(indio_dev);
		if (ret)
			return ret;
	}

	data->conv_invalid = true;

	ret = pm_runtime_set_active(&client->dev);
	if (ret)
		return ret;

	pm_runtime_set_autosuspend_delay(&client->dev, ADS1015_SLEEP_DELAY_MS);
	pm_runtime_use_autosuspend(&client->dev);
	pm_runtime_enable(&client->dev);

	ret = iio_device_register(indio_dev);
	if (ret < 0)
	{
		dev_err(&client->dev, "Failed to register IIO device\n");
		return ret;
	}

	return 0;
}

static int ads1015_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct ads1015_data *data = iio_priv(indio_dev);

	iio_device_unregister(indio_dev);

	pm_runtime_disable(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	pm_runtime_put_noidle(&client->dev);

	/* power down single shot mode */
	return ads1015_set_conv_mode(data, ADS1015_SINGLESHOT);
}

#ifdef CONFIG_PM
static int ads1015_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct ads1015_data *data = iio_priv(indio_dev);

	return ads1015_set_conv_mode(data, ADS1015_SINGLESHOT);
}

static int ads1015_runtime_resume(struct device *dev)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(to_i2c_client(dev));
	struct ads1015_data *data = iio_priv(indio_dev);
	int ret;

	ret = ads1015_set_conv_mode(data, ADS1015_CONTINUOUS);
	if (!ret)
		data->conv_invalid = true;

	return ret;
}
#endif

static const struct dev_pm_ops ads1015_pm_ops = {
	SET_RUNTIME_PM_OPS(ads1015_runtime_suspend,
					   ads1015_runtime_resume, NULL)};

static const struct i2c_device_id ads1015_id[] = {
	{"ads1015", ADS1015},
	{"ads1115", ADS1115},
	{}};
MODULE_DEVICE_TABLE(i2c, ads1015_id);

static const struct of_device_id ads1015_of_match[] = {
	{.compatible = "ti,ads1015",
	 .data = (void *)ADS1015},
	{.compatible = "ti,ads1115",
	 .data = (void *)ADS1115},
	{}};
MODULE_DEVICE_TABLE(of, ads1015_of_match);

static struct i2c_driver ads1015_driver = {
	.driver = {
		.name = ADS1015_DRV_NAME,
		.of_match_table = ads1015_of_match,
		.pm = &ads1015_pm_ops,
	},
	.probe = ads1015_probe,
	.remove = ads1015_remove,
	.id_table = ads1015_id,
};

module_i2c_driver(ads1015_driver);

MODULE_AUTHOR("Daniel Baluta <daniel.baluta@intel.com>");
MODULE_DESCRIPTION("Texas Instruments ADS1015 ADC driver");
MODULE_LICENSE("GPL v2");

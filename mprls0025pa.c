// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MPRLS0025PA - Honeywell MicroPressure pressure sensor series driver
 *
 * Copyright (c) Andreas Klinger <ak@it-klinger.de>
 *
 * Data sheet:
 *  https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/
 *    products/sensors/pressure-sensors/board-mount-pressure-sensors/
 *    micropressure-mpr-series/documents/
 *    sps-siot-mpr-series-datasheet-32332628-ciid-172626.pdf
 *
 * 7-bit I2C default slave address: 0x18
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/units.h>

#include <linux/gpio/consumer.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include <linux/regulator/consumer.h>

#include <asm/unaligned.h>

#include "mprls0025pa.h"

/*
 * support _RAW sysfs interface:
 *
 * Calculation formula from the datasheet:
 * pressure = (press_cnt - outputmin) * scale + pmin
 * with:
 * * pressure	- measured pressure in Pascal
 * * press_cnt	- raw value read from sensor
 * * pmin	- minimum pressure range value of sensor (data->pmin)
 * * pmax	- maximum pressure range value of sensor (data->pmax)
 * * outputmin	- minimum numerical range raw value delivered by sensor
 *						(mpr_func_spec.output_min)
 * * outputmax	- maximum numerical range raw value delivered by sensor
 *						(mpr_func_spec.output_max)
 * * scale	- (pmax - pmin) / (outputmax - outputmin)
 *
 * formula of the userspace:
 * pressure = (raw + offset) * scale
 *
 * Values given to the userspace in sysfs interface:
 * * raw	- press_cnt
 * * offset	- (-1 * outputmin) - pmin / scale
 *                note: With all sensors from the datasheet pmin = 0
 *                which reduces the offset to (-1 * outputmin)
 */

struct mpr_func_spec {
	u32			output_min;
	u32			output_max;
};

/*
 * transfer function A: 10%   to 90%   of 2^24
 * transfer function B:  2.5% to 22.5% of 2^24
 * transfer function C: 20%   to 80%   of 2^24
 */
static const struct mpr_func_spec mpr_func_spec[] = {
	[MPR_FUNCTION_A] = {.output_min = 1677722, .output_max = 15099494},
	[MPR_FUNCTION_B] = {.output_min =  419430, .output_max =  3774874},
	[MPR_FUNCTION_C] = {.output_min = 3355443, .output_max = 13421773},
};

enum mpr_variants {
	MPR0001BA = 0x00, MPR01_6BA = 0x01, MPR02_5BA = 0x02, MPR0060MG = 0x03,
	MPR0100MG = 0x04, MPR0160MG = 0x05, MPR0250MG = 0x06, MPR0400MG = 0x07,
	MPR0600MG = 0x08, MPR0001BG = 0x09, MPR01_6BG = 0x0a, MPR02_5BG = 0x0b,
	MPR0100KA = 0x0c, MPR0160KA = 0x0d, MPR0250KA = 0x0e, MPR0006KG = 0x0f,
	MPR0010KG = 0x10, MPR0016KG = 0x11, MPR0025KG = 0x12, MPR0040KG = 0x13,
	MPR0060KG = 0x14, MPR0100KG = 0x15, MPR0160KG = 0x16, MPR0250KG = 0x17,
	MPR0015PA = 0x18, MPR0025PA = 0x19, MPR0030PA = 0x1a, MPR0001PG = 0x1b,
	MPR0005PG = 0x1c, MPR0015PG = 0x1d, MPR0030PG = 0x1e, MPR0300YG = 0x1f,
	MPR_VARIANTS_MAX
};

static const char * const mpr_triplet_variants[MPR_VARIANTS_MAX] = {
	[MPR0001BA] = "0001BA", [MPR01_6BA] = "01.6BA", [MPR02_5BA] = "02.5BA",
	[MPR0060MG] = "0060MG", [MPR0100MG] = "0100MG", [MPR0160MG] = "0160MG",
	[MPR0250MG] = "0250MG", [MPR0400MG] = "0400MG", [MPR0600MG] = "0600MG",
	[MPR0001BG] = "0001BG", [MPR01_6BG] = "01.6BG", [MPR02_5BG] = "02.5BG",
	[MPR0100KA] = "0100KA", [MPR0160KA] = "0160KA", [MPR0250KA] = "0250KA",
	[MPR0006KG] = "0006KG", [MPR0010KG] = "0010KG", [MPR0016KG] = "0016KG",
	[MPR0025KG] = "0025KG", [MPR0040KG] = "0040KG", [MPR0060KG] = "0060KG",
	[MPR0100KG] = "0100KG", [MPR0160KG] = "0160KG", [MPR0250KG] = "0250KG",
	[MPR0015PA] = "0015PA", [MPR0025PA] = "0025PA", [MPR0030PA] = "0030PA",
	[MPR0001PG] = "0001PG", [MPR0005PG] = "0005PG", [MPR0015PG] = "0015PG",
	[MPR0030PG] = "0030PG", [MPR0300YG] = "0300YG"
};

/**
 * struct mpr_range_config - list of pressure ranges based on nomenclature
 * @pmin: lowest pressure that can be measured
 * @pmax: highest pressure that can be measured
 */
struct mpr_range_config {
	const s32 pmin;
	const s32 pmax;
};

/* All min max limits have been converted to pascals */
static const struct mpr_range_config mpr_range_config[MPR_VARIANTS_MAX] = {
	[MPR0001BA] = { .pmin = 0, .pmax = 100000 },
	[MPR01_6BA] = { .pmin = 0, .pmax = 160000 },
	[MPR02_5BA] = { .pmin = 0, .pmax = 250000 },
	[MPR0060MG] = { .pmin = 0, .pmax =   6000 },
	[MPR0100MG] = { .pmin = 0, .pmax =  10000 },
	[MPR0160MG] = { .pmin = 0, .pmax =  16000 },
	[MPR0250MG] = { .pmin = 0, .pmax =  25000 },
	[MPR0400MG] = { .pmin = 0, .pmax =  40000 },
	[MPR0600MG] = { .pmin = 0, .pmax =  60000 },
	[MPR0001BG] = { .pmin = 0, .pmax = 100000 },
	[MPR01_6BG] = { .pmin = 0, .pmax = 160000 },
	[MPR02_5BG] = { .pmin = 0, .pmax = 250000 },
	[MPR0100KA] = { .pmin = 0, .pmax = 100000 },
	[MPR0160KA] = { .pmin = 0, .pmax = 160000 },
	[MPR0250KA] = { .pmin = 0, .pmax = 250000 },
	[MPR0006KG] = { .pmin = 0, .pmax =   6000 },
	[MPR0010KG] = { .pmin = 0, .pmax =  10000 },
	[MPR0016KG] = { .pmin = 0, .pmax =  16000 },
	[MPR0025KG] = { .pmin = 0, .pmax =  25000 },
	[MPR0040KG] = { .pmin = 0, .pmax =  40000 },
	[MPR0060KG] = { .pmin = 0, .pmax =  60000 },
	[MPR0100KG] = { .pmin = 0, .pmax = 100000 },
	[MPR0160KG] = { .pmin = 0, .pmax = 160000 },
	[MPR0250KG] = { .pmin = 0, .pmax = 250000 },
	[MPR0015PA] = { .pmin = 0, .pmax = 103421 },
	[MPR0025PA] = { .pmin = 0, .pmax = 172369 },
	[MPR0030PA] = { .pmin = 0, .pmax = 206843 },
	[MPR0001PG] = { .pmin = 0, .pmax =   6895 },
	[MPR0005PG] = { .pmin = 0, .pmax =  34474 },
	[MPR0015PG] = { .pmin = 0, .pmax = 103421 },
	[MPR0030PG] = { .pmin = 0, .pmax = 206843 },
	[MPR0300YG] = { .pmin = 0, .pmax =  39997 }
};

static const struct iio_chan_spec mpr_channels[] = {
	{
		.type = IIO_PRESSURE,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
					BIT(IIO_CHAN_INFO_SCALE) |
					BIT(IIO_CHAN_INFO_OFFSET),
		.scan_index = 0,
		.scan_type = {
			.sign = 's',
			.realbits = 32,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	},
	IIO_CHAN_SOFT_TIMESTAMP(1),
};

static void mpr_reset(struct mpr_data *data)
{
	if (data->gpiod_reset) {
		gpiod_set_value(data->gpiod_reset, 0);
		udelay(10);
		gpiod_set_value(data->gpiod_reset, 1);
	}
}

/**
 * mpr_read_pressure() - Read pressure value from sensor via I2C
 * @data: Pointer to private data struct.
 *
 * If there is an end of conversion (EOC) interrupt registered the function
 * waits for a maximum of one second for the interrupt.
 *
 * Context: The function can sleep and data->lock should be held when calling it
 * Return:
 * * 0		- OK, the pressure value could be read
 * * -ETIMEDOUT	- Timeout while waiting for the EOC interrupt or busy flag is
 *		  still set after nloops attempts of reading
 */
static int mpr_read_pressure(struct mpr_data *data)
{
	int ret;

	ret = data->xfer_cb(data);
	if (ret < 0)
		return ret;

	//*press = get_unaligned_be24(&buf[1]);

	//data->is_valid = chip->valid(data);
	//if (!data->is_valid)
	//	return -EAGAIN;

	//dev_dbg(dev, "received: %*ph cnt: %d\n", ret, buf, *press);

	return 0;
}

static irqreturn_t mpr_eoc_handler(int irq, void *p)
{
	struct mpr_data *data = p;

	complete(&data->completion);

	return IRQ_HANDLED;
}

static irqreturn_t mpr_trigger_handler(int irq, void *p)
{
	int ret;
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct mpr_data *data = iio_priv(indio_dev);

	mutex_lock(&data->lock);
	ret = mpr_read_pressure(data);
	if (ret < 0)
		goto err;

	iio_push_to_buffers_with_timestamp(indio_dev, &data->chan,
						iio_get_time_ns(indio_dev));

err:
	mutex_unlock(&data->lock);
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int mpr_read_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int *val, int *val2, long mask)
{
	int ret;
	s32 pressure;
	struct mpr_data *data = iio_priv(indio_dev);

	if (chan->type != IIO_PRESSURE)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		//ret = mpr_read_pressure(data, &pressure);
		ret = mpr_read_pressure(data);
		mutex_unlock(&data->lock);
		if (ret < 0)
			return ret;
		*val = pressure;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = data->scale;
		*val2 = data->scale2;
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		*val = data->offset;
		*val2 = data->offset2;
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info mpr_info = {
	.read_raw = &mpr_read_raw,
};

int mpr_common_probe(struct device *dev, mpr_xfer_fn xfer)
{
	int ret;
	struct mpr_data *data;
	struct iio_dev *indio_dev;
	//struct device *dev = &client->dev;
	s64 scale, offset;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	data->dev = dev;
	data->xfer_cb = xfer;
	// data->irq = client->irq;  !!FIXME

	mutex_init(&data->lock);
	init_completion(&data->completion);

	indio_dev->name = "mprls0025pa";
	indio_dev->info = &mpr_info;
	indio_dev->channels = mpr_channels;
	indio_dev->num_channels = ARRAY_SIZE(mpr_channels);
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				"can't get and enable vdd supply\n");

	if (dev_fwnode(dev)) {
		ret = device_property_read_u32(dev, "honeywell,pmin-pascal",
								&data->pmin);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,pmin-pascal could not be read\n");
		ret = device_property_read_u32(dev, "honeywell,pmax-pascal",
								&data->pmax);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,pmax-pascal could not be read\n");
		ret = device_property_read_u32(dev,
				"honeywell,transfer-function", &data->function);
		if (ret)
			return dev_err_probe(dev, ret,
				"honeywell,transfer-function could not be read\n");
		if (data->function > MPR_FUNCTION_C)
			return dev_err_probe(dev, -EINVAL,
				"honeywell,transfer-function %d invalid\n",
								data->function);
	} else {
		/* when loaded as i2c device we need to use default values */
		dev_notice(dev, "firmware node not found; using defaults\n");
		data->pmin = 0;
		data->pmax = 172369; /* 25 psi */
		data->function = MPR_FUNCTION_A;
	}

	data->outmin = mpr_func_spec[data->function].output_min;
	data->outmax = mpr_func_spec[data->function].output_max;

	/* use 64 bit calculation for preserving a reasonable precision */
	scale = div_s64(((s64)(data->pmax - data->pmin)) * NANO,
						data->outmax - data->outmin);
	data->scale = div_s64_rem(scale, NANO, &data->scale2);
	/*
	 * multiply with NANO before dividing by scale and later divide by NANO
	 * again.
	 */
	offset = ((-1LL) * (s64)data->outmin) * NANO -
			div_s64(div_s64((s64)data->pmin * NANO, scale), NANO);
	data->offset = div_s64_rem(offset, NANO, &data->offset2);

#if 0
	if (data->irq > 0) {
		ret = devm_request_irq(dev, data->irq, mpr_eoc_handler,
				IRQF_TRIGGER_RISING, client->name, data);
		if (ret)
			return dev_err_probe(dev, ret,
				"request irq %d failed\n", data->irq);
	}
#endif

	data->gpiod_reset = devm_gpiod_get_optional(dev, "reset",
							GPIOD_OUT_HIGH);
	if (IS_ERR(data->gpiod_reset))
		return dev_err_probe(dev, PTR_ERR(data->gpiod_reset),
						"request reset-gpio failed\n");

	mpr_reset(data);

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
						mpr_trigger_handler, NULL);
	if (ret)
		return dev_err_probe(dev, ret,
					"iio triggered buffer setup failed\n");

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
					"unable to register iio device\n");

	return 0;
}
EXPORT_SYMBOL_NS(mpr_common_probe, IIO_HONEYWELL_MPRLS0025PA);

MODULE_AUTHOR("Andreas Klinger <ak@it-klinger.de>");
MODULE_DESCRIPTION("Honeywell MPR pressure sensor core driver");
MODULE_LICENSE("GPL");

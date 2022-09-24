/*
 * Copyright (c) 2022, Jimmy Ou
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/counter.h>
#include <zephyr/kernel.h>

#include <hardware/timer.h>

#define DT_DRV_COMPAT	  raspberrypi_pico_counter
#define TIMER_MAX_CHANNEL 4

struct counter_rpi_config {
	timer_hw_t *const timer_dev;
	struct counter_config_info info;
};

struct counter_rpi_channel_data {
	counter_alarm_callback_t callback;
	void *user_data;
};

struct counter_rpi_data {
	struct counter_rpi_channel_data channels[TIMER_MAX_CHANNEL];
};

static int counter_rpi_start(const struct device *dev)
{
	const struct counter_rpi_config *config = dev->config;
	timer_hw_t *const timer_dev = config->timer_dev;

	timer_dev->pause = TIMER_PAUSE_RESET;
	return 0;
}

static int counter_rpi_stop(const struct device *dev)
{
	const struct counter_rpi_config *config = dev->config;
	timer_hw_t *const timer_dev = config->timer_dev;

	timer_dev->pause = TIMER_PAUSE_BITS;
	return 0;
}

static int counter_rpi_get_value(const struct device *dev, uint32_t *ticks)
{
	const struct counter_rpi_config *config = dev->config;
	timer_hw_t *const timer_dev = config->timer_dev;

	*ticks = timer_dev->timerawl;
	return 0;
}

static int counter_rpi_set_alarm(const struct device *dev, uint8_t chan,
				 const struct counter_alarm_cfg *alarm_cfg)
{
	const struct counter_rpi_config *config = dev->config;
	struct counter_rpi_data *data = dev->data;
	timer_hw_t *const timer_dev = config->timer_dev;
	uint32_t ticks;

	if (chan >= config->info.channels) {
		return -EINVAL;
	}
	if (data->channels[chan].callback) {
		return -EBUSY;
	}

	ticks = alarm_cfg->ticks;
	if ((alarm_cfg->flags & COUNTER_ALARM_CFG_ABSOLUTE) != 0) {
	} else {
		ticks += timer_dev->timerawl;
	}

	WRITE_BIT(timer_dev->inte, chan, 0);

	data->channels[chan].callback = alarm_cfg->callback;
	data->channels[chan].user_data = alarm_cfg->user_data;
	timer_dev->alarm[chan] = ticks;

	WRITE_BIT(timer_dev->intr, chan, 1);
	WRITE_BIT(timer_dev->inte, chan, 1);

	return 0;
}

static int counter_rpi_cancel_alarm(const struct device *dev, uint8_t chan)
{
	const struct counter_rpi_config *config = dev->config;
	struct counter_rpi_data *data = dev->data;
	timer_hw_t *const timer_dev = config->timer_dev;

	WRITE_BIT(timer_dev->armed, chan, 1);
	data->channels[chan].callback = NULL;

	return 0;
}

static int counter_rpi_set_top_value(const struct device *dev, const struct counter_top_cfg *cfg)
{
	return -ENOTSUP;
}

static uint32_t counter_rpi_get_pending_int(const struct device *dev)
{
	const struct counter_rpi_config *config = dev->config;
	timer_hw_t *const timer_dev = config->timer_dev;

	return ((timer_dev->ints & 0b111) != 0) ? 1 : 0;
}

static uint32_t counter_rpi_get_top_value(const struct device *dev)
{
	const struct counter_config_info *info = dev->config;

	return info->max_top_value;
}

static uint32_t counter_rpi_get_freq(const struct device *dev)
{
	const struct counter_rpi_config *config = dev->config;

	return config->info.freq;
}

static const struct counter_driver_api counter_rpi_driver_api = {
	.start = counter_rpi_start,
	.stop = counter_rpi_stop,
	.get_value = counter_rpi_get_value,
	.set_alarm = counter_rpi_set_alarm,
	.cancel_alarm = counter_rpi_cancel_alarm,
	.set_top_value = counter_rpi_set_top_value,
	.get_pending_int = counter_rpi_get_pending_int,
	.get_top_value = counter_rpi_get_top_value,
	.get_freq = counter_rpi_get_freq,
};

static inline void counter_rpi_irq_handler(const struct device *dev, int chan)
{
	const struct counter_rpi_config *config = dev->config;
	struct counter_rpi_data *data = dev->data;
	struct counter_rpi_channel_data *channel = &data->channels[chan];

	timer_hw_t *const timer_dev = config->timer_dev;
	const int ticks = timer_dev->timerawl;

	WRITE_BIT(timer_dev->inte, chan, 0);

	counter_alarm_callback_t cb = channel->callback;
	void *user_data = channel->user_data;

	channel->callback = NULL;
	channel->user_data = NULL;

	if (cb != NULL) {
		cb(dev, chan, ticks, user_data);
	}
}

#define TIMER(idx) DT_NODELABEL(timer##idx)
#define FREQ	   1000000U

#define TIMER_CHANNEL_IRQ_CONNECT(idx, ch)                                                         \
	COND_CODE_1(CONFIG_COUNTER_TIMER##idx##_ZLI,                                               \
		    (IRQ_DIRECT_CONNECT(DT_IRQ_BY_IDX(TIMER(idx), ch, irq),                        \
					DT_IRQ_BY_IDX(TIMER(idx), ch, priority),                   \
					counter_rpi_timer##idx##_ch##ch##_irq_handler,             \
					IRQ_ZERO_LATENCY)),                                        \
		    (IRQ_CONNECT(DT_IRQ_BY_IDX(TIMER(idx), ch, irq),                               \
				 DT_IRQ_BY_IDX(TIMER(idx), ch, priority),                          \
				 counter_rpi_timer##idx##_ch##ch##_irq_handler,                    \
				 DEVICE_DT_GET(TIMER(idx)), 0)))

/* clang-format off */
#define TIMER_CHANNEL_IRQ_DECLARE(idx, ch)                                                         \
	COND_CODE_1(CONFIG_COUNTER_TIMER##ch##_ZLI,                                                \
		(ISR_DIRECT_DECLARE(counter_rpi_timer##idx##_ch##ch##_irq_handler)                 \
		{                                                                                  \
			counter_rpi_irq_handler(dev, ch);                                          \
			return 0;                                                                  \
		}),                                                                                \
		(static void counter_rpi_timer##idx##_ch##ch##_irq_handler(void *args)             \
		{                                                                                  \
			const struct device *dev = args;                                           \
			counter_rpi_irq_handler(dev, ch);                                          \
		}))

/* clang-format on */

#define TIMER_IRQ_CONNECT(idx)                                                                     \
	TIMER_CHANNEL_IRQ_CONNECT(idx, 0);                                                         \
	TIMER_CHANNEL_IRQ_CONNECT(idx, 1);                                                         \
	TIMER_CHANNEL_IRQ_CONNECT(idx, 2);                                                         \
	TIMER_CHANNEL_IRQ_CONNECT(idx, 3)

#define TIMER_IRQ_ENABLE(idx)                                                                      \
	irq_enable(DT_IRQ_BY_IDX(TIMER(idx), 0, irq));                                             \
	irq_enable(DT_IRQ_BY_IDX(TIMER(idx), 1, irq));                                             \
	irq_enable(DT_IRQ_BY_IDX(TIMER(idx), 2, irq));                                             \
	irq_enable(DT_IRQ_BY_IDX(TIMER(idx), 3, irq));

#define TIMER_IRQ_DECLARE(idx)                                                                     \
	TIMER_CHANNEL_IRQ_DECLARE(idx, 0);                                                         \
	TIMER_CHANNEL_IRQ_DECLARE(idx, 1);                                                         \
	TIMER_CHANNEL_IRQ_DECLARE(idx, 2);                                                         \
	TIMER_CHANNEL_IRQ_DECLARE(idx, 3)

#define COUNTER_DEVICE_INIT(idx)                                                                   \
	TIMER_IRQ_DECLARE(idx);                                                                    \
                                                                                                   \
	static int counter_rpi_timer##idx##_init(const struct device *dev)                         \
	{                                                                                          \
		struct counter_rpi_data *data = dev->data;                                         \
		for (size_t i = 0; i < ARRAY_SIZE(data->channels); i++) {                          \
			data->channels[i].callback = NULL;                                         \
			data->channels[i].user_data = NULL;                                        \
		}                                                                                  \
                                                                                                   \
		TIMER_IRQ_CONNECT(idx);                                                            \
		TIMER_IRQ_ENABLE(idx);                                                             \
                                                                                                   \
		return 0;                                                                          \
	}                                                                                          \
                                                                                                   \
	static struct counter_rpi_data counter_rpi_##idx##_data;                                   \
	static const struct counter_rpi_config rpi_counter_##idx##_config = {                      \
		.timer_dev = timer_hw,                                                             \
		.info = {.max_top_value = UINT32_MAX,                                              \
			 .freq = FREQ,                                                             \
			 .flags = COUNTER_CONFIG_INFO_COUNT_UP,                                    \
			 .channels = TIMER_MAX_CHANNEL},                                           \
	};                                                                                         \
	DEVICE_DT_DEFINE(TIMER(idx), counter_rpi_timer##idx##_init, NULL,                          \
			 &counter_rpi_##idx##_data, &rpi_counter_##idx##_config, PRE_KERNEL_1,     \
			 CONFIG_COUNTER_INIT_PRIORITY, &counter_rpi_driver_api)

DT_INST_FOREACH_STATUS_OKAY(COUNTER_DEVICE_INIT);

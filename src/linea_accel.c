/*
 * LiNEA40 用のトラボの加速（ZMK v0.2.1 の入力プロセッサ）
 * 動かす速さに応じて X/Y の移動量に倍率をかける。端数は軸ごとに持ち越すので、
 * 倍率が 1 未満でもゆっくり動かした分が消えない。
 */
#define DT_DRV_COMPAT linea_input_processor_accel

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

struct accel_config {
    int32_t min_milli;
    int32_t max_milli;
    int32_t speed_low;
    int32_t speed_high;
};

struct accel_data {
    int32_t rem_x, rem_y;   /* 持ち越した端数（1/1000 単位） */
    int32_t last_x, last_y; /* 直前の各軸の移動量（速さの見積もり用） */
    int64_t last_time;
};

static int32_t factor_for(const struct accel_config *cfg, int32_t speed) {
    if (speed <= cfg->speed_low) {
        return cfg->min_milli;
    }
    if (speed >= cfg->speed_high || cfg->speed_high <= cfg->speed_low) {
        return cfg->max_milli;
    }
    return cfg->min_milli +
           (cfg->max_milli - cfg->min_milli) * (speed - cfg->speed_low) / (cfg->speed_high - cfg->speed_low);
}

static int accel_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                              uint32_t param2, struct zmk_input_processor_state *state) {
    const struct accel_config *cfg = dev->config;
    struct accel_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* しばらく止まっていたら、端数と速さの記録を捨てる */
    int64_t now = k_uptime_get();
    if (now - data->last_time > 100) {
        data->rem_x = data->rem_y = 0;
        data->last_x = data->last_y = 0;
    }
    data->last_time = now;

    int32_t v = event->value;
    if (event->code == INPUT_REL_X) {
        data->last_x = abs(v);
    } else {
        data->last_y = abs(v);
    }
    /* 斜めの速さのおおよそ（大きい方 + 小さい方の半分） */
    int32_t hi = MAX(data->last_x, data->last_y), lo = MIN(data->last_x, data->last_y);
    int32_t factor = factor_for(cfg, hi + lo / 2);

    int32_t *rem = event->code == INPUT_REL_X ? &data->rem_x : &data->rem_y;
    int32_t scaled = v * factor + *rem;
    int32_t out = scaled / 1000;
    *rem = scaled - out * 1000;
    event->value = out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api accel_driver_api = {
    .handle_event = accel_handle_event,
};

#define ACCEL_INST(n)                                                                              \
    static const struct accel_config accel_config_##n = {                                          \
        .min_milli = DT_INST_PROP(n, min_factor_milli),                                            \
        .max_milli = DT_INST_PROP(n, max_factor_milli),                                            \
        .speed_low = DT_INST_PROP(n, speed_low),                                                   \
        .speed_high = DT_INST_PROP(n, speed_high),                                                 \
    };                                                                                             \
    static struct accel_data accel_data_##n;                                                       \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &accel_data_##n, &accel_config_##n, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &accel_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST)

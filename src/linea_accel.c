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

#include "linea_settings.h"

struct accel_config {
    int32_t min_milli;
    int32_t max_milli;
    int32_t speed_low;
    int32_t speed_high;
    int32_t min_interval;
    int32_t settings_slot; /* 1 = カーソル（LiNEA40 Studio から変えた速さの倍率を掛ける） */
};

struct accel_data {
    int32_t rem_x, rem_y;   /* 持ち越した端数（1/1000 単位） */
    int32_t last_x, last_y; /* 直前の各軸の移動量（速さの見積もり用） */
    int64_t last_time;
    int32_t pend_x, pend_y;    /* 間隔待ちでためている移動量 */
    int64_t sent_x, sent_y;    /* 最後に送った時刻 */
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
        data->pend_x = data->pend_y = 0;
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
    /* LiNEA40 Studio から変えた速さ（CPI を変えたのと同じ）。ビルドせずにすぐ反映する */
    if (cfg->settings_slot == 1) {
        factor = (int32_t)((int64_t)factor * linea_setting_get(LINEA_SET_CURSOR_SPEED, 1000) / 1000);
    }

    int32_t *rem = event->code == INPUT_REL_X ? &data->rem_x : &data->rem_y;
    int32_t scaled = v * factor + *rem;
    int32_t out = scaled / 1000;
    *rem = scaled - out * 1000;

    /* 間隔を空けて送る: 前に送ってから min_interval 経つまではためておく */
    if (cfg->min_interval > 0) {
        int32_t *pend = event->code == INPUT_REL_X ? &data->pend_x : &data->pend_y;
        int64_t *sent = event->code == INPUT_REL_X ? &data->sent_x : &data->sent_y;
        *pend += out;
        if (now - *sent < cfg->min_interval) {
            out = 0;
        } else {
            out = *pend;
            *pend = 0;
            if (out != 0) {
                *sent = now;
            }
        }
    }
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
        .min_interval = DT_INST_PROP(n, min_interval_ms),                                          \
        .settings_slot = DT_INST_PROP(n, settings_slot),                                           \
    };                                                                                             \
    static struct accel_data accel_data_##n;                                                       \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &accel_data_##n, &accel_config_##n, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &accel_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST)

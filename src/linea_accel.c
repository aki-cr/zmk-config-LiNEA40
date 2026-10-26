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

#include <zmk/keymap.h>

#include "linea_settings.h"

struct accel_config {
    int32_t min_milli;
    int32_t max_milli;
    int32_t speed_low;
    int32_t speed_high;
    int32_t min_interval;
    int32_t settings_slot; /* LiNEA40 Studio から変えた値を使う: 1 = カーソル（速さ・加速・精密モード）, 2 = スクロールの速さ, 3 = スクロールの加速 */
    int32_t precision_layer;   /* このレイヤーがいちばん上のときは 1/precision_divisor にする（-1 = 使わない） */
    int32_t precision_divisor;
    int32_t enable_default;    /* 0 なら、LiNEA40 Studio でオンにするまで何もしない（スクロールの加速） */
};

struct accel_data {
    int32_t rem_x, rem_y;   /* 持ち越した端数（1/1000 単位） */
    int32_t last_x, last_y; /* 直前の各軸の移動量（速さの見積もり用） */
    int64_t last_time;
    int32_t pend_x, pend_y;    /* 間隔待ちでためている移動量 */
    int64_t sent_x, sent_y;    /* 最後に送った時刻 */
};

/* カーソル（settings_slot 1）は、LiNEA40 Studio から変えた加速の値があればそれを使う */
static int32_t factor_for(const struct accel_config *cfg, int32_t speed) {
    int32_t min = cfg->min_milli, max = cfg->max_milli, low = cfg->speed_low, high = cfg->speed_high;
    if (cfg->settings_slot == 1) {
        min = linea_setting_get(LINEA_SET_CURSOR_ACCEL_MIN, min);
        max = linea_setting_get(LINEA_SET_CURSOR_ACCEL_MAX, max);
        low = linea_setting_get(LINEA_SET_CURSOR_SPEED_LOW, low);
        high = linea_setting_get(LINEA_SET_CURSOR_SPEED_HIGH, high);
    } else if (cfg->settings_slot == 3) {
        min = linea_setting_get(LINEA_SET_SCROLL_ACCEL_MIN, min);
        max = linea_setting_get(LINEA_SET_SCROLL_ACCEL_MAX, max);
        low = linea_setting_get(LINEA_SET_SCROLL_SPEED_LOW, low);
        high = linea_setting_get(LINEA_SET_SCROLL_SPEED_HIGH, high);
    }
    if (speed <= low) {
        return min;
    }
    if (speed >= high || high <= low) {
        return max;
    }
    return min + (max - min) * (speed - low) / (high - low);
}

static int accel_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                              uint32_t param2, struct zmk_input_processor_state *state) {
    const struct accel_config *cfg = dev->config;
    struct accel_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    /* スクロールの加速はオン/オフを LiNEA40 Studio から切り替える。オフなら何もしない */
    if (cfg->settings_slot == 3 && !linea_setting_get(LINEA_SET_SCROLL_ACCEL_ON, cfg->enable_default)) {
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
        /* DeX モード（プロファイル 3）では Android 用の倍率も掛ける（Android は Mac より同じ量で大きく動くため） */
        if (linea_dex_active()) {
            factor = (int32_t)((int64_t)factor * linea_setting_get(LINEA_SET_DEX_CURSOR, LINEA_DEX_CURSOR_DEFAULT) / 1000);
        }
        /* 精密モード: ドライバーの snipe と同じく、いちばん上のレイヤーが Precision のとき */
        /* レイヤーはキーマップを持つ右手側（セントラル）にしか無い。左手側では使わない */
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        if (cfg->precision_layer >= 0 && zmk_keymap_highest_layer_active() == cfg->precision_layer) {
            int32_t div = linea_setting_get(LINEA_SET_PRECISION_DIVISOR, cfg->precision_divisor);
            factor /= MAX(div, 1);
        }
#endif
    } else if (cfg->settings_slot == 2) {
        factor = (int32_t)((int64_t)factor * linea_setting_get(LINEA_SET_SCROLL_SPEED, 1000) / 1000);
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
        .precision_layer = DT_INST_PROP(n, precision_layer),                                       \
        .precision_divisor = DT_INST_PROP(n, precision_divisor),                                   \
        .enable_default = DT_INST_PROP(n, enable_default),                                         \
    };                                                                                             \
    static struct accel_data accel_data_##n;                                                       \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &accel_data_##n, &accel_config_##n, POST_KERNEL,          \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &accel_driver_api);

DT_INST_FOREACH_STATUS_OKAY(ACCEL_INST)

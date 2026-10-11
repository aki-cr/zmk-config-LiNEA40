/*
 * LiNEA40 用のスクロールの刻み（ZMK v0.2.1 の入力プロセッサ）
 * トラボのスクロールを、エンコーダーと同じ「1回の報告で ±1 段」の信号にそろえる。
 * 1回で2段以上になる分は持ち越して次の報告で送る（持ち越しは max-carry 段まで。止まったら捨てる）。
 * Mac Mouse Fix は1つの信号を1段として扱うので、エンコーダーと同じなめらかな加速・減速になる。
 *
 * また、縦と横を同時に送らない（そのとき多く動いている方だけ送る）。
 * Mac Mouse Fix は縦横が同時に入ったスクロールをなめらかにせずそのまま通すため、
 * ボールが少し斜めに回るだけで Mac 標準の加速スクロールになり、「ビュンと速くなって急に止まる」動きになっていた。
 */
#define DT_DRV_COMPAT linea_input_processor_tick

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

#include "linea_settings.h"

struct tick_config {
    int32_t max_carry;
    int32_t enable_default; /* 0 なら、LiNEA40 Studio でオンにするまで何もしない */
};

struct tick_data {
    int32_t carry_v, carry_h; /* 縦・横の持ち越し */
    int32_t mag_v, mag_h;     /* 最近動いた量（縦・横）。どちらの向きを送るかを決める */
    int32_t lock;             /* 送る向き: 0=まだ決めていない 1=縦 2=横 */
    int64_t last_time;
};

static int tick_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                             uint32_t param2, struct zmk_input_processor_state *state) {
    const struct tick_config *cfg = dev->config;
    struct tick_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_WHEEL && event->code != INPUT_REL_HWHEEL)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    /* LiNEA40 Studio から切り替えられる（オフならまとめて送る） */
    if (!linea_setting_get(LINEA_SET_SCROLL_TICK_ON, cfg->enable_default)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    int32_t max_carry = linea_setting_get(LINEA_SET_SCROLL_MAX_CARRY, cfg->max_carry);

    /* しばらく止まっていたら持ち越しを捨てる（止めたのに後から動かないように） */
    int64_t now = k_uptime_get();
    if (now - data->last_time > 100) {
        data->carry_v = data->carry_h = 0;
        data->mag_v = data->mag_h = 0;
        data->lock = 0;
    }
    data->last_time = now;

    /* 縦か横のどちらを送るか決める: 最初に動いた向き。ただし反対の向きが3倍以上動いたら乗り換える */
    int32_t axis = event->code == INPUT_REL_WHEEL ? 1 : 2;
    int32_t *mag = axis == 1 ? &data->mag_v : &data->mag_h;
    int32_t *other = axis == 1 ? &data->mag_h : &data->mag_v;
    *mag = *mag * 7 / 8 + abs(event->value) * 8; /* 8倍して端数を残しつつ、古い分は少しずつ減らす */
    if (data->lock == 0 && event->value != 0) {
        data->lock = axis;
    } else if (data->lock != axis && data->lock != 0 && *mag > *other * 3 + 16) {
        /* 乗り換え: この報告ではまだ送らない（同じ報告で前の向きを送っているかもしれないので、縦横が重ならないように） */
        data->lock = axis;
        data->carry_v = data->carry_h = 0;
        event->value = 0;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int32_t *carry = axis == 1 ? &data->carry_v : &data->carry_h;
    if (data->lock != axis) {
        /* 送らない向き: 捨てる */
        *carry = 0;
        event->value = 0;
        return ZMK_INPUT_PROC_CONTINUE;
    }
    /* 向きが変わったら持ち越しは捨てる */
    if ((int64_t)event->value * *carry < 0) {
        *carry = 0;
    }
    int32_t total = event->value + *carry;
    int32_t out = CLAMP(total, -1, 1);
    *carry = CLAMP(total - out, -max_carry, max_carry);
    event->value = out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api tick_driver_api = {
    .handle_event = tick_handle_event,
};

#define TICK_INST(n)                                                                               \
    static const struct tick_config tick_config_##n = {                                            \
        .max_carry = DT_INST_PROP(n, max_carry),                                                   \
        .enable_default = DT_INST_PROP(n, enable_default),                                         \
    };                                                                                             \
    static struct tick_data tick_data_##n;                                                         \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &tick_data_##n, &tick_config_##n, POST_KERNEL,            \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tick_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TICK_INST)

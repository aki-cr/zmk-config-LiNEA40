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

/*
 * すき間を埋める（pace）: LiNEA40 のトラボは小さく、連続で転がしても指を戻す 0.2〜0.4秒のすき間ができる。
 * そのすき間でホイールの信号が止まると、Mac Mouse Fix のなめらかなスクロールも止まり「詰まる」。
 * そこで、届いた段をいったん貯め、最近の平均の間隔で1段ずつ送り出す（すき間の間も送り続ける）。
 *   - 最初の1段はすぐ送る（反応は遅れない）。逆向きに転がしたら貯めた段は捨てる
 *   - 貯める段は pace-buffer まで（指を離したあと流れ続けすぎないように）
 *   - 送り出しはこの部品自身を「入力の出どころ」にして input_report_rel で出し、
 *     LiNEA40_right.overlay の linea_scroll_listener（処理なし）がそのまま Mac に送る
 * （2026-10-13 記録で、詰まりは転がし直しのすき間と一致。持ち越しを増やしても、次の報告が来るまで送らないので効かなかった）
 */
#define PACE_WINDOW_MS 400
#define PACE_HIST 32

struct tick_config {
    int32_t max_carry;
    int32_t enable_default; /* 0 なら、LiNEA40 Studio でオンにするまで何もしない */
    int32_t pace_default;   /* すき間を埋めるか（LiNEA40 Studio で変えていないとき） */
    int32_t pace_buffer;    /* 貯める段の上限 */
};

struct tick_data {
    int32_t carry_v, carry_h; /* 縦・横の持ち越し */
    int32_t mag_v, mag_h;     /* 最近動いた量（縦・横）。どちらの向きを送るかを決める */
    int32_t lock;             /* 送る向き: 0=まだ決めていない 1=縦 2=横 */
    int64_t last_time;
    /* すき間を埋める */
    const struct device *dev;
    struct k_work_delayable pace_work;
    struct k_spinlock lock_pace;
    int32_t queue;   /* 貯めている段（符号つき） */
    uint16_t q_code; /* 送る向き（INPUT_REL_WHEEL / HWHEEL） */
    bool running;    /* 送り出し中 */
    int64_t hist[PACE_HIST]; /* 段が届いた時刻（平均の間隔を出す） */
    int hist_i;
};

/* 最近 PACE_WINDOW_MS に届いた段の数から、送り出す間隔を決める */
static int32_t pace_interval(struct tick_data *data, int64_t now, int32_t cap) {
    int n = 0;
    for (int i = 0; i < PACE_HIST; i++) {
        if (data->hist[i] && now - data->hist[i] <= PACE_WINDOW_MS) {
            n++;
        }
    }
    int32_t iv = n >= 2 ? PACE_WINDOW_MS / n : 40;
    if (abs(data->queue) > cap / 2) {
        iv = iv * 2 / 3; /* 貯まりすぎたら少し速く送る（遅れを広げない） */
    }
    return CLAMP(iv, 8, 60);
}

static void pace_work_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct tick_data *data = CONTAINER_OF(dw, struct tick_data, pace_work);
    const struct tick_config *cfg = data->dev->config;

    k_spinlock_key_t key = k_spin_lock(&data->lock_pace);
    if (data->queue == 0) {
        data->running = false;
        k_spin_unlock(&data->lock_pace, key);
        return;
    }
    int32_t s = data->queue > 0 ? 1 : -1;
    data->queue -= s;
    uint16_t code = data->q_code;
    int32_t iv = pace_interval(data, k_uptime_get(), linea_setting_get(LINEA_SET_SCROLL_PACE_BUFFER, cfg->pace_buffer));
    data->running = data->queue != 0;
    k_spin_unlock(&data->lock_pace, key);

    input_report_rel(data->dev, code, s, true, K_NO_WAIT);
    if (data->running) {
        k_work_schedule(&data->pace_work, K_MSEC(iv));
    }
}

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
    /* すき間を埋める: 段を貯めて、送り出しはこの部品が行う（ここでは送らない） */
    if (linea_setting_get(LINEA_SET_SCROLL_PACE, cfg->pace_default)) {
        int32_t v = event->value;
        event->value = 0;
        if (v == 0) {
            return ZMK_INPUT_PROC_CONTINUE;
        }
        k_spinlock_key_t key = k_spin_lock(&data->lock_pace);
        if ((int64_t)data->queue * v < 0 || data->q_code != event->code) {
            data->queue = 0; /* 逆向き・向きの乗り換え: 貯めた段は捨てて、すぐ新しい向きへ */
        }
        data->q_code = event->code;
        int32_t cap = linea_setting_get(LINEA_SET_SCROLL_PACE_BUFFER, cfg->pace_buffer);
        data->queue = CLAMP(data->queue + v, -cap, cap);
        for (int i = 0; i < abs(v) && i < PACE_HIST; i++) {
            data->hist[data->hist_i] = now;
            data->hist_i = (data->hist_i + 1) % PACE_HIST;
        }
        bool start = !data->running;
        data->running = true;
        k_spin_unlock(&data->lock_pace, key);
        if (start) {
            k_work_reschedule(&data->pace_work, K_NO_WAIT); /* 最初の1段はすぐ */
        }
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

static int tick_init(const struct device *dev) {
    struct tick_data *data = dev->data;
    data->dev = dev;
    k_work_init_delayable(&data->pace_work, pace_work_handler);
    return 0;
}

#define TICK_INST(n)                                                                               \
    static const struct tick_config tick_config_##n = {                                            \
        .max_carry = DT_INST_PROP(n, max_carry),                                                   \
        .enable_default = DT_INST_PROP(n, enable_default),                                         \
        .pace_default = DT_INST_PROP(n, pace_default),                                             \
        .pace_buffer = DT_INST_PROP(n, pace_buffer),                                               \
    };                                                                                             \
    static struct tick_data tick_data_##n;                                                         \
    DEVICE_DT_INST_DEFINE(n, tick_init, NULL, &tick_data_##n, &tick_config_##n, POST_KERNEL,            \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tick_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TICK_INST)

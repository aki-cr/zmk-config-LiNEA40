/*
 * LiNEA40 用のスクロールの刻み（ZMK v0.2.1 の入力プロセッサ）
 * トラボのスクロールを、エンコーダーと同じ「1回の報告で ±1 段」の信号にそろえる。
 * 1回で2段以上になる分は持ち越して次の報告で送る（持ち越しは max-carry 段まで。止まったら捨てる）。
 * Mac Mouse Fix は1つの信号を1段として扱うので、エンコーダーと同じなめらかな加速・減速になる。
 *
 * また、縦と横を同時に送らない（そのとき多く動いている方だけ送る）。
 * Mac Mouse Fix は縦横が同時に入ったスクロールをなめらかにせずそのまま通すため、
 * ボールが少し斜めに回るだけで Mac 標準の加速スクロールになり、「ビュンと速くなって急に止まる」動きになっていた。
 *
 * さらに、速く弾いた直後の「惰性でゆっくり回っている分」は送らない。
 * Mac Mouse Fix は信号が 0.16 秒以上途切れるたびに「次のひと転がし」と数え、素早く何回も転がすと一気に加速する（Conductor と同じ）。
 * 持ち替えの間もボールの惰性で信号が出続けると、ずっと1回に見えて加速しないため。
 */
#define DT_DRV_COMPAT linea_input_processor_tick

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

struct tick_config {
    int32_t max_carry;
    int32_t fast_milli;
};

struct tick_data {
    int32_t carry_v, carry_h; /* 縦・横の持ち越し */
    int32_t mag_v, mag_h;     /* 最近動いた量（縦・横）。どちらの向きを送るかを決める */
    int32_t lock;             /* 送る向き: 0=まだ決めていない 1=縦 2=横 */
    int32_t speed, peak;      /* 今の速さ（ならした値）と、最近の一番速い値（どちらも 64 倍） */
    int64_t last_time;
};

static int tick_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                             uint32_t param2, struct zmk_input_processor_state *state) {
    const struct tick_config *cfg = dev->config;
    struct tick_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_WHEEL && event->code != INPUT_REL_HWHEEL)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* しばらく止まっていたら持ち越しを捨てる（止めたのに後から動かないように） */
    int64_t now = k_uptime_get();
    if (now - data->last_time > 100) {
        data->carry_v = data->carry_h = 0;
        data->mag_v = data->mag_h = 0;
        data->lock = 0;
        data->speed = data->peak = 0;
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
    /* 惰性の分を止める: 最近速く弾いていて（peak）、今はその 1/3 より遅いなら送らない */
    data->speed = data->speed * 3 / 4 + abs(event->value) * 16;
    data->peak = MAX(data->peak - data->peak / 32, data->speed);
    if (cfg->fast_milli > 0 && data->peak >= cfg->fast_milli * 64 / 1000 && data->speed * 3 < data->peak) {
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
    *carry = CLAMP(total - out, -cfg->max_carry, cfg->max_carry);
    event->value = out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api tick_driver_api = {
    .handle_event = tick_handle_event,
};

#define TICK_INST(n)                                                                               \
    static const struct tick_config tick_config_##n = {                                            \
        .max_carry = DT_INST_PROP(n, max_carry),                                                   \
        .fast_milli = DT_INST_PROP(n, fast_milli),                                                 \
    };                                                                                             \
    static struct tick_data tick_data_##n;                                                         \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &tick_data_##n, &tick_config_##n, POST_KERNEL,            \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tick_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TICK_INST)

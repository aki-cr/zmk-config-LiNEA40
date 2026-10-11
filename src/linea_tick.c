/*
 * LiNEA40 用のスクロールの刻み（ZMK v0.2.1 の入力プロセッサ）
 * トラボのスクロールを、エンコーダーと同じ「1回の報告で ±1 段」の信号にそろえる。
 * 1回で2段以上になる分は持ち越して次の報告で送る（持ち越しは max-carry 段まで。止まったら捨てる）。
 * Mac Mouse Fix は1つの信号を1段として扱うので、エンコーダーと同じなめらかな加速・減速になる。
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
};

struct tick_data {
    int32_t carry_v, carry_h; /* 縦・横の持ち越し */
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
    }
    data->last_time = now;

    int32_t *carry = event->code == INPUT_REL_WHEEL ? &data->carry_v : &data->carry_h;
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
    };                                                                                             \
    static struct tick_data tick_data_##n;                                                         \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &tick_data_##n, &tick_config_##n, POST_KERNEL,            \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &tick_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TICK_INST)

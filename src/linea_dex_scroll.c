/*
 * DeX モードのスクロール（ZMK v0.2.1 の入力プロセッサ）
 * DeX モード（linea_dex.c。プロファイル 3）のときだけ、ホイールの信号の向きを逆にし、量に倍率をかける。
 * Mac はスクロールの向きを「ナチュラル」にしているが、Android はマウスのホイールをそのまま使うので逆になるため。
 * ほかのプロファイルと USB では何もしない。トラボのスクロール・エンコーダーのどちらの最後にも置く。
 */
#define DT_DRV_COMPAT linea_input_processor_dex_scroll

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include "linea_settings.h"

struct dex_scroll_data {
    int32_t rem_v, rem_h; /* 倍率をかけた端数（縦・横） */
    int64_t last_time;
};

static int dex_scroll_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                                   uint32_t param2, struct zmk_input_processor_state *state) {
    struct dex_scroll_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_WHEEL && event->code != INPUT_REL_HWHEEL)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }
    if (!linea_dex_active()) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* しばらく止まっていたら端数を捨てる */
    int64_t now = k_uptime_get();
    if (now - data->last_time > 300) {
        data->rem_v = data->rem_h = 0;
    }
    data->last_time = now;

    int32_t v = event->value;
    if (linea_setting_get(LINEA_SET_DEX_SCROLL_INVERT, LINEA_DEX_SCROLL_INVERT_DEFAULT)) {
        v = -v;
    }
    int32_t milli = linea_setting_get(LINEA_SET_DEX_SCROLL_SPEED, LINEA_DEX_SCROLL_SPEED_DEFAULT);
    int32_t *rem = event->code == INPUT_REL_WHEEL ? &data->rem_v : &data->rem_h;
    int32_t scaled = v * milli + *rem;
    int32_t out = scaled / 1000;
    *rem = scaled - out * 1000;
    event->value = out;

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api dex_scroll_driver_api = {
    .handle_event = dex_scroll_handle_event,
};

#define DEX_SCROLL_INST(n)                                                                         \
    static struct dex_scroll_data dex_scroll_data_##n = {};                                        \
    DEVICE_DT_INST_DEFINE(n, NULL, NULL, &dex_scroll_data_##n, NULL, POST_KERNEL,                  \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dex_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DEX_SCROLL_INST)

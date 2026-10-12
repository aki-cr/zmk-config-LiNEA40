/*
 * LiNEA40 用の Gesture（Conductor と同じ使い方）
 * Gesture レイヤーにいる間、トラボの動きでカーソルは動かさず、上下左右のどれかに一定量転がしたら、
 * Gesture レイヤーの「上・下・左・右」の位置に置いたキーを1回押す。ボールが止まったら次を受け付ける。
 * 送るキーはキーマップから読むので、Studio（LiNEA40 Studio）で変えられる。
 */
#define DT_DRV_COMPAT linea_input_processor_gesture

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>

#include <zmk/keymap.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <zmk/virtual_key_position.h>

#include "linea_settings.h"

struct gesture_config {
    uint8_t index;
    uint8_t layer;        /* Gesture レイヤーの番号 */
    uint8_t positions[4]; /* 上・下・左・右 に対応するキーの番号 */
    int32_t threshold;    /* これだけ転がしたら1回 */
    int32_t idle_ms;      /* これだけ止まったら次を受け付ける */
};

struct gesture_data {
    int32_t ax, ay;
    int64_t last_time;
    bool fired;
    int64_t fire_time; /* 最後に発動した時刻 */
    /* キーの動作は入力の処理（スタックが小さい）の中では呼ばず、ワークで呼ぶ */
    struct k_work work;
    const struct device *dev;
    int dir;
    uint8_t listener_index;
};

static void fire_work(struct k_work *work) {
    struct gesture_data *data = CONTAINER_OF(work, struct gesture_data, work);
    const struct gesture_config *cfg = data->dev->config;
    int dir = data->dir;
/* キーの動作を実行できるのはキーマップを持つ側（右手 = セントラル）だけ。左手のビルドでは何もしない */
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(cfg->layer);
    const struct zmk_behavior_binding *b = zmk_keymap_get_layer_binding_at_idx(id, cfg->positions[dir]);
    if (b == NULL || b->behavior_dev == NULL) {
        return;
    }
    struct zmk_behavior_binding_event ev = {
        .position = ZMK_VIRTUAL_KEY_POSITION_BEHAVIOR_INPUT_PROCESSOR(data->listener_index, cfg->index),
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    /* 押して、少し待って離す（キューで順番に実行される） */
    zmk_behavior_queue_add(&ev, *b, true, 30);
    zmk_behavior_queue_add(&ev, *b, false, 0);
#endif
}

static int gesture_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                                uint32_t param2, struct zmk_input_processor_state *state) {
    const struct gesture_config *cfg = dev->config;
    struct gesture_data *data = dev->data;

    if (event->type != INPUT_EV_REL || (event->code != INPUT_REL_X && event->code != INPUT_REL_Y)) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* LiNEA40 Studio から変えた値があればそれを使う（ビルドせずにすぐ反映） */
    int32_t idle_ms = linea_setting_get(LINEA_SET_GESTURE_IDLE, cfg->idle_ms);
    int32_t threshold = linea_setting_get(LINEA_SET_GESTURE_THRESHOLD, cfg->threshold);

    int64_t now = k_uptime_get();
    if (now - data->last_time > idle_ms) {
        data->ax = data->ay = 0;
        data->fired = false;
    }
    data->last_time = now;

    if (event->code == INPUT_REL_X) {
        data->ax += event->value;
    } else {
        data->ay += event->value;
    }
    int32_t back = threshold / 4; /* 逆向きに振り返したと見なす量（キビキビ反応するよう小さめ） */
    if (data->fired) {
        /*
         * 発動のあと、止まらなくても次を受け付ける（Conductor のように左右に続けてスワイプできるように）:
         *   - 発動した向きと逆に threshold の 1/4 転がしたら、すぐ次を受け付ける（右→左→右）
         *   - 同じ向きに転がし続けているときは、0.25秒たったら次を受け付ける（右・右・右）
         * （以前はボールが idle-ms 止まるまで次を受け付けず、左右に続けて振ると 2回目以降が出なかった）
         */
        bool reversed = (data->dir == 0 && data->ay > back) || (data->dir == 1 && data->ay < -back) ||
                        (data->dir == 2 && data->ax > back) || (data->dir == 3 && data->ax < -back);
        if (reversed) {
            data->fired = false; /* 逆向きに転がした分はそのまま数える */
        } else if (now - data->fire_time > 250) {
            data->fired = false;
            data->ax = data->ay = 0;
        }
    }
    if (!data->fired && (abs(data->ax) >= threshold || abs(data->ay) >= threshold)) {
        /* 向きは多く動いた方（縦横の差を待つ斜めの処理は、反応が悪くなったのでやめた） */
        int dir = abs(data->ay) >= abs(data->ax) ? (data->ay < 0 ? 0 : 1) : (data->ax < 0 ? 2 : 3);
        data->dir = dir;
        data->listener_index = state ? state->input_device_index : 0;
        k_work_submit(&data->work);
        data->fired = true;
        data->fire_time = now;
        data->ax = data->ay = 0;
    }

    /* Gesture 中はカーソルを動かさない。ZMK v0.2.1 はレイヤー別の設定の中で STOP を返しても無視して
     * カーソルに渡してしまう（input_listener.c の filter_with_input_config が 0 を返す）ので、移動量も 0 にする */
    event->value = 0;
    return ZMK_INPUT_PROC_STOP;
}

static struct zmk_input_processor_driver_api gesture_driver_api = {
    .handle_event = gesture_handle_event,
};

static int gesture_init(const struct device *dev) {
    struct gesture_data *data = dev->data;
    data->dev = dev;
    k_work_init(&data->work, fire_work);
    return 0;
}

#define GESTURE_INST(n)                                                                            \
    static const struct gesture_config gesture_config_##n = {                                      \
        .index = 8 + n,                                                                            \
        .layer = DT_INST_PROP(n, layer),                                                           \
        .positions = {DT_INST_PROP(n, up), DT_INST_PROP(n, down), DT_INST_PROP(n, left),           \
                      DT_INST_PROP(n, right)},                                                     \
        .threshold = DT_INST_PROP(n, threshold),                                                   \
        .idle_ms = DT_INST_PROP(n, idle_ms),                                                       \
    };                                                                                             \
    static struct gesture_data gesture_data_##n;                                                   \
    DEVICE_DT_INST_DEFINE(n, gesture_init, NULL, &gesture_data_##n, &gesture_config_##n, POST_KERNEL,      \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(GESTURE_INST)

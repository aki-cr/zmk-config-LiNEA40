/*
 * LiNEA40 用のオートマウス（ZMK v0.2.1 の入力プロセッサ）
 * ZMK の zip_temp_layer（input_processor_temp_layer.c, MIT）と同じ動きを、値を LiNEA40 Studio からすぐ変えられる形で作り直したもの。
 *   - ボールを動かすと layer のレイヤーに入る（直前のキー入力から require-prior-idle-ms 以内なら入らない）
 *   - timeout-ms 動かさないか、excluded-positions 以外のキーを押すと抜ける
 * ZMK の zip_temp_layer は抜けるまでの時間などがファームに固定されていて、ビルドしないと変えられないため。
 */
#define DT_DRV_COMPAT linea_input_processor_aml

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <drivers/input_processor.h>
#include <zephyr/logging/log.h>
#include <zmk/keymap.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/layer_state_changed.h>

#include "linea_settings.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

/* LiNEA40 はレイヤーの切り替えを1つのオートマウスでしか使わないので、1つだけ持つ */
struct aml_config {
    uint8_t layer;
    int32_t timeout_ms;
    int32_t prior_idle_ms;
    uint32_t excluded_lo, excluded_hi; /* 抜けないキー（0〜31番・32〜63番のビット） */
};

/* & 31 は、使わない側の式でもシフトの数が負や32以上にならないようにするため */
#define AML_EXCL_BIT(node, prop, idx)                                                              \
    | (DT_PROP_BY_IDX(node, prop, idx) < 32 ? BIT(DT_PROP_BY_IDX(node, prop, idx) & 31) : 0)
#define AML_EXCL_BIT_HI(node, prop, idx)                                                           \
    | (DT_PROP_BY_IDX(node, prop, idx) >= 32 ? BIT((DT_PROP_BY_IDX(node, prop, idx) - 32) & 31) : 0)

static const struct aml_config cfg = {
    .layer = DT_INST_PROP(0, layer),
    .timeout_ms = DT_INST_PROP(0, timeout_ms),
    .prior_idle_ms = DT_INST_PROP(0, require_prior_idle_ms),
    .excluded_lo = 0 DT_INST_FOREACH_PROP_ELEM(0, excluded_positions, AML_EXCL_BIT),
    .excluded_hi = 0 DT_INST_FOREACH_PROP_ELEM(0, excluded_positions, AML_EXCL_BIT_HI),
};

static struct {
    struct k_mutex lock;
    bool is_active;
    int64_t last_tapped;
} st;

static int32_t timeout_ms(void) { return linea_setting_get(LINEA_SET_AML_TIMEOUT, cfg.timeout_ms); }
static int32_t prior_idle_ms(void) { return linea_setting_get(LINEA_SET_AML_PRIOR_IDLE, cfg.prior_idle_ms); }
static bool excluded(uint32_t pos) {
    if (pos < 32) {
        return (uint32_t)linea_setting_get(LINEA_SET_AML_EXCL_LO, (int32_t)cfg.excluded_lo) & BIT(pos);
    }
    if (pos < 64) {
        return (uint32_t)linea_setting_get(LINEA_SET_AML_EXCL_HI, (int32_t)cfg.excluded_hi) & BIT(pos - 32);
    }
    return false;
}

/* レイヤーの出し入れ（ZMK と同じく、入力の流れの中ではなく作業キューで行う） */
static void set_active(bool on) {
    if (st.is_active == on) {
        return;
    }
    st.is_active = on;
    if (on) {
        zmk_keymap_layer_activate(cfg.layer);
    } else {
        zmk_keymap_layer_deactivate(cfg.layer);
    }
}

static atomic_t want_on = ATOMIC_INIT(0);
static void activate_work_cb(struct k_work *work) {
    k_mutex_lock(&st.lock, K_FOREVER);
    if (atomic_clear(&want_on)) {
        set_active(true);
    }
    k_mutex_unlock(&st.lock);
}
static K_WORK_DEFINE(activate_work, activate_work_cb);

static void disable_work_cb(struct k_work *work) {
    k_mutex_lock(&st.lock, K_FOREVER);
    if (zmk_keymap_layer_active(cfg.layer)) {
        set_active(false);
    }
    k_mutex_unlock(&st.lock);
}
static K_WORK_DELAYABLE_DEFINE(disable_work, disable_work_cb);

static int aml_handle_event(const struct device *dev, struct input_event *event, uint32_t param1,
                            uint32_t param2, struct zmk_input_processor_state *state) {
    k_mutex_lock(&st.lock, K_FOREVER);
    if (!st.is_active && (st.last_tapped + prior_idle_ms()) <= k_uptime_get()) {
        atomic_set(&want_on, 1);
        k_work_submit(&activate_work);
    }
    int32_t t = timeout_ms();
    if (t > 0) {
        k_work_reschedule(&disable_work, K_MSEC(t));
    }
    k_mutex_unlock(&st.lock);
    return ZMK_INPUT_PROC_CONTINUE;
}

static int aml_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        if (pos->state) {
            k_mutex_lock(&st.lock, K_FOREVER);
            if (st.is_active && !excluded(pos->position)) {
                set_active(false);
            }
            k_mutex_unlock(&st.lock);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    const struct zmk_keycode_state_changed *kc = as_zmk_keycode_state_changed(eh);
    if (kc) {
        if (kc->state) {
            k_mutex_lock(&st.lock, K_FOREVER);
            st.last_tapped = kc->timestamp;
            k_mutex_unlock(&st.lock);
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (as_zmk_layer_state_changed(eh)) {
        k_mutex_lock(&st.lock, K_FOREVER);
        /* ほかの理由でレイヤーが抜けたら、こちらも抜けた扱いにする */
        if (st.is_active && !zmk_keymap_layer_active(zmk_keymap_layer_index_to_id(cfg.layer))) {
            st.is_active = false;
            k_work_cancel_delayable(&disable_work);
        }
        k_mutex_unlock(&st.lock);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(linea_aml, aml_listener);
ZMK_SUBSCRIPTION(linea_aml, zmk_position_state_changed);
ZMK_SUBSCRIPTION(linea_aml, zmk_keycode_state_changed);
ZMK_SUBSCRIPTION(linea_aml, zmk_layer_state_changed);

static int aml_init(const struct device *dev) {
    k_mutex_init(&st.lock);
    return 0;
}

static struct zmk_input_processor_driver_api aml_driver_api = {
    .handle_event = aml_handle_event,
};

DEVICE_DT_INST_DEFINE(0, aml_init, NULL, NULL, &cfg, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                      &aml_driver_api);

#endif

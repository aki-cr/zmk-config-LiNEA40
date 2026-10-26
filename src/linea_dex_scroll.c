/*
 * DeX モードのスクロール（ZMK v0.2.1 の入力プロセッサ）
 * DeX モード（linea_dex.c。プロファイル 3）のときだけ、ホイールの信号の向きを逆にし、量に倍率をかける。
 * Mac はスクロールの向きを「ナチュラル」にしているが、Android はマウスのホイールをそのまま使うので逆になるため。
 * ほかのプロファイルと USB では何もしない。トラボのスクロール・エンコーダーのどちらの最後にも置く。
 *
 * 慣性: Mac では LiNEA40 Scroll（Mac 側）が慣性をつけるが、Android のホイールは回した分だけきっちり止まる。
 * そこで DeX モードのときだけ、転がすのをやめたあとも、最後の速さから少しずつ弱めながら段を送り続ける。
 *   - 速さは、最近届いた段の間隔から出す（段/秒）
 *   - 最後の段から STOP_MS 届かなければ流し始める。減り方は設定 31（ms。大きいほど長く流れる。0 = 慣性なし）
 *   - また転がしたら、流れている分はすぐ止める
 *   - 流す段はこの部品自身を「入力の出どころ」にして input_report_rel で出し、
 *     LiNEA40_right.overlay の linea_dex_inertia_listener（処理なし）がそのまま送る（向き・倍率はもうかけてある）
 */
#define DT_DRV_COMPAT linea_input_processor_dex_scroll

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>

#include "linea_settings.h"

#define STOP_MS 60      /* これだけ段が届かなければ、転がすのをやめたとみなして流し始める */
#define FRAME_MS 16     /* 流すときの間隔 */
#define MIN_SPEED 6     /* これより遅ければ流さない・止める（段/秒） */
#define MAX_SPEED 90    /* 流し始めの速さの上限（段/秒） */

struct dex_scroll_data {
    const struct device *dev;
    int32_t rem_v, rem_h; /* 倍率をかけた端数（縦・横） */
    int64_t last_time;
    /* 慣性（縦・横それぞれの速さ。段/秒 × 1000） */
    struct k_spinlock lock;
    struct k_work_delayable glide_work;
    int64_t last_v, last_h; /* 最後に段が届いた時刻 */
    int32_t vel_v, vel_h;
    int32_t pos_v, pos_h;   /* 流している間の端数（段 × 1000） */
    bool gliding;
};

/* 届いた段から速さを更新する（直前の段からの間隔で、なめらかに） */
static void track(int64_t now, int64_t *last, int32_t *vel, int32_t out) {
    int64_t dt = now - *last;
    *last = now;
    if (out == 0) {
        return;
    }
    if (dt > 150 || (int64_t)*vel * out < 0) {
        *vel = 0; /* 止まっていた・向きが変わった */
    }
    int32_t inst = out * 1000 * 1000 / (int32_t)MAX(dt, 8);
    *vel = (*vel * 6 + inst * 4) / 10;
    *vel = CLAMP(*vel, -MAX_SPEED * 1000, MAX_SPEED * 1000);
}

/* 速さを1コマ分すすめて、送る段を返す */
static int32_t step(int32_t *vel, int32_t *pos, int32_t tau) {
    if (abs(*vel) < MIN_SPEED * 1000) {
        *vel = 0;
        *pos = 0;
        return 0;
    }
    *pos += *vel * FRAME_MS / 1000;
    int32_t out = *pos / 1000;
    *pos -= out * 1000;
    *vel -= (int32_t)((int64_t)*vel * FRAME_MS / tau);
    return out;
}

static void glide_work_handler(struct k_work *work) {
    struct k_work_delayable *dw = k_work_delayable_from_work(work);
    struct dex_scroll_data *data = CONTAINER_OF(dw, struct dex_scroll_data, glide_work);
    int32_t tau = linea_setting_get(LINEA_SET_DEX_INERTIA, LINEA_DEX_INERTIA_DEFAULT);

    k_spinlock_key_t key = k_spin_lock(&data->lock);
    if (!data->gliding) {
        /* 最後の段から STOP_MS たったか（まだ転がしているなら待つ） */
        int64_t idle = k_uptime_get() - MAX(data->last_v, data->last_h);
        if (idle < STOP_MS) {
            k_spin_unlock(&data->lock, key);
            k_work_schedule(&data->glide_work, K_MSEC(STOP_MS - idle));
            return;
        }
        data->gliding = true;
        data->pos_v = data->pos_h = 0;
    }
    int32_t dv = 0, dh = 0;
    if (tau > 0 && linea_dex_active()) {
        dv = step(&data->vel_v, &data->pos_v, tau);
        dh = step(&data->vel_h, &data->pos_h, tau);
    } else {
        data->vel_v = data->vel_h = 0;
    }
    bool more = data->vel_v != 0 || data->vel_h != 0;
    data->gliding = more;
    k_spin_unlock(&data->lock, key);

    if (dv) {
        input_report_rel(data->dev, INPUT_REL_WHEEL, dv, dh == 0, K_NO_WAIT);
    }
    if (dh) {
        input_report_rel(data->dev, INPUT_REL_HWHEEL, dh, true, K_NO_WAIT);
    }
    if (more) {
        k_work_schedule(&data->glide_work, K_MSEC(FRAME_MS));
    }
}

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

    /* 慣性: 転がしている間は速さを覚えておき、流れている分は止める。やめたら流し始める */
    if (out != 0 && linea_setting_get(LINEA_SET_DEX_INERTIA, LINEA_DEX_INERTIA_DEFAULT) > 0) {
        k_spinlock_key_t key = k_spin_lock(&data->lock);
        if (data->gliding) {
            data->gliding = false;
            data->vel_v = data->vel_h = 0;
        }
        if (event->code == INPUT_REL_WHEEL) {
            track(now, &data->last_v, &data->vel_v, out);
        } else {
            track(now, &data->last_h, &data->vel_h, out);
        }
        k_spin_unlock(&data->lock, key);
        k_work_reschedule(&data->glide_work, K_MSEC(STOP_MS));
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static struct zmk_input_processor_driver_api dex_scroll_driver_api = {
    .handle_event = dex_scroll_handle_event,
};

static int dex_scroll_init(const struct device *dev) {
    struct dex_scroll_data *data = dev->data;
    data->dev = dev;
    k_work_init_delayable(&data->glide_work, glide_work_handler);
    return 0;
}

#define DEX_SCROLL_INST(n)                                                                         \
    static struct dex_scroll_data dex_scroll_data_##n = {};                                        \
    DEVICE_DT_INST_DEFINE(n, dex_scroll_init, NULL, &dex_scroll_data_##n, NULL, POST_KERNEL,       \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &dex_scroll_driver_api);

DT_INST_FOREACH_STATUS_OKAY(DEX_SCROLL_INST)

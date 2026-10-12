/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * LiNEA40 用: ZMK v0.2.1 の combo.c を写し、コンボの一覧をファームに固定せず、
 * LiNEA40 Studio から Bluetooth ですぐ書き換えられるようにしたもの（キーボード本体に保存する）。
 * 同時押しの見分け方（候補の絞り込み・タイムアウト・離したときの扱い）は ZMK と同じ。
 * 変えたところ:
 *   - combo_cfg を const の配列から、メモリ上の表（combos[]）にした。最初はキーマップの linea,combos の値
 *   - レイヤーの指定をビットの並び（layers_mask。0 = 全レイヤー）にした
 *   - コンボの仮想キー番号を LINEA_COMBO_VKP_BASE からにした（ZMK のコンボを使わないので）
 *   - 書き換えは、コンボの途中でないときにシステムの作業キュー（キーの処理と同じ流れ）で入れ替える
 * キーの知らせはリンクした順に配られるので、CMakeLists.txt で ZMK 本体の keymap より前に入れる（hold-tap の後）。
 */

#define DT_DRV_COMPAT linea_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/kernel.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define LINEA_MAX_COMBOS 32
#define LINEA_COMBO_VKP_BASE 500 /* ほかの仮想キー番号（センサー・入力の処理）と重ならない番号 */

struct combo_cfg {
    int32_t key_positions[CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO];
    int32_t key_position_len;
    struct zmk_behavior_binding behavior;
    int32_t timeout_ms;
    int32_t require_prior_idle_ms;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
    // the virtual key position is a key position outside the range used by the keyboard.
    // it is necessary so hold-taps can uniquely identify a behavior.
    int32_t virtual_key_position;
    uint32_t layers_mask; /* 効くレイヤーのビット。0 = 全レイヤー */
};

/* 今のコンボの表 */
static struct combo_cfg combos[LINEA_MAX_COMBOS];
static int combo_count = 0;

struct active_combo {
    const struct combo_cfg *combo;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint32_t key_positions_pressed_count;
    struct zmk_position_state_changed_event
        key_positions_pressed[CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO];
};

struct combo_candidate {
    const struct combo_cfg *combo;
    // the time after which this behavior should be removed from candidates.
    // by keeping track of when the candidate should be cleared there is no
    // possibility of accidental releases.
    int64_t timeout_at;
};

static uint32_t pressed_keys_count = 0;
// set of keys pressed
static struct zmk_position_state_changed_event pressed_keys[CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO] = {};
// the set of candidate combos based on the currently pressed_keys
static struct combo_candidate candidates[CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY];
// the last candidate that was completely pressed
static const struct combo_cfg *fully_pressed_combo = NULL;
// a lookup dict that maps a key position to all combos on that position
static const struct combo_cfg *combo_lookup[ZMK_KEYMAP_LEN][CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY] = {NULL};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
static struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {NULL};
static int active_combo_count = 0;

static struct k_work_delayable timeout_task;
static int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
static int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
static int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

// Store the combo key pointer in the combos array, one pointer for each key position
// The combos are sorted shortest-first, then by virtual-key-position.
static int initialize_combo(const struct combo_cfg *new_combo) {
    for (int i = 0; i < new_combo->key_position_len; i++) {
        int32_t position = new_combo->key_positions[i];
        if (position >= ZMK_KEYMAP_LEN) {
            LOG_ERR("Unable to initialize combo, key position %d does not exist", position);
            return -EINVAL;
        }

        const struct combo_cfg *insert_combo = new_combo;
        bool set = false;
        for (int j = 0; j < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; j++) {
            const struct combo_cfg *combo_at_j = combo_lookup[position][j];
            if (combo_at_j == NULL) {
                combo_lookup[position][j] = insert_combo;
                set = true;
                break;
            }
            if (combo_at_j->key_position_len < insert_combo->key_position_len ||
                (combo_at_j->key_position_len == insert_combo->key_position_len &&
                 combo_at_j->virtual_key_position < insert_combo->virtual_key_position)) {
                continue;
            }
            // put insert_combo in this spot, move all other combos up.
            combo_lookup[position][j] = insert_combo;
            insert_combo = combo_at_j;
        }
        if (!set) {
            LOG_ERR("Too many combos for key position %d, CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY %d.",
                    position, CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY);
            return -ENOMEM;
        }
    }
    return 0;
}

static bool combo_active_on_layer(const struct combo_cfg *combo, uint8_t layer) {
    if (combo->layers_mask == 0) {
        // 0 is global layer scope
        return true;
    }
    return layer < 32 && (combo->layers_mask & BIT(layer));
}

static bool is_quick_tap(const struct combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        const struct combo_cfg *combo = combo_lookup[position][i];
        if (combo == NULL) {
            return number_of_combo_candidates;
        }
        if (combo_active_on_layer(combo, highest_active_layer) && !is_quick_tap(combo, timestamp)) {
            candidates[number_of_combo_candidates].combo = combo;
            candidates[number_of_combo_candidates].timeout_at = timestamp + combo->timeout_ms;
            number_of_combo_candidates++;
        }
        // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
    }
    return number_of_combo_candidates;
}

static int filter_candidates(int32_t position) {
    // this code iterates over candidates and the lookup together to filter in O(n)
    // assuming they are both sorted on key_position_len, virtual_key_position
    int matches = 0, lookup_idx = 0, candidate_idx = 0;
    while (lookup_idx < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY &&
           candidate_idx < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY) {
        const struct combo_cfg *candidate = candidates[candidate_idx].combo;
        const struct combo_cfg *lookup = combo_lookup[position][lookup_idx];
        if (candidate == NULL || lookup == NULL) {
            break;
        }
        if (candidate->virtual_key_position == lookup->virtual_key_position) {
            candidates[matches] = candidates[candidate_idx];
            matches++;
            candidate_idx++;
            lookup_idx++;
        } else if (candidate->key_position_len > lookup->key_position_len) {
            lookup_idx++;
        } else if (candidate->key_position_len < lookup->key_position_len) {
            candidate_idx++;
        } else if (candidate->virtual_key_position > lookup->virtual_key_position) {
            lookup_idx++;
        } else if (candidate->virtual_key_position < lookup->virtual_key_position) {
            candidate_idx++;
        }
    }
    // clear unmatched candidates
    for (int i = matches; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        candidates[i].combo = NULL;
    }
    // LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        if (candidates[i].combo == NULL) {
            break;
        }
        if (candidates[i].timeout_at < first_timeout) {
            first_timeout = candidates[i].timeout_at;
        }
    }
    return first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    int remaining_candidates = 0;
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        struct combo_candidate *candidate = &candidates[i];
        if (candidate->combo == NULL) {
            break;
        }
        if (candidate->timeout_at > timestamp) {
            bool need_to_bubble_up = remaining_candidates != i;
            if (need_to_bubble_up) {
                // bubble up => reorder candidates so they're contiguous
                candidates[remaining_candidates].combo = candidate->combo;
                candidates[remaining_candidates].timeout_at = candidate->timeout_at;
                // clear the previous location
                candidates[i].combo = NULL;
                candidates[i].timeout_at = 0;
            }

            remaining_candidates++;
        } else {
            candidate->combo = NULL;
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int clear_candidates() {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY; i++) {
        if (candidates[i].combo == NULL) {
            return i;
        }
        candidates[i].combo = NULL;
    }
    return CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}


static int release_pressed_keys() {
    uint32_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(const struct combo_cfg *combo, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = combo->virtual_key_position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(const struct combo_cfg *combo, int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = combo->virtual_key_position,
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length = MIN(pressed_keys_count, active_combo->combo->key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(const struct combo_cfg *combo) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo == NULL) {
            active_combos[i].combo = combo;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(const struct combo_cfg *combo) {
    struct active_combo *active_combo = store_active_combo(combo);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo, active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count].combo = NULL;
    active_combos[active_combo_count] = (struct active_combo){0};
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed =
            active_combo->key_positions_pressed_count == active_combo->combo->key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            if ((active_combo->combo->slow_release && all_keys_released) ||
                (!active_combo->combo->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    clear_candidates();
    if (fully_pressed_combo != NULL) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = NULL;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (candidates[0].combo == NULL) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }
    update_timeout_task();

    const struct combo_cfg *candidate_combo = candidates[0].combo;
    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    switch (num_candidates) {
    case 0:
        cleanup();
        return ret;
    case 1:
        if (candidate_is_completely_pressed(candidate_combo)) {
            fully_pressed_combo = candidate_combo;
            cleanup();
        }
        return ret;
    default:
        if (candidate_is_completely_pressed(candidate_combo)) {
            fully_pressed_combo = candidate_combo;
        }
        return ret;
    }
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        cleanup();
    }
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static int linea_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(linea_combo, linea_combo_listener);
ZMK_SUBSCRIPTION(linea_combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(linea_combo, zmk_keycode_state_changed);

/* ---- キーマップに書いたコンボ（LiNEA40 Studio で変えていないときの値） ---- */
#define LINEA_LAYER_BIT(node, prop, idx)                                                           \
    | (DT_PROP_BY_IDX(node, prop, idx) < 0 ? 0 : BIT(DT_PROP_BY_IDX(node, prop, idx) & 31))

#define COMBO_INST(n)                                                                              \
    {                                                                                              \
        .timeout_ms = DT_PROP(n, timeout_ms),                                                      \
        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),                                \
        .key_positions = DT_PROP(n, key_positions),                                                \
        .key_position_len = DT_PROP_LEN(n, key_positions),                                         \
        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                                              \
        .slow_release = DT_PROP(n, slow_release),                                                  \
        .layers_mask = 0 DT_FOREACH_PROP_ELEM(n, layers, LINEA_LAYER_BIT),                         \
    },

static const struct combo_cfg default_combos[] = {DT_INST_FOREACH_CHILD(0, COMBO_INST)};

/* 表から、キーごとの候補の一覧（combo_lookup）を作り直す */
static void rebuild_lookup(void) {
    memset(combo_lookup, 0, sizeof(combo_lookup));
    for (int i = 0; i < combo_count; i++) {
        combos[i].virtual_key_position = LINEA_COMBO_VKP_BASE + i;
        initialize_combo(&combos[i]);
    }
}

static void load_defaults(void) {
    combo_count = MIN(ARRAY_SIZE(default_combos), LINEA_MAX_COMBOS);
    for (int i = 0; i < combo_count; i++) {
        combos[i] = default_combos[i];
    }
}

static int combo_init(void) {
    k_work_init_delayable(&timeout_task, combo_timeout_handler);
    load_defaults();
    rebuild_lookup();
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS) && IS_ENABLED(CONFIG_SETTINGS)
/*
 * ---- LiNEA40 Studio から書き換える・本体に保存する ----
 * 動作は「動作の番号（local id。ZMK Studio と同じ番号）＋値2つ」で受け渡す。
 * 保存・受け渡しの1件（26バイト）:
 *   [キーの数, キー番号×4（使わない所は 0xff）, 印(1=slow-release), timeout(2), require-prior-idle(2, 符号付き),
 *    レイヤーのビット(4), 動作の番号(2), 値1(4), 値2(4)]  数値はリトルエンディアン
 */
#define REC_LEN 26
struct combo_rec {
    uint8_t data[REC_LEN];
};

static void rec_from_combo(const struct combo_cfg *c, struct combo_rec *r) {
    uint8_t *d = r->data;
    memset(d, 0xff, 5);
    d[0] = (uint8_t)c->key_position_len;
    for (int k = 0; k < c->key_position_len && k < 4; k++) {
        d[1 + k] = (uint8_t)c->key_positions[k];
    }
    d[5] = c->slow_release ? 1 : 0;
    sys_put_le16((uint16_t)c->timeout_ms, &d[6]);
    sys_put_le16((uint16_t)(int16_t)c->require_prior_idle_ms, &d[8]);
    sys_put_le32(c->layers_mask, &d[10]);
    sys_put_le16(zmk_behavior_get_local_id(c->behavior.behavior_dev), &d[14]);
    sys_put_le32(c->behavior.param1, &d[16]);
    sys_put_le32(c->behavior.param2, &d[20]);
    d[24] = d[25] = 0;
}

/* 受け取った1件をコンボにする。動作が見つからない・値がおかしいときは false */
static bool combo_from_rec(const struct combo_rec *r, struct combo_cfg *c) {
    const uint8_t *d = r->data;
    int len = d[0];
    if (len < 2 || len > MIN(4, CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO)) {
        return false;
    }
    memset(c, 0, sizeof(*c));
    for (int k = 0; k < len; k++) {
        if (d[1 + k] >= ZMK_KEYMAP_LEN) {
            return false;
        }
        c->key_positions[k] = d[1 + k];
    }
    c->key_position_len = len;
    c->slow_release = d[5] & 1;
    c->timeout_ms = sys_get_le16(&d[6]);
    c->require_prior_idle_ms = (int16_t)sys_get_le16(&d[8]);
    c->layers_mask = sys_get_le32(&d[10]);
    const char *name = zmk_behavior_find_behavior_name_from_local_id(sys_get_le16(&d[14]));
    if (!name || c->timeout_ms <= 0 || c->timeout_ms > 2000) {
        return false;
    }
    c->behavior.behavior_dev = name;
    c->behavior.param1 = sys_get_le32(&d[16]);
    c->behavior.param2 = sys_get_le32(&d[20]);
    return true;
}

/* 受け取り中の表（書き込みが全部そろったら入れ替える） */
static struct combo_rec staged[LINEA_MAX_COMBOS];
static int staged_count = -1; /* -1 = 入れ替えるものは無い */
static bool staged_is_default = false;
static bool staged_save = false;

static bool combo_busy(void) {
    return pressed_keys_count > 0 || active_combo_count > 0 || candidates[0].combo != NULL ||
           fully_pressed_combo != NULL;
}

static void apply_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(apply_work, apply_work_handler);

static void apply_work_handler(struct k_work *work) {
    if (staged_count < 0) {
        return;
    }
    /* コンボの途中なら、少し待ってからにする */
    if (combo_busy()) {
        k_work_reschedule(&apply_work, K_MSEC(20));
        return;
    }
    if (staged_is_default) {
        load_defaults();
    } else {
        int n = 0;
        for (int i = 0; i < staged_count && n < LINEA_MAX_COMBOS; i++) {
            if (combo_from_rec(&staged[i], &combos[n])) {
                n++;
            } else {
                LOG_WRN("LiNEA40 combo %d is invalid, skipped", i);
            }
        }
        combo_count = n;
    }
    rebuild_lookup();
    if (staged_save) {
        if (staged_is_default) {
            settings_delete("lcombo/table");
        } else {
            settings_save_one("lcombo/table", staged, sizeof(struct combo_rec) * staged_count);
        }
    }
    staged_count = -1;
}

/* 保存してある表を読む（動作の番号から名前への対応は全部読み終わってから決まるので、commit で入れ替える） */
static int combos_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (!settings_name_steq(name, "table", NULL)) {
        return -ENOENT;
    }
    if (len % sizeof(struct combo_rec) != 0 || len > sizeof(staged)) {
        return -EINVAL;
    }
    ssize_t n = read_cb(cb_arg, staged, len);
    if (n < 0) {
        return (int)n;
    }
    staged_count = n / sizeof(struct combo_rec);
    staged_is_default = false;
    staged_save = false;
    return 0;
}
static int combos_settings_commit(void) {
    if (staged_count >= 0) {
        k_work_reschedule(&apply_work, K_NO_WAIT);
    }
    return 0;
}
/* 「linea」は linea_settings.c が使っているので、別の名前にする（同じ名前だと片方しか読まれない） */
SETTINGS_STATIC_HANDLER_DEFINE(linea_combos, "lcombo", NULL, combos_settings_set, combos_settings_commit, NULL);

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* 4a4e980b-132a-4338-9860-d6d20d9cb209（設定のサービスの続き番号） */
static struct bt_uuid_128 combo_svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x4a4e980b, 0x132a, 0x4338, 0x9860, 0xd6d20d9cb209));
/* 999f06e2-c35d-41fd-90c8-0000a2705140 */
static struct bt_uuid_128 combo_chr_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x999f06e2, 0xc35d, 0x41fd, 0x90c8, 0xa2705140));

/* 書き込み中の表（BT の流れで受け取り、そろったら staged に移して入れ替える） */
static struct combo_rec incoming[LINEA_MAX_COMBOS];

/* 読み出し: [0x01, 件数, 最大件数, 1件 26バイト × 件数] */
static ssize_t read_combos(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                           uint16_t offset) {
    static uint8_t out[3 + REC_LEN * LINEA_MAX_COMBOS];
    if (offset == 0) {
        out[0] = 0x01;
        out[1] = (uint8_t)combo_count;
        out[2] = LINEA_MAX_COMBOS;
        for (int i = 0; i < combo_count; i++) {
            rec_from_combo(&combos[i], (struct combo_rec *)&out[3 + REC_LEN * i]);
        }
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, out, 3 + REC_LEN * combo_count);
}

/*
 * 書き込み:
 *   [0x01, 番号, 1件の前半 13バイト（キーの数〜require-prior-idle の次の1バイト目まで: data[0..12]）]
 *   [0x02, 番号, 1件の後半 13バイト（data[13..25]）]
 *   [0x03, 件数]  受け取った件数分で入れ替えて保存する
 *   [0x04]        キーマップに書いたコンボに戻す（保存した表を消す）
 */
static ssize_t write_combos(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                            uint16_t len, uint16_t offset, uint8_t flags) {
    const uint8_t *b = buf;
    if (offset != 0 || len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    switch (b[0]) {
    case 0x01:
    case 0x02:
        if (len != 2 + REC_LEN / 2 || b[1] >= LINEA_MAX_COMBOS) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        memcpy(&incoming[b[1]].data[b[0] == 0x01 ? 0 : REC_LEN / 2], &b[2], REC_LEN / 2);
        break;
    case 0x03:
        if (len != 2 || b[1] > LINEA_MAX_COMBOS) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        memcpy(staged, incoming, sizeof(struct combo_rec) * b[1]);
        staged_count = b[1];
        staged_is_default = false;
        staged_save = true;
        k_work_reschedule(&apply_work, K_NO_WAIT);
        break;
    case 0x04:
        staged_count = 0;
        staged_is_default = true;
        staged_save = true;
        k_work_reschedule(&apply_work, K_NO_WAIT);
        break;
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    return len;
}

BT_GATT_SERVICE_DEFINE(linea_combo_svc, BT_GATT_PRIMARY_SERVICE(&combo_svc_uuid),
                       BT_GATT_CHARACTERISTIC(&combo_chr_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                                              read_combos, write_combos, NULL));
#endif /* CONFIG_ZMK_BLE */
#endif /* LOCAL_IDS && SETTINGS */

#endif

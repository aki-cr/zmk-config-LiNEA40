/*
 * DeX モード: 決めた Bluetooth プロファイル（既定は 3）につないでいる間だけ、Mac のキー操作を
 * Android（Samsung DeX）の同じ操作に置き換えて送る。ほかのプロファイルと USB では何もしない。
 *
 *   ⌘ → Ctrl（⌘C / ⌘V / ⌘Z / ⌘S / ⌘W / ⌘T / ⌘F / ⌘A / ⌘+クリック などはこれで同じになる）
 *   ⌘Tab → Alt+Tab（⌘を押している間は Alt も押したまま。続けて Tab で次のアプリ）
 *   ⌘Space → Meta（DeX のアプリ一覧）     ⌘Q → Alt+F4
 *   ⌘← / ⌘→ → Home / End                 ⌘↑ / ⌘↓ → Ctrl+Home / Ctrl+End
 *   ⌘Backspace → Shift+Home のあと Backspace（行頭まで消す）
 *   ⇧⌘3 / ⇧⌘4 / ⇧⌘5 → PrintScreen（Android のスクリーンショット）
 *   ⌥← / ⌥→ / ⌥Backspace / ⌥Delete → Ctrl+同じキー（単語単位）
 *   キーマップに書いた Mac 用の Ctrl つきキー: ⌃A / ⌃E → Home / End、⌃↑（Mission Control）→ Meta+Tab（最近のアプリ）、
 *   ⌃← / ⌃→ / ⌃↓（デスクトップの切り替えなど。DeX に同じ操作が無い）→ 何も送らない
 *
 * 置き換えは、キーを押したときに決めて離すときも同じにする（押している間にプロファイルが変わっても、押しっぱなしにならない）。
 * ZMK の hid_listener より前にキーの知らせを受け取って中身を書き換える。消したい修飾キーは hid の masked_modifiers で隠す。
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <dt-bindings/zmk/hid_usage.h>
#include <dt-bindings/zmk/hid_usage_pages.h>
#include <dt-bindings/zmk/modifiers.h>
#include <zmk/keys.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include "linea_settings.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* プロファイル 3（BT_SEL 2）。LiNEA40 Studio の設定 27 で変えられる（0 = 使わない） */
#define DEX_PROFILE_DEFAULT 3

#define GUI (MOD_LGUI | MOD_RGUI)
#define CTL (MOD_LCTL | MOD_RCTL)
#define ALT (MOD_LALT | MOD_RALT)
#define SFT (MOD_LSFT | MOD_RSFT)
#define LALT_INDEX 2 /* zmk_hid_register_mod に渡す Alt の番号（0xE2 - 0xE0） */

/* 押している間のキーの置き換え（離すときに同じ置き換えをする） */
struct held {
    bool used;
    uint16_t page;
    uint32_t orig;     /* 押されたキー */
    uint32_t keycode;  /* 代わりに送るキー */
    uint8_t implicit;  /* 代わりに送る修飾キー */
    uint8_t mask;      /* 押している間に隠す修飾キー */
    bool swallow;      /* 何も送らない */
    bool cmd;          /* ⌘を Ctrl にしたもの */
};

#define HELD_MAX 10
static struct held held[HELD_MAX];
static bool switcher; /* ⌘Tab で Alt を押したまま */

static bool dex_on(void) {
    int profile = linea_setting_get(LINEA_SET_DEX_PROFILE, DEX_PROFILE_DEFAULT);
    if (profile <= 0) {
        return false;
    }
    struct zmk_endpoint_instance ep = zmk_endpoints_selected();
    return ep.transport == ZMK_TRANSPORT_BLE && ep.ble.profile_index == profile - 1;
}

static int cmd_count(void) {
    int n = 0;
    for (int i = 0; i < HELD_MAX; i++) {
        n += held[i].used && held[i].cmd;
    }
    return n;
}

static void apply_mask(void) {
    uint8_t mask = switcher ? CTL : 0;
    for (int i = 0; i < HELD_MAX; i++) {
        if (held[i].used) {
            mask |= held[i].mask;
        }
    }
    zmk_hid_masked_modifiers_set(mask);
}

static struct held *find(uint16_t page, uint32_t keycode) {
    for (int i = 0; i < HELD_MAX; i++) {
        if (held[i].used && held[i].page == page && held[i].orig == keycode) {
            return &held[i];
        }
    }
    return NULL;
}

static struct held *add(const struct zmk_keycode_state_changed *ev) {
    for (int i = 0; i < HELD_MAX; i++) {
        if (!held[i].used) {
            held[i] = (struct held){.used = true,
                                    .page = ev->usage_page,
                                    .orig = ev->keycode,
                                    .keycode = ev->keycode,
                                    .implicit = ev->implicit_modifiers};
            return &held[i];
        }
    }
    return NULL;
}

/* ⌘Backspace: 先に Shift+Home を1回送って、行頭までを選んでおく */
static void tap_shift_home(void) {
    zmk_hid_implicit_modifiers_press(MOD_LSFT);
    zmk_hid_press(ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_HOME));
    zmk_endpoints_send_report(HID_USAGE_KEY);
    zmk_hid_release(ZMK_HID_USAGE(HID_USAGE_KEY, HID_USAGE_KEY_KEYBOARD_HOME));
    zmk_hid_implicit_modifiers_release();
    zmk_endpoints_send_report(HID_USAGE_KEY);
}

/* 押したキーの置き換えを決める。置き換えないときは false */
static bool translate(const struct zmk_keycode_state_changed *ev, struct held *h) {
    uint32_t k = ev->keycode;
    uint8_t imp = ev->implicit_modifiers;
    bool cmd_held = cmd_count() > 0;
    bool cmd = cmd_held || (imp & GUI);
    bool alt = (zmk_hid_get_explicit_mods() | imp) & ALT;

    if (ev->usage_page != HID_USAGE_KEY) {
        if (!(imp & GUI)) {
            return false;
        }
        h->implicit = (imp & ~GUI) | MOD_LCTL;
        return true;
    }

    /* ⌘そのもの → Ctrl */
    if (k == HID_USAGE_KEY_KEYBOARD_LEFT_GUI || k == HID_USAGE_KEY_KEYBOARD_RIGHT_GUI) {
        h->keycode = k == HID_USAGE_KEY_KEYBOARD_LEFT_GUI ? HID_USAGE_KEY_KEYBOARD_LEFTCONTROL
                                                          : HID_USAGE_KEY_KEYBOARD_RIGHTCONTROL;
        h->cmd = true;
        return true;
    }
    if (is_mod(ev->usage_page, k)) {
        return false;
    }

    if (cmd) {
        /* キーマップの LG(…) の ⌘ は Ctrl にする。押している⌘はもう Ctrl になっている */
        h->implicit = (imp & ~GUI) | (cmd_held ? 0 : MOD_LCTL);
        switch (k) {
        case HID_USAGE_KEY_KEYBOARD_TAB:
            h->mask = CTL;
            h->implicit &= ~CTL;
            if (cmd_held) {
                if (!switcher) {
                    switcher = true;
                    zmk_hid_register_mod(LALT_INDEX);
                }
            } else {
                h->implicit |= MOD_LALT;
            }
            return true;
        case HID_USAGE_KEY_KEYBOARD_LEFTARROW:
        case HID_USAGE_KEY_KEYBOARD_RIGHTARROW:
            h->keycode = k == HID_USAGE_KEY_KEYBOARD_LEFTARROW ? HID_USAGE_KEY_KEYBOARD_HOME
                                                               : HID_USAGE_KEY_KEYBOARD_END;
            h->mask = CTL;
            h->implicit &= ~CTL;
            return true;
        case HID_USAGE_KEY_KEYBOARD_UPARROW:
        case HID_USAGE_KEY_KEYBOARD_DOWNARROW:
            h->keycode = k == HID_USAGE_KEY_KEYBOARD_UPARROW ? HID_USAGE_KEY_KEYBOARD_HOME
                                                             : HID_USAGE_KEY_KEYBOARD_END;
            return true;
        case HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE:
            h->mask = CTL;
            h->implicit &= ~CTL;
            h->keycode = HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE;
            return true;
        case HID_USAGE_KEY_KEYBOARD_Q:
            h->keycode = HID_USAGE_KEY_KEYBOARD_F4;
            h->mask = CTL;
            h->implicit = (h->implicit & ~CTL) | MOD_LALT;
            return true;
        case HID_USAGE_KEY_KEYBOARD_SPACEBAR:
            h->keycode = HID_USAGE_KEY_KEYBOARD_LEFT_GUI;
            h->mask = CTL;
            h->implicit = 0;
            return true;
        case HID_USAGE_KEY_KEYBOARD_3_AND_HASH:
        case HID_USAGE_KEY_KEYBOARD_4_AND_DOLLAR:
        case HID_USAGE_KEY_KEYBOARD_5_AND_PERCENT:
            if (!((zmk_hid_get_explicit_mods() | imp) & SFT)) {
                return true;
            }
            h->keycode = HID_USAGE_KEY_KEYBOARD_PRINTSCREEN;
            h->mask = CTL | SFT;
            h->implicit = 0;
            return true;
        default:
            return true;
        }
    }

    if (alt) {
        switch (k) {
        case HID_USAGE_KEY_KEYBOARD_LEFTARROW:
        case HID_USAGE_KEY_KEYBOARD_RIGHTARROW:
        case HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE:
        case HID_USAGE_KEY_KEYBOARD_DELETE_FORWARD:
            h->mask = ALT;
            h->implicit = (imp & ~ALT) | MOD_LCTL;
            return true;
        default:
            return false;
        }
    }

    /* キーマップに書いた Mac 用の Ctrl つきキー（押している Ctrl はそのまま Ctrl） */
    if ((imp & CTL) && !(zmk_hid_get_explicit_mods() & CTL)) {
        switch (k) {
        case HID_USAGE_KEY_KEYBOARD_A:
        case HID_USAGE_KEY_KEYBOARD_E:
            h->keycode = k == HID_USAGE_KEY_KEYBOARD_A ? HID_USAGE_KEY_KEYBOARD_HOME
                                                             : HID_USAGE_KEY_KEYBOARD_END;
            h->implicit = imp & ~CTL;
            return true;
        case HID_USAGE_KEY_KEYBOARD_UPARROW:
            h->keycode = HID_USAGE_KEY_KEYBOARD_TAB;
            h->implicit = MOD_LGUI;
            return true;
        case HID_USAGE_KEY_KEYBOARD_LEFTARROW:
        case HID_USAGE_KEY_KEYBOARD_RIGHTARROW:
        case HID_USAGE_KEY_KEYBOARD_DOWNARROW:
            if (imp & ~CTL) {
                return false;
            }
            h->swallow = true;
            return true;
        default:
            return false;
        }
    }
    return false;
}

static int dex_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!ev->state) {
        struct held *h = find(ev->usage_page, ev->keycode);
        if (!h) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        struct held done = *h;
        h->used = false;
        if (done.cmd && switcher && cmd_count() == 0) {
            switcher = false;
            zmk_hid_unregister_mod(LALT_INDEX);
        }
        apply_mask();
        if (done.swallow) {
            return ZMK_EV_EVENT_HANDLED;
        }
        ev->keycode = done.keycode;
        ev->implicit_modifiers = done.implicit;
        ev->explicit_modifiers &= ~GUI;
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (!dex_on()) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    struct held *h = add(ev);
    if (!h) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (!translate(ev, h)) {
        h->used = false;
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (h->swallow) {
        return ZMK_EV_EVENT_HANDLED;
    }

    if (h->mask) {
        /* 隠す修飾キーを先に離した形で1回送ってから、キーを押す（同時だと修飾つきで受け取られることがある） */
        apply_mask();
        zmk_endpoints_send_report(HID_USAGE_KEY);
    }
    if (h->orig == HID_USAGE_KEY_KEYBOARD_DELETE_BACKSPACE && h->mask == CTL) {
        tap_shift_home();
    }
    ev->keycode = h->keycode;
    ev->implicit_modifiers = h->implicit;
    ev->explicit_modifiers &= ~GUI;
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(linea_dex, dex_listener);
ZMK_SUBSCRIPTION(linea_dex, zmk_keycode_state_changed);

/*
 * リモートモード: Galaxy のアプリ（LiNEA Remote）が開いている間だけ、キーボードとトラボの入力を
 * Galaxy にはキー入力として送らず、専用の GATT 通知でアプリに渡す。アプリはそれを Mac へ中継し、
 * Mac 側の受け手が仮想キーボード・マウスとして入力する（Mac から見ると、つないだときと同じレポートが届く）。
 *
 * 横取りする場所: ZMK が作り終えたレポートを送る zmk_endpoints_send_report / zmk_endpoints_send_mouse_report
 * （リンカの --wrap で差し替える。CMakeLists.txt）。キーマップ・長押し・コンボ・トラボの加速などはすべて通った後なので、
 * Mac に直接つないだときと同じ中身になる。DeX の置き換え（linea_dex.c）はリモートモードの間は止める。
 *
 * アプリからの書き込み（暗号化必須）:
 *   [0x01] 始める・続ける（2秒ごとに送ってもらう。REMOTE_TIMEOUT_MS 届かなければやめる）
 *   [0x00] やめる
 * アプリへの通知（1回の通知 = 1つのデータ）:
 *   [0x01, キーボードのレポート（修飾キー, 予約, キー×6）]
 *   [0x03, メディアキーのレポート（16ビット×6）]
 *   [0x04, ボタン, X, Y, 縦スクロール, 横スクロール（それぞれ 16ビット リトルエンディアン）]
 *   [0x05] すべて離した（始めたとき・やめたとき）
 * 読み出し: [1 = リモートモード中 / 0]
 *
 * やめる条件: アプリが [0x00] を書いた、書き込みが途切れた、その接続が切れた、プロファイル（つなぎ先）が変わった。
 * やめたら、Galaxy には今の状態をそのまま送り直す（押しっぱなしにならない）。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <dt-bindings/zmk/hid_usage_pages.h>

#include <zmk/hid.h>
#include <zmk/hog.h>
#include <zmk/endpoints.h>
#include <zmk/event_manager.h>
#include <zmk/events/endpoint_changed.h>

#include "linea_settings.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define REMOTE_TIMEOUT_MS 6000

#define FRAME_KEYBOARD 0x01
#define FRAME_CONSUMER 0x03
#define FRAME_MOUSE 0x04
#define FRAME_RELEASE_ALL 0x05

/* 通知1回に入る大きさ（ATT の既定 MTU 23 - 3） */
#define FRAME_MAX 20
BUILD_ASSERT(1 + sizeof(struct zmk_hid_keyboard_report_body) <= FRAME_MAX, "キーボードのレポートが通知に入らない");
BUILD_ASSERT(1 + sizeof(struct zmk_hid_consumer_report_body) <= FRAME_MAX, "メディアキーのレポートが通知に入らない");

/* 7b1d5e00-6c2a-4f0e-9a3b-4c494e454131 */
static struct bt_uuid_128 svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7b1d5e00, 0x6c2a, 0x4f0e, 0x9a3b, 0x4c494e454131));
/* 7b1d5e01-…: アプリが書く（始める・やめる）／読む（今の状態） */
static struct bt_uuid_128 ctl_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7b1d5e01, 0x6c2a, 0x4f0e, 0x9a3b, 0x4c494e454131));
/* 7b1d5e02-…: レポートの通知 */
static struct bt_uuid_128 rpt_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x7b1d5e02, 0x6c2a, 0x4f0e, 0x9a3b, 0x4c494e454131));

static atomic_t active = ATOMIC_INIT(0);
static struct bt_conn *remote_conn; /* 始めたアプリの接続（通知の宛先） */
static K_MUTEX_DEFINE(conn_lock);

int __real_zmk_endpoints_send_report(uint16_t usage_page);
int __real_zmk_endpoints_send_mouse_report(void);
int __wrap_zmk_endpoints_send_report(uint16_t usage_page);
int __wrap_zmk_endpoints_send_mouse_report(void);

bool linea_remote_active(void) {
    return atomic_get(&active) && zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE;
}

/* ---- 送り待ち: キー・メディアキーは順番どおりに全部送る。マウスは送る前なら1つにまとめる ---- */
struct frame {
    uint8_t len;
    uint8_t data[FRAME_MAX];
};

#define QUEUE_LEN 48
static struct frame queue[QUEUE_LEN];
static int q_head, q_count;
static struct k_spinlock q_lock;
static K_SEM_DEFINE(q_sem, 0, 1);

static int16_t sat_add(int16_t a, int16_t b) {
    int32_t s = (int32_t)a + b;
    return s > INT16_MAX ? INT16_MAX : (s < INT16_MIN ? INT16_MIN : (int16_t)s);
}

static void push(const uint8_t *data, uint8_t len) {
    k_spinlock_key_t key = k_spin_lock(&q_lock);
    if (q_count > 0 && data[0] == FRAME_MOUSE) {
        struct frame *tail = &queue[(q_head + q_count - 1) % QUEUE_LEN];
        /* 送る前のマウスで、ボタンが同じなら動きを足し合わせる（なくさずにまとめる） */
        if (tail->data[0] == FRAME_MOUSE && tail->data[1] == data[1]) {
            for (int i = 0; i < 4; i++) {
                int16_t v = sat_add(sys_get_le16(&tail->data[2 + i * 2]), sys_get_le16(&data[2 + i * 2]));
                sys_put_le16(v, &tail->data[2 + i * 2]);
            }
            k_spin_unlock(&q_lock, key);
            return;
        }
    }
    if (q_count == QUEUE_LEN) {
        /* あふれたら古いものを捨てる（キーのレポートは毎回「今の状態」なので、最後のものが届けば押しっぱなしにはならない） */
        q_head = (q_head + 1) % QUEUE_LEN;
        q_count--;
    }
    struct frame *f = &queue[(q_head + q_count) % QUEUE_LEN];
    f->len = len;
    memcpy(f->data, data, len);
    q_count++;
    k_spin_unlock(&q_lock, key);
    k_sem_give(&q_sem);
}

static bool pop(struct frame *out) {
    k_spinlock_key_t key = k_spin_lock(&q_lock);
    bool ok = q_count > 0;
    if (ok) {
        *out = queue[q_head];
        q_head = (q_head + 1) % QUEUE_LEN;
        q_count--;
    }
    k_spin_unlock(&q_lock, key);
    return ok;
}

static void clear_queue(void) {
    k_spinlock_key_t key = k_spin_lock(&q_lock);
    q_head = q_count = 0;
    k_spin_unlock(&q_lock, key);
}

/* ---- 始める・やめる ---- */
static void timeout_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(timeout_work, timeout_fn);

static void start(struct bt_conn *conn) {
    k_mutex_lock(&conn_lock, K_FOREVER);
    if (remote_conn != conn) {
        if (remote_conn) {
            bt_conn_unref(remote_conn);
        }
        remote_conn = bt_conn_ref(conn);
    }
    k_mutex_unlock(&conn_lock);
    k_work_reschedule(&timeout_work, K_MSEC(REMOTE_TIMEOUT_MS));
    if (atomic_set(&active, 1)) {
        return;
    }
    LOG_INF("リモートモードを始める");
    /* Galaxy 側は何も押していない状態にしておく（始めた瞬間に押していたキーが残らないように） */
    struct zmk_hid_keyboard_report_body kb = {0};
    struct zmk_hid_consumer_report_body cs = {0};
    struct zmk_hid_mouse_report_body ms = {0};
    zmk_hog_send_keyboard_report(&kb);
    zmk_hog_send_consumer_report(&cs);
    zmk_hog_send_mouse_report(&ms);
    clear_queue();
    uint8_t rel = FRAME_RELEASE_ALL;
    push(&rel, 1);
    /* 今押しているキーがあれば、Mac 側にもそのまま渡す */
    __wrap_zmk_endpoints_send_report(HID_USAGE_KEY);
}

static void stop(void) {
    k_work_cancel_delayable(&timeout_work);
    if (!atomic_set(&active, 0)) {
        return;
    }
    LOG_INF("リモートモードをやめる");
    uint8_t rel = FRAME_RELEASE_ALL;
    push(&rel, 1);
    /* Galaxy に今の状態を送り直す */
    if (zmk_endpoints_selected().transport == ZMK_TRANSPORT_BLE) {
        __real_zmk_endpoints_send_report(HID_USAGE_KEY);
        __real_zmk_endpoints_send_report(HID_USAGE_CONSUMER);
    }
}

static void timeout_fn(struct k_work *work) {
    LOG_WRN("リモートモード: アプリからの知らせが途切れた");
    stop();
}

static ssize_t write_ctl(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                         uint16_t len, uint16_t offset, uint8_t flags) {
    if (offset != 0 || len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
    }
    if (((const uint8_t *)buf)[0] == 0x01) {
        start(conn);
    } else {
        stop();
    }
    return len;
}

static ssize_t read_ctl(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                        uint16_t len, uint16_t offset) {
    uint8_t v = atomic_get(&active) ? 1 : 0;
    return bt_gatt_attr_read(conn, attr, buf, len, offset, &v, 1);
}

BT_GATT_SERVICE_DEFINE(linea_remote_svc, BT_GATT_PRIMARY_SERVICE(&svc_uuid),
                       BT_GATT_CHARACTERISTIC(&ctl_uuid.uuid,
                                              BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
                                                  BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                                              read_ctl, write_ctl, NULL),
                       BT_GATT_CHARACTERISTIC(&rpt_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
                                              NULL, NULL, NULL),
                       BT_GATT_CCC(NULL, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

/* ---- 送り出し: 専用の低優先度スレッド（bt_gatt_notify は空きを待って止まることがあるので、入力の処理では呼ばない） ---- */
static void remote_thread(void *a, void *b, void *c) {
    struct frame f;
    for (;;) {
        k_sem_take(&q_sem, K_FOREVER);
        while (pop(&f)) {
            k_mutex_lock(&conn_lock, K_FOREVER);
            struct bt_conn *conn = remote_conn ? bt_conn_ref(remote_conn) : NULL;
            k_mutex_unlock(&conn_lock);
            if (!conn) {
                continue;
            }
            int err = bt_gatt_notify(conn, &linea_remote_svc.attrs[4], f.data, f.len);
            if (err) {
                LOG_WRN("リモートモード: 通知に失敗 %d", err);
            }
            bt_conn_unref(conn);
        }
    }
}
K_THREAD_DEFINE(linea_remote_tid, 2048, remote_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO,
                0, 0);

/* ---- 横取り（--wrap） ---- */
int __wrap_zmk_endpoints_send_report(uint16_t usage_page) {
    if (!linea_remote_active()) {
        return __real_zmk_endpoints_send_report(usage_page);
    }
    uint8_t buf[FRAME_MAX];
    switch (usage_page) {
    case HID_USAGE_KEY: {
        struct zmk_hid_keyboard_report_body *body = &zmk_hid_get_keyboard_report()->body;
        buf[0] = FRAME_KEYBOARD;
        memcpy(&buf[1], body, sizeof(*body));
        push(buf, 1 + sizeof(*body));
        return 0;
    }
    case HID_USAGE_CONSUMER: {
        struct zmk_hid_consumer_report_body *body = &zmk_hid_get_consumer_report()->body;
        buf[0] = FRAME_CONSUMER;
        memcpy(&buf[1], body, sizeof(*body));
        push(buf, 1 + sizeof(*body));
        return 0;
    }
    }
    return __real_zmk_endpoints_send_report(usage_page);
}

int __wrap_zmk_endpoints_send_mouse_report(void) {
    if (!linea_remote_active()) {
        return __real_zmk_endpoints_send_mouse_report();
    }
    struct zmk_hid_mouse_report_body *body = &zmk_hid_get_mouse_report()->body;
    uint8_t buf[10] = {FRAME_MOUSE, body->buttons};
    sys_put_le16(body->d_x, &buf[2]);
    sys_put_le16(body->d_y, &buf[4]);
    sys_put_le16(body->d_scroll_y, &buf[6]);
    sys_put_le16(body->d_scroll_x, &buf[8]);
    push(buf, sizeof(buf));
    return 0;
}

/* ---- 接続が切れた・つなぎ先が変わったらやめる ---- */
static void disconnected(struct bt_conn *conn, uint8_t reason) {
    k_mutex_lock(&conn_lock, K_FOREVER);
    bool mine = remote_conn == conn;
    if (mine) {
        bt_conn_unref(remote_conn);
        remote_conn = NULL;
    }
    k_mutex_unlock(&conn_lock);
    if (mine) {
        stop();
    }
}

BT_CONN_CB_DEFINE(linea_remote_conn_cb) = {
    .disconnected = disconnected,
};

static int endpoint_listener(const zmk_event_t *eh) {
    if (as_zmk_endpoint_changed(eh)) {
        stop();
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(linea_remote, endpoint_listener);
ZMK_SUBSCRIPTION(linea_remote, zmk_endpoint_changed);

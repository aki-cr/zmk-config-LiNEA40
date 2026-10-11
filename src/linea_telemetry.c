/*
 * LiNEA40 Studio 用の状態の知らせ
 * レイヤーが切り替わったとき・キーが押された／離されたときに知らせる。
 * ZMK Studio の通信には「今のレイヤー」や「押されたキー」の知らせが無いため、別の窓口を用意する。
 *
 * Bluetooth（専用の GATT 特性・通知）: 1回の通知 = 1つのデータ
 *   レイヤー: [0x02, いちばん上のレイヤー番号, 有効なレイヤーのビット列(4バイト, リトルエンディアン)]
 *   キー:     [0x01, キーの番号, 1=押した / 0=離した]
 *   読み出すと、今のレイヤーのデータが返る。
 * USB（Bluetooth が届かないとき用）: いちばん上のレイヤーが変わったら、
 *   Consumer ページの 0xFE0 + レイヤー番号 を押して離す（未割り当ての番号なので Mac では何も起きない）
 *
 * 入力の処理（キー・トラボ・オートマウス）を止めないことを最優先にする:
 *   - 送信は専用の低優先度スレッド / ワークで行い、イベントの処理中には送らない
 *   - 送り待ちがあふれたら、その知らせは捨てる（待たない）
 *   - Bluetooth の知らせは、相手が受け取りを申し込んでいるときだけ
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/endpoints.h>
#include <zmk/hid.h>
#include <zmk/event_manager.h>
#include <zmk/keymap.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/position_state_changed.h>

/* 4a4e980b-132a-4338-9860-d6d20d9cb207 */
static struct bt_uuid_128 svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x4a4e980b, 0x132a, 0x4338, 0x9860, 0xd6d20d9cb207));
/* 999f06e0-c35d-41fd-90c8-0000a2705140（BT_UUID_128_ENCODE の5つ目は48ビット） */
static struct bt_uuid_128 chr_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x999f06e0, 0xc35d, 0x41fd, 0x90c8, 0xa2705140));

static atomic_t subscribed = ATOMIC_INIT(0);

static void make_layer_frame(uint8_t *buf) {
    buf[0] = 0x02;
    buf[1] = (uint8_t)zmk_keymap_highest_layer_active();
    sys_put_le32((uint32_t)zmk_keymap_layer_state(), &buf[2]);
}

static ssize_t read_state(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                          uint16_t len, uint16_t offset) {
    uint8_t frame[6];
    make_layer_frame(frame);
    return bt_gatt_attr_read(conn, attr, buf, len, offset, frame, sizeof(frame));
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    atomic_set(&subscribed, value == BT_GATT_CCC_NOTIFY);
}

BT_GATT_SERVICE_DEFINE(linea_telemetry_svc, BT_GATT_PRIMARY_SERVICE(&svc_uuid),
                       BT_GATT_CHARACTERISTIC(&chr_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, read_state, NULL, NULL),
                       BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

/* ---- Bluetooth の知らせ: 送り待ちに積んで、専用スレッドが送る ---- */
struct tele_frame {
    uint8_t len;
    uint8_t data[6];
};
K_MSGQ_DEFINE(tele_q, sizeof(struct tele_frame), 16, 1);

static void enqueue(const uint8_t *data, uint8_t len) {
    if (!atomic_get(&subscribed)) {
        return;
    }
    struct tele_frame f = {.len = len};
    memcpy(f.data, data, len);
    (void)k_msgq_put(&tele_q, &f, K_NO_WAIT); /* あふれたら捨てる */
}

static void tele_thread(void *a, void *b, void *c) {
    struct tele_frame f;
    for (;;) {
        k_msgq_get(&tele_q, &f, K_FOREVER);
        bt_gatt_notify(NULL, &linea_telemetry_svc.attrs[2], f.data, f.len);
    }
}
K_THREAD_DEFINE(linea_tele_tid, 1024, tele_thread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0,
                0);

/* ---- USB 用: いちばん上のレイヤーが変わったときだけ、ワークでメディアキーの信号を送る ---- */
static atomic_t pending_top = ATOMIC_INIT(-1);
static int last_sent_top = -1;

static void consumer_work_fn(struct k_work *work) {
    int top = (int)atomic_set(&pending_top, -1);
    if (top < 0 || top == last_sent_top) {
        return;
    }
    last_sent_top = top;
    zmk_key_t usage = 0xFE0 + (top & 0x0F);
    zmk_hid_consumer_press(usage);
    zmk_endpoints_send_report(HID_USAGE_CONSUMER);
    zmk_hid_consumer_release(usage);
    zmk_endpoints_send_report(HID_USAGE_CONSUMER);
}
static K_WORK_DEFINE(consumer_work, consumer_work_fn);

static int telemetry_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        uint8_t frame[3] = {0x01, (uint8_t)pos->position, pos->state ? 1 : 0};
        enqueue(frame, sizeof(frame));
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (as_zmk_layer_state_changed(eh)) {
        uint8_t frame[6];
        make_layer_frame(frame);
        enqueue(frame, sizeof(frame));
        atomic_set(&pending_top, frame[1]);
        k_work_submit(&consumer_work);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(linea_telemetry, telemetry_listener);
ZMK_SUBSCRIPTION(linea_telemetry, zmk_position_state_changed);
ZMK_SUBSCRIPTION(linea_telemetry, zmk_layer_state_changed);

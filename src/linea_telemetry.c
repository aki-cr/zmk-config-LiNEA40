/*
 * LiNEA40 Studio 用の状態の知らせ（Bluetooth）
 * レイヤーが切り替わったとき・キーが押された／離されたときに、専用の GATT 特性で通知する。
 * ZMK Studio の通信には「今のレイヤー」や「押されたキー」の知らせが無いため、別の窓口を用意する。
 *
 * 1回の通知 = 1つのデータ:
 *   レイヤー: [0x02, いちばん上のレイヤー番号, 有効なレイヤーのビット列(4バイト, リトルエンディアン)]
 *   キー:     [0x01, キーの番号, 1=押した / 0=離した]
 * 読み出すと、今のレイヤーのデータが返る。
 */
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>

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

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {}

BT_GATT_SERVICE_DEFINE(linea_telemetry_svc, BT_GATT_PRIMARY_SERVICE(&svc_uuid),
                       BT_GATT_CHARACTERISTIC(&chr_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, read_state, NULL, NULL),
                       BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

static void send(const uint8_t *data, uint16_t len) {
    /* 購読していない接続には送られない。送れなくても入力の処理には影響させない */
    bt_gatt_notify(NULL, &linea_telemetry_svc.attrs[2], data, len);
}

static int telemetry_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);
    if (pos) {
        uint8_t frame[3] = {0x01, (uint8_t)pos->position, pos->state ? 1 : 0};
        send(frame, sizeof(frame));
        return ZMK_EV_EVENT_BUBBLE;
    }
    if (as_zmk_layer_state_changed(eh)) {
        uint8_t frame[6];
        make_layer_frame(frame);
        send(frame, sizeof(frame));
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(linea_telemetry, telemetry_listener);
ZMK_SUBSCRIPTION(linea_telemetry, zmk_position_state_changed);
ZMK_SUBSCRIPTION(linea_telemetry, zmk_layer_state_changed);

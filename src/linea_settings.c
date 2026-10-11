/*
 * LiNEA40 Studio から Bluetooth で変えられる設定（ビルドせずにすぐ反映する値）
 *
 * Bluetooth（専用の GATT サービス・特性）: ペアリング済みの機器だけが読み書きできる（暗号化が必要）
 *   書き込み: [0x01, 番号, 値(4バイト, リトルエンディアン)]  値を変える
 *             [0x02, 番号]                                   その値を「設定なし」（ファームの値）に戻す
 *             [0x03]                                         全部「設定なし」に戻す
 *   読み出し: [0x01(形式), 番号の数 N, (番号, 値(4バイト))×N] 設定なしの値は 0x7fffffff
 *
 * 値は変えてから少し待ってまとめて保存する（フラッシュの書き換え回数を減らすため）。
 * 値を使う側（linea_accel など）は linea_setting_get() で毎回読む。
 */
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include "linea_settings.h"

LOG_MODULE_REGISTER(linea_settings, CONFIG_ZMK_LOG_LEVEL);

#define UNSET 0x7fffffff

static int32_t values[LINEA_SET_COUNT] = {[0 ... LINEA_SET_COUNT - 1] = UNSET};

/* 値の範囲（変な値を受け取っても、トラボが止まったり暴れたりしないように） */
static bool in_range(uint8_t id, int32_t v) {
    switch (id) {
    case LINEA_SET_CURSOR_SPEED:
    case LINEA_SET_SCROLL_SPEED:
        return v >= 100 && v <= 8000;
    case LINEA_SET_CURSOR_ACCEL_MIN:
    case LINEA_SET_CURSOR_ACCEL_MAX:
        return v >= 100 && v <= 5000;
    case LINEA_SET_CURSOR_SPEED_LOW:
        return v >= 0 && v <= 100;
    case LINEA_SET_CURSOR_SPEED_HIGH:
        return v >= 1 && v <= 200;
    case LINEA_SET_GESTURE_THRESHOLD:
        return v >= 5 && v <= 500;
    case LINEA_SET_GESTURE_IDLE:
        return v >= 50 && v <= 2000;
    default:
        return false;
    }
}

int32_t linea_setting_get(enum linea_setting_id id, int32_t def) {
    if (id <= 0 || id >= LINEA_SET_COUNT) {
        return def;
    }
    int32_t v = values[id];
    return v == UNSET ? def : v;
}

/* ---- 保存（Zephyr の settings。ペアリングの記録と同じ場所） ---- */
static void save_work_handler(struct k_work *work) {
    int err = settings_save_one("linea/values", values, sizeof(values));
    if (err) {
        LOG_ERR("設定を保存できません (%d)", err);
    }
}
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_handler);

static int settings_set_cb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (!settings_name_steq(name, "values", NULL)) {
        return -ENOENT;
    }
    int32_t buf[LINEA_SET_COUNT];
    for (int i = 0; i < LINEA_SET_COUNT; i++) {
        buf[i] = UNSET;
    }
    /* 番号が増えた版と古い版で長さが違っても、読める分だけ読む */
    ssize_t n = read_cb(cb_arg, buf, MIN(len, sizeof(buf)));
    if (n < 0) {
        return (int)n;
    }
    for (int i = 1; i < LINEA_SET_COUNT; i++) {
        values[i] = (buf[i] == UNSET || in_range(i, buf[i])) ? buf[i] : UNSET;
    }
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(linea_settings, "linea", NULL, settings_set_cb, NULL, NULL);

static void changed(void) { k_work_reschedule(&save_work, K_SECONDS(2)); }

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

/* 4a4e980b-132a-4338-9860-d6d20d9cb208（知らせのサービスの続き番号） */
static struct bt_uuid_128 set_svc_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x4a4e980b, 0x132a, 0x4338, 0x9860, 0xd6d20d9cb208));
/* 999f06e1-c35d-41fd-90c8-0000a2705140 */
static struct bt_uuid_128 set_chr_uuid =
    BT_UUID_INIT_128(BT_UUID_128_ENCODE(0x999f06e1, 0xc35d, 0x41fd, 0x90c8, 0xa2705140));

static ssize_t read_values(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf, uint16_t len,
                           uint16_t offset) {
    uint8_t out[2 + 5 * (LINEA_SET_COUNT - 1)];
    out[0] = 0x01;
    out[1] = LINEA_SET_COUNT - 1;
    for (int i = 1; i < LINEA_SET_COUNT; i++) {
        out[2 + 5 * (i - 1)] = (uint8_t)i;
        sys_put_le32((uint32_t)values[i], &out[3 + 5 * (i - 1)]);
    }
    return bt_gatt_attr_read(conn, attr, buf, len, offset, out, sizeof(out));
}

static ssize_t write_values(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                            uint16_t len, uint16_t offset, uint8_t flags) {
    const uint8_t *b = buf;
    if (offset != 0 || len < 1) {
        return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
    }
    switch (b[0]) {
    case 0x01: {
        if (len != 6) {
            return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
        }
        uint8_t id = b[1];
        int32_t v = (int32_t)sys_get_le32(&b[2]);
        if (id == 0 || id >= LINEA_SET_COUNT || !in_range(id, v)) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        values[id] = v;
        break;
    }
    case 0x02:
        if (len != 2 || b[1] == 0 || b[1] >= LINEA_SET_COUNT) {
            return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
        }
        values[b[1]] = UNSET;
        break;
    case 0x03:
        for (int i = 1; i < LINEA_SET_COUNT; i++) {
            values[i] = UNSET;
        }
        break;
    default:
        return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
    }
    changed();
    return len;
}

BT_GATT_SERVICE_DEFINE(linea_settings_svc, BT_GATT_PRIMARY_SERVICE(&set_svc_uuid),
                       BT_GATT_CHARACTERISTIC(&set_chr_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
                                              BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT,
                                              read_values, write_values, NULL));
#endif

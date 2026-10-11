/*
 * LiNEA40 Studio から Bluetooth で変えられる設定（ビルドせずにすぐ反映する値）
 * 値はキーボード本体に保存し、電源を切っても残す。
 */
#pragma once

#include <stdint.h>

/* 設定の番号（LiNEA40 Studio と同じ番号を使う） */
enum linea_setting_id {
    LINEA_SET_CURSOR_SPEED = 1, /* カーソルの速さの倍率（1000 = 1倍。センサーの CPI に掛ける） */
    LINEA_SET_CURSOR_ACCEL_MIN = 2, /* カーソルの加速: ゆっくり動かしたときの倍率（1000 = 1倍） */
    LINEA_SET_CURSOR_ACCEL_MAX = 3, /* 速く動かしたときの倍率 */
    LINEA_SET_CURSOR_SPEED_LOW = 4, /* この移動量以下はゆっくり扱い */
    LINEA_SET_CURSOR_SPEED_HIGH = 5, /* この移動量以上は速い扱い */
    LINEA_SET_SCROLL_SPEED = 6,     /* トラボのスクロールの速さの倍率（1000 = 1倍） */
    LINEA_SET_GESTURE_THRESHOLD = 7, /* Gesture: これだけ転がしたら1回 */
    LINEA_SET_GESTURE_IDLE = 8,     /* Gesture: これだけ止まったら次を受け付ける（ms） */
    LINEA_SET_COUNT,
};

/* 値が設定されていなければ def を返す */
int32_t linea_setting_get(enum linea_setting_id id, int32_t def);

/*
 * LiNEA40 Studio から Bluetooth で変えられる設定（ビルドせずにすぐ反映する値）
 * 値はキーボード本体に保存し、電源を切っても残す。
 */
#pragma once

#include <stdint.h>

/* 設定の番号（LiNEA40 Studio と同じ番号を使う） */
enum linea_setting_id {
    LINEA_SET_CURSOR_SPEED = 1, /* カーソルの速さの倍率（1000 = 1倍。センサーの CPI に掛ける） */
    LINEA_SET_COUNT,
};

/* 値が設定されていなければ def を返す */
int32_t linea_setting_get(enum linea_setting_id id, int32_t def);

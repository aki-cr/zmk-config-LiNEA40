/*
 * LiNEA40 Studio から Bluetooth で変えられる設定（ビルドせずにすぐ反映する値）
 * 値はキーボード本体に保存し、電源を切っても残す。
 */
#pragma once

#include <stdbool.h>
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
    LINEA_SET_HOLD_FLAVOR = 9,      /* 長押しの判定のしかた: 0 ホールド優先 1 バランス 2 タップ優先 3 tap-unless-interrupted */
    LINEA_SET_HOLD_TAPPING_TERM = 10, /* 長押しと判定するまで（ms） */
    LINEA_SET_HOLD_QUICK_TAP = 11,  /* quick-tap（ms。0 = なし） */
    LINEA_SET_PRECISION_DIVISOR = 12, /* 精密モード（Precision レイヤー）でカーソルを 1/N にする */
    LINEA_SET_SCROLL_ACCEL_ON = 13, /* トラボのスクロールの加速: 1 = あり 0 = なし */
    LINEA_SET_SCROLL_ACCEL_MIN = 14, /* スクロールの加速: ゆっくりのときの倍率 */
    LINEA_SET_SCROLL_ACCEL_MAX = 15, /* 速いときの倍率 */
    LINEA_SET_SCROLL_SPEED_LOW = 16, /* この移動量以下はゆっくり扱い */
    LINEA_SET_SCROLL_SPEED_HIGH = 17, /* この移動量以上は速い扱い */
    LINEA_SET_SCROLL_TICK_ON = 18,  /* スクロールを1回 ±1 段ずつ送る: 1 = あり 0 = なし */
    LINEA_SET_SCROLL_MAX_CARRY = 19, /* 1段ずつ送るときに持ち越す段数 */
    LINEA_SET_AML_TIMEOUT = 20,     /* オートマウス: これだけ動かさないと抜ける（ms） */
    LINEA_SET_AML_PRIOR_IDLE = 21,  /* オートマウス: 直前のキー入力からこの時間以内の動きでは入らない（ms） */
    LINEA_SET_AML_EXCL_LO = 22,     /* オートマウス: 押しても抜けないキー（0〜31番のビット） */
    LINEA_SET_AML_EXCL_HI = 23,     /* 同じく 32〜63番のビット */
    LINEA_SET_SCROLL_PACE = 24,     /* スクロールのすき間を埋める（貯めた段を平均の間隔で送り出す）: 1 = あり 0 = なし */
    LINEA_SET_SCROLL_PACE_BUFFER = 25, /* すき間を埋めるときに貯める段の上限 */
    LINEA_SET_SCROLL_HORIZONTAL = 26, /* トラボの横スクロール: 1 = あり 0 = なし（縦だけ） */
    LINEA_SET_DEX_PROFILE = 27,    /* DeX モード（linea_dex.c）にする Bluetooth プロファイル: 1〜5（0 = 使わない） */
    LINEA_SET_DEX_CURSOR = 28,     /* DeX モードのカーソルの速さの倍率（1000 = Mac と同じ） */
    LINEA_SET_DEX_SCROLL_INVERT = 29, /* DeX モードでスクロールの向きを逆にする: 1 = 逆 0 = Mac と同じ */
    LINEA_SET_DEX_SCROLL_SPEED = 30, /* DeX モードのスクロールの量の倍率（1000 = Mac と同じ） */
    LINEA_SET_COUNT,
};

/* 値が設定されていなければ def を返す */
int32_t linea_setting_get(enum linea_setting_id id, int32_t def);

/* DeX モード（決めたプロファイルにつないでいる）か。linea_dex.c が無い左手側では常に false */
bool linea_dex_active(void);

/* DeX モードの既定値（LiNEA40 Studio で変えていないとき） */
#define LINEA_DEX_CURSOR_DEFAULT 600
#define LINEA_DEX_SCROLL_INVERT_DEFAULT 1
#define LINEA_DEX_SCROLL_SPEED_DEFAULT 1000

# LiNEA40 用の追加部品。どちらもトラボとキーマップを持つ右手側（セントラル）でだけ使う
if(CONFIG_ZMK_POINTING)
  zephyr_library()
  zephyr_library_sources(src/linea_accel.c src/linea_gesture.c src/linea_tick.c src/linea_settings.c src/linea_dex_scroll.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

# レイヤーとキーの状態を LiNEA40 Studio に知らせる（Bluetooth）
if(CONFIG_ZMK_BLE AND (CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR NOT CONFIG_ZMK_SPLIT))
  zephyr_library_named(linea_telemetry)
  zephyr_library_sources(src/linea_telemetry.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

# 長押しの判定（hold-tap）とオートマウスの LiNEA40 用の部品。キーマップを持つ右手側（セントラル）でだけ使う。
# ZMK はキーの知らせを「部品を並べた順」に配るので、ZMK と同じく keymap・combo より前に受け取れるよう、
# 別の部品の束（zephyr_library）ではなく ZMK 本体（app）に、ZMK の部品より先に入れる。
# 並びは ZMK と同じ: オートマウス（zip_temp_layer の位置）→ 長押しの判定 →（ZMK の combo・keymap）
# （別の束に入れたら keymap より後になり、長押しがほかのキーで決まらず、判定時間を待つようになった）
if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  # コンボも ZMK と同じく hold-tap の後・keymap の前（ZMK の combo.c はキーマップに zmk,combos が無いので空になる）
  target_sources(app PRIVATE src/linea_aml.c src/linea_hold_tap.c src/linea_combo.c)
  # DeX モード（プロファイル 3 のときだけ Mac のキー操作を Android 用に置き換える）は、キーの知らせを hid_listener より前に書き換える。
  # 設定（linea_settings.c）がトラボのある右手側にしか無いので、ZMK_POINTING のときだけ
  if(CONFIG_ZMK_POINTING)
    target_sources(app PRIVATE src/linea_dex.c)
  endif()
endif()

# 充電中・充電完了・残りが少ないを LED で知らせる（左右どちらも、自分の電池を）
if(CONFIG_RGBLED_WIDGET AND CONFIG_ZMK_BATTERY_REPORTING)
  zephyr_library_named(linea_charge)
  zephyr_library_sources(src/linea_charge.c)
endif()

# LiNEA40 用の追加部品。どちらもトラボとキーマップを持つ右手側（セントラル）でだけ使う
if(CONFIG_ZMK_POINTING)
  zephyr_library()
  zephyr_library_sources(src/linea_accel.c src/linea_gesture.c src/linea_tick.c src/linea_settings.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

# レイヤーとキーの状態を LiNEA40 Studio に知らせる（Bluetooth）
if(CONFIG_ZMK_BLE AND (CONFIG_ZMK_SPLIT_ROLE_CENTRAL OR NOT CONFIG_ZMK_SPLIT))
  zephyr_library_named(linea_telemetry)
  zephyr_library_sources(src/linea_telemetry.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

# 長押しの判定（hold-tap）とオートマウスの LiNEA40 用の部品。キーマップを持つ右手側（セントラル）でだけ使う
if((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
  zephyr_library_named(linea_central)
  zephyr_library_sources(src/linea_hold_tap.c src/linea_aml.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

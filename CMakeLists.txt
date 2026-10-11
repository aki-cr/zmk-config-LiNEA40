# LiNEA40 用の追加部品（トラボの加速）。トラボを持つ右手側でだけ使う
if(CONFIG_ZMK_POINTING)
  zephyr_library()
  zephyr_library_sources(src/linea_accel.c)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)
endif()

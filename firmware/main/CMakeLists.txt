# RecBean 应用组件。rb_app/rb_adpcm/rb_ring/rb_text/rb_transcript/rb_proto/rb_setup
# 是纯逻辑模块（无 ESP-IDF/LVGL 依赖），同时由 tools/validate.sh 编译为主机测试。
idf_component_register(
    SRCS "main.c"
         "rb_adpcm.c"
         "rb_app.c"
         "rb_proto.c"
         "rb_ring.c"
         "rb_setup.c"
         "rb_text.c"
         "rb_transcript.c"
         "rb_audio.c"
         "rb_fonts.c"
         "rb_net.c"
         "rb_portal.c"
         "rb_session.c"
         "rb_store.c"
         "rb_ui.c"
    INCLUDE_DIRS "."
    REQUIRES bsp esp_event esp_http_client esp_http_server esp_netif esp_timer esp_wifi
             esp_hw_support lwip nvs_flash
)

# 断网缓存帧数（每帧 200 ms / 1608 B）。设计目标 50 帧 ≈ 80 KB ≈ 10 s；静态 RAM 预算
# 核算后先取 40 帧 ≈ 64 KB ≈ 8 s（见 UPSTREAM.md “内存预算”）。真机测得堆余量充足时可调回 50。
target_compile_definitions(${COMPONENT_LIB} PRIVATE RB_RING_FRAMES=40)

# GB2312 16 px 中文字库放在 assets/fonts（素材目录规范），显式加入本组件编译。
target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/recbean_font_16.c")

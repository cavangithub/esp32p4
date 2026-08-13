set(LV_DEMO_DIR ../managed_components/lvgl__lvgl/demos)
file(GLOB_RECURSE LV_DEMOS_SOURCES ${LV_DEMO_DIR}/*.c)

idf_component_register(
    # ${LV_DEMOS_SOURCES}
    SRCS main.c station_example.c time_service.c ui_clock.c ui_image.c msc_example.c

    # ${LV_DEMO_DIR}
    INCLUDE_DIRS . )

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)

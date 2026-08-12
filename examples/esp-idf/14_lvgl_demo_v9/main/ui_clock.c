#include "ui_clock.h"

#include <stdio.h>
#include <time.h>

#include "time_service.h"


static lv_obj_t *clock_label;


static void clock_timer_cb(lv_timer_t *timer)
{
    struct tm timeinfo;

    char buffer[64];


    time_service_get(
        &timeinfo
    );


    snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d\n%02d:%02d:%02d",
        timeinfo.tm_year + 1900,
        timeinfo.tm_mon + 1,
        timeinfo.tm_mday,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
    );


    if(clock_label)
    {
        lv_label_set_text(
            clock_label,
            buffer
        );
    }
}

void ui_clock_create(lv_obj_t *parent)
{
    clock_label =
    lv_label_create(parent);


    lv_label_set_text(
        clock_label,
        "---- -- --\n--:--:--"
    );
    lv_obj_set_style_text_font(
        clock_label,
        &lv_font_montserrat_26,
        0
    );


    lv_obj_center(
        clock_label
    );

    lv_timer_create(
        clock_timer_cb,
        1000,
        NULL
    );
}
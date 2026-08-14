#include "album_ctrl.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "lvgl.h"

#include "msc_example.h"
#include "ui_image.h"
#include "time_service.h"

static const char *TAG = "album_ctrl";

#define ALBUM_SLIDE_MS 5000
#define ALBUM_CLOCK_MS 1000

static size_t s_index;
static bool s_playing;
static lv_timer_t *s_slide_timer;
static lv_timer_t *s_clock_timer;

static void album_show_current(void);
static void slide_timer_cb(lv_timer_t *timer);
static void clock_timer_cb(lv_timer_t *timer);

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    struct tm timeinfo;
    time_service_get(&timeinfo);

    char buf[16];
    if (timeinfo.tm_year > 70) {
        snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    } else {
        snprintf(buf, sizeof(buf), "--:--");
    }
    photo_set_status(NULL, buf);
}

static void album_stop_slideshow(void)
{
    s_playing = false;
    if (s_slide_timer) {
        lv_timer_pause(s_slide_timer);
    }
}

static void album_start_slideshow(void)
{
    if (jpg_list_count() == 0) {
        album_stop_slideshow();
        return;
    }
    s_playing = true;
    if (s_slide_timer) {
        lv_timer_pause(s_slide_timer);
    }
}

static void album_show_current(void)
{
    size_t count = jpg_list_count();
    if (count == 0 || s_index >= count) {
        photo_show(NULL);
        photo_set_status(jpg_list_ready() ? "No JPEG found" : "Insert USB", NULL);
        photo_set_play_label(false);
        return;
    }

    /* Wait until the new photo is on screen before starting the 5s timer. */
    if (s_slide_timer) {
        lv_timer_pause(s_slide_timer);
    }

    const char *vfs_path = jpg_list_get(s_index);
    if (!vfs_path) {
        return;
    }

    ESP_LOGI(TAG, "Show [%u/%u] %s",
             (unsigned)(s_index + 1), (unsigned)count, vfs_path);

    photo_show(vfs_path);

    char status[48];
    snprintf(status, sizeof(status), "USB %u/%u",
             (unsigned)(s_index + 1), (unsigned)count);
    photo_set_status(status, NULL);
    photo_set_play_label(s_playing);
}

static void slide_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_playing || jpg_list_count() == 0) {
        return;
    }
    album_ctrl_next();
}

void album_ctrl_init(void)
{
    s_index = 0;
    s_playing = false;

    s_slide_timer = lv_timer_create(slide_timer_cb, ALBUM_SLIDE_MS, NULL);
    lv_timer_pause(s_slide_timer);

    s_clock_timer = lv_timer_create(clock_timer_cb, ALBUM_CLOCK_MS, NULL);

    photo_set_status("Insert USB", "--:--");
    photo_set_play_label(false);
}

void album_ctrl_on_usb_ready(void)
{
    s_index = 0;
    album_start_slideshow();
    album_show_current();
}

void album_ctrl_on_usb_removed(void)
{
    s_index = 0;
    album_stop_slideshow();
    album_show_current();
}

void album_ctrl_on_photo_shown(void)
{
    if (!s_playing || !s_slide_timer || jpg_list_count() == 0) {
        return;
    }
    lv_timer_reset(s_slide_timer);
    lv_timer_resume(s_slide_timer);
}

void album_ctrl_prev(void)
{
    size_t count = jpg_list_count();
    if (count == 0) {
        return;
    }
    s_index = (s_index == 0) ? (count - 1) : (s_index - 1);
    album_show_current();
}

void album_ctrl_next(void)
{
    size_t count = jpg_list_count();
    if (count == 0) {
        return;
    }
    s_index = (s_index + 1) % count;
    album_show_current();
}

void album_ctrl_play_toggle(void)
{
    if (jpg_list_count() == 0) {
        s_playing = false;
        photo_set_play_label(false);
        return;
    }

    s_playing = !s_playing;
    if (s_playing) {
        if (s_slide_timer) {
            lv_timer_resume(s_slide_timer);
            lv_timer_reset(s_slide_timer);
        }
    } else if (s_slide_timer) {
        lv_timer_pause(s_slide_timer);
    }
    photo_set_play_label(s_playing);
}

bool album_ctrl_is_playing(void)
{
    return s_playing;
}

#include "ui_image.h"

#include "album_ctrl.h"
#include "bsp/esp-bsp.h"
#include "jpeg_photo.h"

typedef struct {
    lv_obj_t *screen;

    lv_obj_t *status_bar;
    lv_obj_t *label_usb;
    lv_obj_t *label_time;

    lv_obj_t *img;

    lv_obj_t *ctrl_panel;
    lv_obj_t *btn_prev;
    lv_obj_t *btn_play;
    lv_obj_t *btn_next;
    lv_obj_t *label_play;
} photo_ui_t;

static photo_ui_t g_photo_ui;

static void jpeg_ready_cb(const lv_image_dsc_t *dsc)
{
    bsp_display_lock(-1);
    if (g_photo_ui.img) {
        lv_image_set_src(g_photo_ui.img, dsc);
    }
    if (dsc) {
        album_ctrl_on_photo_shown();
    }
    bsp_display_unlock();
}

static void btn_prev_cb(lv_event_t *e)
{
    (void)e;
    album_ctrl_prev();
}

static void btn_next_cb(lv_event_t *e)
{
    (void)e;
    album_ctrl_next();
}

static void btn_play_cb(lv_event_t *e)
{
    (void)e;
    album_ctrl_play_toggle();
}

void photo_show(const char *path)
{
    if (!g_photo_ui.img) {
        return;
    }
    if (path == NULL || path[0] == '\0') {
        jpeg_photo_request(NULL, 0, 0);
        return;
    }

    int max_w = lv_display_get_horizontal_resolution(NULL);
    int max_h = lv_display_get_vertical_resolution(NULL);
    jpeg_photo_request(path, max_w, max_h);
}

void photo_set_status(const char *usb_text, const char *time_text)
{
    if (usb_text && g_photo_ui.label_usb) {
        lv_label_set_text(g_photo_ui.label_usb, usb_text);
    }
    if (time_text && g_photo_ui.label_time) {
        lv_label_set_text(g_photo_ui.label_time, time_text);
    }
}

void photo_set_play_label(bool playing)
{
    if (!g_photo_ui.label_play) {
        return;
    }
    lv_label_set_text(g_photo_ui.label_play, playing ? "Pause" : "Play");
}

lv_obj_t *photo_page_create(void)
{
    photo_ui_t *ui = &g_photo_ui;

    ui->screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(ui->screen);
    lv_obj_set_size(ui->screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(ui->screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->screen, LV_OPA_COVER, 0);

    ui->img = lv_image_create(ui->screen);
    lv_obj_set_size(ui->img, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(ui->img);
    lv_image_set_inner_align(ui->img, LV_IMAGE_ALIGN_CENTER);

    ui->status_bar = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->status_bar);
    lv_obj_set_size(ui->status_bar, LV_HOR_RES, 32);
    lv_obj_align(ui->status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(ui->status_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->status_bar, LV_OPA_50, 0);

    ui->label_usb = lv_label_create(ui->status_bar);
    lv_label_set_text(ui->label_usb, "Insert USB");
    lv_obj_set_style_text_color(ui->label_usb, lv_color_white(), 0);
    lv_obj_align(ui->label_usb, LV_ALIGN_LEFT_MID, 10, 0);

    ui->label_time = lv_label_create(ui->status_bar);
    lv_label_set_text(ui->label_time, "--:--");
    lv_obj_set_style_text_color(ui->label_time, lv_color_white(), 0);
    lv_obj_align(ui->label_time, LV_ALIGN_RIGHT_MID, -10, 0);

    ui->ctrl_panel = lv_obj_create(ui->screen);
    lv_obj_remove_style_all(ui->ctrl_panel);
    lv_obj_set_size(ui->ctrl_panel, LV_HOR_RES, 60);
    lv_obj_align(ui->ctrl_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ui->ctrl_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui->ctrl_panel, LV_OPA_50, 0);

    ui->btn_prev = lv_button_create(ui->ctrl_panel);
    lv_obj_set_size(ui->btn_prev, 100, 40);
    lv_obj_align(ui->btn_prev, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_t *label_prev = lv_label_create(ui->btn_prev);
    lv_label_set_text(label_prev, "Prev");
    lv_obj_center(label_prev);
    lv_obj_add_event_cb(ui->btn_prev, btn_prev_cb, LV_EVENT_CLICKED, NULL);

    ui->btn_play = lv_button_create(ui->ctrl_panel);
    lv_obj_set_size(ui->btn_play, 100, 40);
    lv_obj_center(ui->btn_play);
    ui->label_play = lv_label_create(ui->btn_play);
    lv_label_set_text(ui->label_play, "Play");
    lv_obj_center(ui->label_play);
    lv_obj_add_event_cb(ui->btn_play, btn_play_cb, LV_EVENT_CLICKED, NULL);

    ui->btn_next = lv_button_create(ui->ctrl_panel);
    lv_obj_set_size(ui->btn_next, 100, 40);
    lv_obj_align(ui->btn_next, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_t *label_next = lv_label_create(ui->btn_next);
    lv_label_set_text(label_next, "Next");
    lv_obj_center(label_next);
    lv_obj_add_event_cb(ui->btn_next, btn_next_cb, LV_EVENT_CLICKED, NULL);

    jpeg_photo_init(jpeg_ready_cb);

    return ui->screen;
}

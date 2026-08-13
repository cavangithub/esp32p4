#ifndef UI_IMAGE_H
#define UI_IMAGE_H

#include <stdbool.h>
#include "lvgl.h"

lv_obj_t *photo_page_create(void);
void photo_show(const char *path);
void photo_set_status(const char *usb_text, const char *time_text);
void photo_set_play_label(bool playing);

#endif

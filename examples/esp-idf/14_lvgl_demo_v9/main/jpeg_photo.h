#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Called from the decode worker; must take the LVGL lock, set image src, then unlock. dsc is NULL to clear. */
typedef void (*jpeg_photo_ready_cb_t)(const lv_image_dsc_t *dsc);

void jpeg_photo_init(jpeg_photo_ready_cb_t ready_cb);

/** Queue a VFS JPEG path (or NULL to clear). Returns immediately. */
void jpeg_photo_request(const char *vfs_path, int max_w, int max_h);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void album_ctrl_init(void);

/** Call from main under display lock when USB JPG list is ready. */
void album_ctrl_on_usb_ready(void);

/** Call from main under display lock when USB is removed. */
void album_ctrl_on_usb_removed(void);

void album_ctrl_prev(void);
void album_ctrl_next(void);
void album_ctrl_play_toggle(void);
bool album_ctrl_is_playing(void);

#ifdef __cplusplus
}
#endif

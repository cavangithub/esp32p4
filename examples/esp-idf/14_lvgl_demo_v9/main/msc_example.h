#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JPG_LIST_MAX 16

typedef enum {
    MSC_EVENT_CONNECTED,    /*!< USB mounted and JPG list scanned */
    MSC_EVENT_DISCONNECTED, /*!< USB removed and JPG list cleared */
} msc_event_id_t;

typedef struct {
    msc_event_id_t id;
    size_t jpg_count;
} msc_event_t;

/**
 * @brief Start USB host + MSC worker task (non-blocking).
 *
 * Worker detects connect/disconnect, mounts VFS, scans JPGs,
 * then posts @ref msc_event_t to an internal queue for main.
 */
esp_err_t msc_start(void);

/**
 * @brief Wait for USB album events from the MSC worker task.
 *
 * @return true if an event was received, false on timeout
 */
bool msc_wait_event(msc_event_t *event, TickType_t timeout_ticks);

size_t jpg_list_count(void);
const char *jpg_list_get(size_t index);
bool jpg_list_ready(void);

#ifdef __cplusplus
}
#endif

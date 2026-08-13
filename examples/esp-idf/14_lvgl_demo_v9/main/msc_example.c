/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/msc_host_vfs.h"
#include "ffconf.h"
#include "errno.h"
#include "msc_example.h"

static const char *TAG = "msc";

#define MNT_PATH         "/usb"     // Base mount path prefix, devices will be mounted as /usb0, /usb1, /usb2...
#define MAX_MSC_DEVICES  CONFIG_FATFS_VOLUME_COUNT
#define JPG_PATH_MAX     256
#define JPG_SCAN_MAX_DEPTH 8

/**
 * @brief MSC Device Entry
 *
 * This structure holds information about a connected MSC device,
 * including the USB address, MSC device handle, VFS handle, and assigned mount point.
 */
typedef struct {
    uint8_t usb_addr;                     /*!< USB device address */
    msc_host_device_handle_t msc_device;  /*!< Handle of the MSC device */
    msc_host_vfs_handle_t vfs_handle;     /*!< VFS handle assigned to the MSC device */
} msc_dev_entry_t;

static msc_dev_entry_t *msc_devices[MAX_MSC_DEVICES] = {0};

static char s_jpg_paths[JPG_LIST_MAX][JPG_PATH_MAX];
static size_t s_jpg_count;
static bool s_jpg_ready;

size_t jpg_list_count(void)
{
    return s_jpg_count;
}

const char *jpg_list_get(size_t index)
{
    if (index >= s_jpg_count) {
        return NULL;
    }
    return s_jpg_paths[index];
}

bool jpg_list_ready(void)
{
    return s_jpg_ready;
}

/**
 * @brief Internal MSC host queue (connect/disconnect from USB callback)
 *        and notify queue (events delivered to main).
 */
static QueueHandle_t s_host_queue;
static QueueHandle_t s_notify_queue;

typedef struct {
    enum {
        HOST_DEVICE_CONNECTED,
        HOST_DEVICE_DISCONNECTED,
    } id;
    union {
        uint8_t new_dev_address;
        msc_host_device_handle_t device_handle;
    } data;
} host_message_t;

/**
 * @brief Find a free slot in the device table.
 *
 * @return Index of the free slot, or -1 if no free slot is available.
 */
static inline int find_free_slot(void)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Allocates a new MSC device entry and mounts it to VFS.
 *
 * This function finds a free slot for a new MSC device, allocates memory for the device entry,
 * installs the MSC device, and mounts it to the virtual file system (VFS).
 *
 * If any step fails, the function ensures proper cleanup of allocated resources before returning an error.
 *
 * @param[in] msg        Message containing the address of the new USB device.
 * @param[out] out_slot  Pointer to store the allocated slot index on success.
 *
 * @return
 *         - ESP_OK on success.
 *         - ESP_ERR_NOT_FOUND if no free slot is available.
 *         - ESP_ERR_NO_MEM if memory allocation fails.
 *         - Other esp_err_t codes if device installation or VFS registration fails.
 */
static esp_err_t allocate_new_msc_device(const host_message_t *msg, int *out_slot)
{
    int slot = find_free_slot();
    if (slot < 0) {
        ESP_LOGW(TAG, "No free slots for new MSC device (max %d)", MAX_MSC_DEVICES);
        return ESP_ERR_NOT_FOUND;
    }

    msc_devices[slot] = calloc(1, sizeof(msc_dev_entry_t));
    if (!msc_devices[slot]) {
        ESP_LOGE(TAG, "Failed to allocate memory for new MSC device entry");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = msc_host_install_device(msg->data.new_dev_address, &msc_devices[slot]->msc_device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device failed: %s", esp_err_to_name(err));
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        return err;
    }

    msc_devices[slot]->usb_addr = msg->data.new_dev_address;

    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 12,
        .allocation_unit_size = 8192,
    };

    char mount_path[16];
    snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", slot);

    err = msc_host_vfs_register(msc_devices[slot]->msc_device, mount_path, &mount_config, &msc_devices[slot]->vfs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_vfs_register failed: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
        free(msc_devices[slot]);
        msc_devices[slot] = NULL;
        return err;
    }

    *out_slot = slot;
    return ESP_OK;
}

/**
 * @brief Find a slot by MSC device handle.
 *
 * This function searches for the slot corresponding to a given MSC device handle.
 *
 * @param handle MSC device handle to search for.
 * @return Index of the slot if found, otherwise -1.
 */
static int find_slot_by_handle(msc_host_device_handle_t handle)
{
    for (int i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Free resources associated with a specific MSC device by slot index.
 */
static void free_msc_device(int slot)
{
    if (slot < 0 || slot >= MAX_MSC_DEVICES || !msc_devices[slot]) {
        ESP_LOGE(TAG, "Invalid slot index for MSC device deallocation");
        return;
    }

    if (msc_devices[slot]->vfs_handle) {
        ESP_ERROR_CHECK(msc_host_vfs_unregister(msc_devices[slot]->vfs_handle));
    }
    if (msc_devices[slot]->msc_device) {
        ESP_ERROR_CHECK(msc_host_uninstall_device(msc_devices[slot]->msc_device));
    }

    free(msc_devices[slot]);
    msc_devices[slot] = NULL;
}

static void notify_main(msc_event_id_t id)
{
    if (!s_notify_queue) {
        return;
    }
    msc_event_t evt = {
        .id = id,
        .jpg_count = s_jpg_count,
    };
    if (xQueueSend(s_notify_queue, &evt, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "Failed to notify main (event=%d)", (int)id);
    }
}

static void jpg_list_clear(void)
{
    s_jpg_count = 0;
    s_jpg_ready = false;
    memset(s_jpg_paths, 0, sizeof(s_jpg_paths));
}

static bool jpg_list_append(const char *path)
{
    if (s_jpg_count >= JPG_LIST_MAX) {
        return false;
    }
    strlcpy(s_jpg_paths[s_jpg_count], path, JPG_PATH_MAX);
    s_jpg_count++;
    return true;
}

static bool is_jpg_name(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }
    return strcasecmp(name + len - 4, ".jpg") == 0;
}

static void scan_jpg_dir(const char *dir, int depth)
{
    if (s_jpg_count >= JPG_LIST_MAX || depth > JPG_SCAN_MAX_DEPTH) {
        return;
    }

    DIR *dh = opendir(dir);
    if (!dh) {
        ESP_LOGW(TAG, "Failed to open directory: %s", dir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dh)) != NULL && s_jpg_count < JPG_LIST_MAX) {
        if (ent->d_name[0] == '.') {
            continue;
        }

        char full[JPG_PATH_MAX];
        int n = snprintf(full, sizeof(full), "%s/%s", dir, ent->d_name);
        if (n < 0 || n >= (int)sizeof(full)) {
            continue;
        }

        bool is_dir = false;
        if (ent->d_type == DT_DIR) {
            is_dir = true;
        } else if (ent->d_type == DT_REG) {
            is_dir = false;
        } else {
            struct stat st;
            if (stat(full, &st) != 0) {
                continue;
            }
            is_dir = S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            scan_jpg_dir(full, depth + 1);
        } else if (is_jpg_name(ent->d_name)) {
            if (jpg_list_append(full)) {
                ESP_LOGI(TAG, "JPG[%u]: %s", (unsigned)s_jpg_count, full);
            }
        }
    }
    closedir(dh);
}

static void jpg_list_scan_mount(int slot)
{
    char mount_path[16];
    snprintf(mount_path, sizeof(mount_path), MNT_PATH "%d", slot);

    jpg_list_clear();
    ESP_LOGI(TAG, "Scanning %s for .jpg files (max %d)", mount_path, JPG_LIST_MAX);
    scan_jpg_dir(mount_path, 0);
    ESP_LOGI(TAG, "JPG list count: %u", (unsigned)s_jpg_count);

    s_jpg_ready = true;
    notify_main(MSC_EVENT_CONNECTED);
}

/**
 * @brief Find a USB addr by MSC device handle.
 */
static inline int8_t find_usb_addr_by_handle(msc_host_device_handle_t handle)
{
    for (int8_t i = 0; i < MAX_MSC_DEVICES; i++) {
        if (msc_devices[i] && msc_devices[i]->msc_device == handle) {
            return msc_devices[i]->usb_addr;
        }
    }
    return -1;
}

/**
 * @brief MSC driver callback — post to host queue (handled by msc_task).
 */
static void msc_event_cb(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (!s_host_queue) {
        return;
    }

    if (event->event == MSC_DEVICE_CONNECTED) {
        ESP_LOGI(TAG, "MSC device connected (usb_addr=%d)", event->device.address);
        host_message_t message = {
            .id = HOST_DEVICE_CONNECTED,
            .data.new_dev_address = event->device.address,
        };
        xQueueSend(s_host_queue, &message, portMAX_DELAY);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        int usb_addr = find_usb_addr_by_handle(event->device.handle);
        if (usb_addr >= 0) {
            ESP_LOGI(TAG, "MSC device disconnected (usb_addr=%d)", usb_addr);
        } else {
            ESP_LOGW(TAG, "MSC device disconnected, but failed to retrieve USB address");
        }
        host_message_t message = {
            .id = HOST_DEVICE_DISCONNECTED,
            .data.device_handle = event->device.handle,
        };
        xQueueSend(s_host_queue, &message, portMAX_DELAY);
    } else {
        ESP_LOGW(TAG, "Unsupported MSC event: %d (possibly suspend/resume)", event->event);
    }
}

static void print_device_info(msc_host_device_info_t *info)
{
    const size_t megabyte = 1024 * 1024;
    uint64_t capacity = ((uint64_t)info->sector_size * info->sector_count) / megabyte;

    printf("Device info:\n");
    printf("\t Capacity: %llu MB\n", capacity);
    printf("\t Sector size: %"PRIu32"\n", info->sector_size);
    printf("\t Sector count: %"PRIu32"\n", info->sector_count);
    printf("\t PID: 0x%04X \n", info->idProduct);
    printf("\t VID: 0x%04X \n", info->idVendor);
#ifndef CONFIG_NEWLIB_NANO_FORMAT
    wprintf(L"\t iProduct: %S \n", info->iProduct);
    wprintf(L"\t iManufacturer: %S \n", info->iManufacturer);
    wprintf(L"\t iSerialNumber: %S \n", info->iSerialNumber);
#endif
}

/**
 * @brief USB host library task — keep handling host events.
 */
static void usb_task(void *args)
{
    (void)args;
    const usb_host_config_t host_config = { .intr_flags = ESP_INTR_FLAG_LEVEL1 };
    ESP_ERROR_CHECK(usb_host_install(&host_config));

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .callback = msc_event_cb,
    };
    ESP_ERROR_CHECK(msc_host_install(&msc_config));

    while (true) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGW(TAG, "USB host: no clients");
        }
    }
}

/**
 * @brief MSC worker — mount/scan on connect, clear on disconnect, notify main.
 */
static void msc_task(void *args)
{
    (void)args;
    ESP_LOGI(TAG, "MSC task waiting for USB flash drive");

    while (true) {
        host_message_t msg;
        if (xQueueReceive(s_host_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (msg.id == HOST_DEVICE_CONNECTED) {
            int slot;
            esp_err_t res = allocate_new_msc_device(&msg, &slot);
            if (res != ESP_OK) {
                continue;
            }

            msc_host_device_info_t info;
            ESP_ERROR_CHECK(msc_host_get_device_info(msc_devices[slot]->msc_device, &info));
            msc_host_print_descriptors(msc_devices[slot]->msc_device);
            print_device_info(&info);

            jpg_list_scan_mount(slot);
            ESP_LOGI(TAG, "Scan finished, notified main (count=%u)", (unsigned)s_jpg_count);
        } else if (msg.id == HOST_DEVICE_DISCONNECTED) {
            int slot = find_slot_by_handle(msg.data.device_handle);
            if (slot >= 0) {
                jpg_list_clear();
                notify_main(MSC_EVENT_DISCONNECTED);
                /* Give main a moment to clear lv_image src before unmount */
                vTaskDelay(pdMS_TO_TICKS(50));
                free_msc_device(slot);
                ESP_LOGI(TAG, "USB removed, notified main");
            }
        }
    }
}

esp_err_t msc_start(void)
{
    if (s_host_queue || s_notify_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    s_host_queue = xQueueCreate(5, sizeof(host_message_t));
    s_notify_queue = xQueueCreate(5, sizeof(msc_event_t));
    if (!s_host_queue || !s_notify_queue) {
        if (s_host_queue) {
            vQueueDelete(s_host_queue);
            s_host_queue = NULL;
        }
        if (s_notify_queue) {
            vQueueDelete(s_notify_queue);
            s_notify_queue = NULL;
        }
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(usb_task, "usb_task", 4096, NULL, 2, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ok = xTaskCreate(msc_task, "msc_task", 8192, NULL, 3, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB MSC tasks started");
    return ESP_OK;
}

bool msc_wait_event(msc_event_t *event, TickType_t timeout_ticks)
{
    if (!event || !s_notify_queue) {
        return false;
    }
    return xQueueReceive(s_notify_queue, event, timeout_ticks) == pdTRUE;
}

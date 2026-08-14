#include "jpeg_photo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "jpeg_photo";

#define JPEG_PATH_MAX 256
#define JPEG_TASK_STACK 24576
#define JPEG_TASK_PRIO  3

typedef struct {
    char path[JPEG_PATH_MAX];
    int max_w;
    int max_h;
    uint32_t seq;
} jpeg_job_t;

static QueueHandle_t s_queue;
static jpeg_photo_ready_cb_t s_ready_cb;
static volatile uint32_t s_req_seq;

static uint8_t *s_pixels;
static lv_image_dsc_t s_dsc;

static int align8_up(int v)
{
    if (v <= 0) {
        return 8;
    }
    return (v + 7) & ~7;
}

static int align16_up(int v)
{
    if (v <= 0) {
        return 16;
    }
    return (v + 15) & ~15;
}

static int align16_down(int v)
{
    v &= ~15;
    return (v < 16) ? 16 : v;
}

static void publish(const lv_image_dsc_t *dsc)
{
    if (s_ready_cb) {
        s_ready_cb(dsc);
    }
}

static void release_shown(void)
{
    publish(NULL);
    if (s_pixels) {
        jpeg_free_align(s_pixels);
        s_pixels = NULL;
    }
    memset(&s_dsc, 0, sizeof(s_dsc));
}

static uint8_t *read_entire_file(const char *path, int *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "open failed: %s", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz <= 2) {
        fclose(f);
        ESP_LOGE(TAG, "empty or invalid file: %s", path);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc((size_t)sz);
    }
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "no mem for %ld byte JPEG", sz);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        ESP_LOGE(TAG, "short read %u / %ld", (unsigned)n, sz);
        free(buf);
        return NULL;
    }
    *out_len = (int)sz;
    return buf;
}

static uint8_t *decode_file(const char *vfs_path, int max_w, int max_h, lv_image_dsc_t *out_dsc)
{
    int jpg_len = 0;
    uint8_t *jpg = read_entire_file(vfs_path, &jpg_len);
    if (!jpg) {
        return NULL;
    }

    jpeg_dec_handle_t dec = NULL;
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    jpeg_error_t ret = jpeg_dec_open(&cfg, &dec);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open failed (%d)", (int)ret);
        free(jpg);
        return NULL;
    }

    jpeg_dec_io_t io = {
        .inbuf = jpg,
        .inbuf_len = jpg_len,
    };
    jpeg_dec_header_info_t info = {0};
    ret = jpeg_dec_parse_header(dec, &io, &info);
    jpeg_dec_close(dec);
    dec = NULL;
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "parse header failed (%d), file %d bytes", (int)ret, jpg_len);
        free(jpg);
        return NULL;
    }

    int src_w = info.width;
    int src_h = info.height;
    if (max_w <= 0) {
        max_w = 960;
    }
    if (max_h <= 0) {
        max_h = 544;
    }

    /* Scale size must be multiple of 8; prefer 16 to match JPEG MCU width. */
    int min_w = align16_up(align8_up(src_w / 8));
    int min_h = align16_up(align8_up(src_h / 8));
    int dw = src_w;
    int dh = src_h;
    if (src_w > max_w || src_h > max_h) {
        float fit_w = (float)max_w / (float)src_w;
        float fit_h = (float)max_h / (float)src_h;
        float fit = (fit_w < fit_h) ? fit_w : fit_h;
        if (fit < 0.125f) {
            fit = 0.125f;
        }
        dw = align16_down((int)(src_w * fit));
        dh = align16_down((int)(src_h * fit));
        if (dw < min_w) {
            dw = min_w;
        }
        if (dh < min_h) {
            dh = min_h;
        }
    }

    cfg.scale.width = 0;
    cfg.scale.height = 0;
    if (dw != src_w || dh != src_h) {
        cfg.scale.width = dw;
        cfg.scale.height = dh;
    }

    ret = jpeg_dec_open(&cfg, &dec);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_open(scale) failed (%d)", (int)ret);
        free(jpg);
        return NULL;
    }

    io.inbuf = jpg;
    io.inbuf_len = jpg_len;
    io.inbuf_remain = 0;
    io.outbuf = NULL;
    io.out_size = 0;
    memset(&info, 0, sizeof(info));
    ret = jpeg_dec_parse_header(dec, &io, &info);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "parse header (scaled) failed (%d)", (int)ret);
        jpeg_dec_close(dec);
        free(jpg);
        return NULL;
    }

    int out_len = 0;
    ret = jpeg_dec_get_outbuf_len(dec, &out_len);
    if (ret != JPEG_ERR_OK || out_len <= 0) {
        ESP_LOGE(TAG, "get_outbuf_len failed (%d)", (int)ret);
        jpeg_dec_close(dec);
        free(jpg);
        return NULL;
    }

    uint8_t *pixels = jpeg_calloc_align((size_t)out_len, 16);
    if (!pixels) {
        ESP_LOGE(TAG, "no mem for %d byte RGB565 (%ux%u)", out_len, info.width, info.height);
        jpeg_dec_close(dec);
        free(jpg);
        return NULL;
    }

    io.outbuf = pixels;
    io.out_size = out_len;
    ret = jpeg_dec_process(dec, &io);
    jpeg_dec_close(dec);
    free(jpg);
    if (ret != JPEG_ERR_OK) {
        ESP_LOGE(TAG, "jpeg_dec_process failed (%d)", (int)ret);
        jpeg_free_align(pixels);
        return NULL;
    }

    int dec_w = info.width;
    int dec_h = info.height;
    int src_stride = (dec_h > 0) ? (out_len / dec_h) : (dec_w * 2);
    if (src_stride < dec_w * 2) {
        src_stride = dec_w * 2;
    }
    /* Decoder may pad rows to MCU width; use the real pitch. */
    if (io.out_size > 0 && dec_h > 0 && (io.out_size / dec_h) >= dec_w * 2) {
        src_stride = io.out_size / dec_h;
    }

    int crop_w = dec_w;
    int crop_h = dec_h;
    if (crop_w > max_w) {
        crop_w = max_w;
    }
    if (crop_h > max_h) {
        crop_h = max_h;
    }
    int src_x = (dec_w - crop_w) / 2;
    int src_y = (dec_h - crop_h) / 2;
    int dst_stride = crop_w * 2;

    uint8_t *packed = pixels;
    if (src_stride != dst_stride || crop_w != dec_w || crop_h != dec_h) {
        packed = jpeg_calloc_align((size_t)dst_stride * (size_t)crop_h, 16);
        if (!packed) {
            ESP_LOGE(TAG, "no mem to pack %dx%d RGB565", crop_w, crop_h);
            jpeg_free_align(pixels);
            return NULL;
        }
        for (int y = 0; y < crop_h; y++) {
            memcpy(packed + (size_t)y * dst_stride,
                   pixels + (size_t)(src_y + y) * src_stride + (size_t)src_x * 2,
                   (size_t)dst_stride);
        }
        jpeg_free_align(pixels);
    }

    memset(out_dsc, 0, sizeof(*out_dsc));
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    out_dsc->header.w = crop_w;
    out_dsc->header.h = crop_h;
    out_dsc->header.stride = (uint32_t)dst_stride;
    out_dsc->data_size = (uint32_t)dst_stride * (uint32_t)crop_h;
    out_dsc->data = packed;

    ESP_LOGI(TAG, "decoded %dx%d -> %dx%d (buf %dx%d stride %d) RGB565 (%s)",
             src_w, src_h, crop_w, crop_h, dec_w, dec_h, src_stride, vfs_path);
    return packed;
}

static void jpeg_task(void *arg)
{
    (void)arg;
    jpeg_job_t job;

    while (true) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (job.path[0] == '\0') {
            release_shown();
            continue;
        }

        lv_image_dsc_t dsc;
        uint8_t *pixels = decode_file(job.path, job.max_w, job.max_h, &dsc);

        if (job.seq != s_req_seq) {
            if (pixels) {
                jpeg_free_align(pixels);
            }
            continue;
        }

        if (!pixels) {
            continue;
        }

        publish(NULL);
        uint8_t *old = s_pixels;
        s_pixels = pixels;
        s_dsc = dsc;
        if (old) {
            jpeg_free_align(old);
        }
        publish(&s_dsc);
    }
}

void jpeg_photo_init(jpeg_photo_ready_cb_t ready_cb)
{
    s_ready_cb = ready_cb;
    if (s_queue) {
        return;
    }

#if CONFIG_ESP_TASK_WDT_EN
    /* Decode is pinned to CPU1 and can run for many seconds. Watch IDLE0 only
     * so a busy JPEG job does not trip the TWDT; UI hangs on CPU0 are still caught. */
    esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
        .idle_core_mask = (1 << 0),
        .trigger_panic = false,
    };
    esp_err_t wdt_err = esp_task_wdt_reconfigure(&twdt_cfg);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "task WDT reconfigure failed: %s", esp_err_to_name(wdt_err));
    }
#endif

    s_queue = xQueueCreate(1, sizeof(jpeg_job_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(jpeg_task, "jpeg_photo", JPEG_TASK_STACK,
                                            NULL, JPEG_TASK_PRIO, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}

void jpeg_photo_request(const char *vfs_path, int max_w, int max_h)
{
    if (!s_queue) {
        return;
    }

    jpeg_job_t job = {
        .max_w = max_w,
        .max_h = max_h,
        .seq = ++s_req_seq,
    };
    if (vfs_path && vfs_path[0]) {
        strlcpy(job.path, vfs_path, sizeof(job.path));
    } else {
        job.path[0] = '\0';
    }
    (void)xQueueOverwrite(s_queue, &job);
}

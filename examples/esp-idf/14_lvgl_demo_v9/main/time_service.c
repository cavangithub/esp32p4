#include "time_service.h"

#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "time_service";


void time_service_init(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    esp_sntp_setoperatingmode(
        SNTP_OPMODE_POLL
    );

    esp_sntp_setservername(
        0,
        "pool.ntp.org"
    );

    esp_sntp_init();

    ESP_LOGI(
        TAG,
        "SNTP initialized"
    );
}


bool time_service_wait_sync(int timeout_ms)
{
    int elapsed = 0;

    while(elapsed < timeout_ms)
    {
        time_t now;

        struct tm timeinfo;


        time(&now);

        localtime_r(
            &now,
            &timeinfo
        );


        if(timeinfo.tm_year >= (2020 - 1900))
        {
            ESP_LOGI(
                TAG,
                "Time synchronized"
            );

            return true;
        }


        vTaskDelay(
            pdMS_TO_TICKS(1000)
        );


        elapsed += 1000;
    }


    ESP_LOGW(
        TAG,
        "Time sync timeout"
    );

    return false;
}


void time_service_get(struct tm *timeinfo)
{
    time_t now;


    time(&now);


    localtime_r(
        &now,
        timeinfo
    );
}
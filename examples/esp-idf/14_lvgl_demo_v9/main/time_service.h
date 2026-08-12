#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <time.h>
#include <stdbool.h>

void time_service_init(void);

bool time_service_wait_sync(int timeout_ms);

void time_service_get(struct tm *timeinfo);

#endif
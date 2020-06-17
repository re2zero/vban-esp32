/*
 *
 * Copyright (c) 2020 INFOMEDIA
 *
 *
 */

#ifndef _SERVICE_H_
#define _SERVICE_H_

#include <string.h>
#include <stdlib.h>

#include "rom/queue.h"
#include "audio_error.h"
#include "esp_audio.h"
#include "sdkconfig.h"


#include "periph_touch.h"
#include "esp_peripherals.h"
#include "periph_sdcard.h"
#include "periph_wifi.h"
#include "periph_button.h"

#include "board.h"
#include "display_service.h"
#include "led_indicator.h"
#include "wifi_service.h"
#include "airkiss_config.h"
#include "smart_config.h"
#include "blufi_config.h"
#include "esp_wifi_setting.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_periph_set_handle_t periph_set_init(esp_periph_event_handle_t cb);

esp_err_t periph_start_handle(esp_periph_set_handle_t set);

display_service_handle_t display_service_init(void);

esp_err_t display_service_destory(display_service_handle_t service);

periph_service_handle_t wifi_service_init(periph_service_cb cb);

esp_err_t wifi_service_destory2(periph_service_handle_t service);

#ifdef __cplusplus
}
#endif

#endif

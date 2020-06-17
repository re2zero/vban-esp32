/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "service.h"

static const char *TAG = "SERVICE";

// esp_err_t periph_callback(audio_event_iface_msg_t *event, void *context)
// {
//     ESP_LOGD(TAG, "Periph Event received: src_type:%x, source:%p cmd:%d, data:%p, data_len:%d",
//              event->source_type, event->source, event->cmd, event->data, event->data_len);
//     switch (event->source_type) {
//         case PERIPH_ID_BUTTON: {
//                 if ((int)event->data == get_input_rec_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
//                     ESP_LOGI(TAG, "PERIPH_NOTIFY_KEY_REC");
//                     rec_engine_trigger_start();
//                 } else if ((int)event->data == get_input_mode_id() &&
//                            ((event->cmd == PERIPH_BUTTON_RELEASE) || (event->cmd == PERIPH_BUTTON_LONG_RELEASE))) {
//                     ESP_LOGI(TAG, "PERIPH_NOTIFY_KEY_REC_QUIT");
//                 }
//                 break;
//             }
//         case PERIPH_ID_TOUCH: {
//                 if ((int)event->data == TOUCH_PAD_NUM4 && event->cmd == PERIPH_BUTTON_PRESSED) {

//                     int player_volume = 0;
//                     esp_audio_vol_get(player, &player_volume);
//                     player_volume -= 10;
//                     if (player_volume < 0) {
//                         player_volume = 0;
//                     }
//                     esp_audio_vol_set(player, player_volume);
//                     ESP_LOGI(TAG, "AUDIO_USER_KEY_VOL_DOWN [%d]", player_volume);
//                 } else if ((int)event->data == TOUCH_PAD_NUM4 && (event->cmd == PERIPH_BUTTON_RELEASE)) {


//                 } else if ((int)event->data == TOUCH_PAD_NUM7 && event->cmd == PERIPH_BUTTON_PRESSED) {
//                     int player_volume = 0;
//                     esp_audio_vol_get(player, &player_volume);
//                     player_volume += 10;
//                     if (player_volume > 100) {
//                         player_volume = 100;
//                     }
//                     esp_audio_vol_set(player, player_volume);
//                     ESP_LOGI(TAG, "AUDIO_USER_KEY_VOL_UP [%d]", player_volume);
//                 } else if ((int)event->data == TOUCH_PAD_NUM7 && (event->cmd == PERIPH_BUTTON_RELEASE)) {


//                 } else if ((int)event->data == TOUCH_PAD_NUM8 && event->cmd == PERIPH_BUTTON_PRESSED) {
//                     ESP_LOGI(TAG, "AUDIO_USER_KEY_PLAY [%d]", __LINE__);

//                 } else if ((int)event->data == TOUCH_PAD_NUM8 && (event->cmd == PERIPH_BUTTON_RELEASE)) {


//                 } else if ((int)event->data == TOUCH_PAD_NUM9 && event->cmd == PERIPH_BUTTON_PRESSED) {
//                     if (wifi_setting_flag == false) {
//                         wifi_service_setting_start(wifi_serv, 0);
//                         wifi_setting_flag = true;
//                         display_service_set_pattern(disp_serv, DISPLAY_PATTERN_WIFI_SETTING, 0);
//                         ESP_LOGI(TAG, "AUDIO_USER_KEY_WIFI_SET, WiFi setting started.");
//                     } else {
//                         ESP_LOGW(TAG, "AUDIO_USER_KEY_WIFI_SET, WiFi setting will be stopped.");
//                         wifi_service_setting_stop(wifi_serv, 0);
//                         wifi_setting_flag = false;
//                         display_service_set_pattern(disp_serv, DISPLAY_PATTERN_TURN_OFF, 0);
//                     }
//                 } else if ((int)event->data == TOUCH_PAD_NUM9 && (event->cmd == PERIPH_BUTTON_RELEASE)) {

//                 }
//                 break;
//             }
//         default:
//             break;
//     }
//     return ESP_OK;
// }

esp_periph_set_handle_t periph_set_init(esp_periph_event_handle_t cb)
{
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    esp_periph_set_handle_t set = esp_periph_set_init(&periph_cfg);
    if (set != NULL) {
        esp_periph_set_register_callback(set, cb, NULL);
        return set;
    }
    return NULL;
}

esp_err_t periph_start_handle(esp_periph_set_handle_t set)
{
    periph_button_cfg_t btn_cfg = {
        .gpio_mask = (1ULL << get_input_rec_id()) | (1ULL << get_input_mode_id()), //REC BTN & MODE BTN
    };
    esp_periph_handle_t button_handle = periph_button_init(&btn_cfg);
    esp_err_t err = esp_periph_start(set, button_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR periph start button [%d]", __LINE__);
        return err;
    }

    // If enabled, the touch will consume a lot of CPU.
    periph_touch_cfg_t touch_cfg = {
        .touch_mask = TOUCH_PAD_SEL4 | TOUCH_PAD_SEL7 | TOUCH_PAD_SEL8 | TOUCH_PAD_SEL9,
        .tap_threshold_percent = 70,
    };
    esp_periph_handle_t touch_periph = periph_touch_init(&touch_cfg);
    err = esp_periph_start(set, touch_periph);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR periph start touch [%d]", __LINE__);
        return err;
    }

    periph_sdcard_cfg_t sdcard_cfg = {
        .root = "/sdcard",
        .card_detect_pin = get_sdcard_intr_gpio(), //GPIO_NUM_34
    };
    esp_periph_handle_t sdcard_handle = periph_sdcard_init(&sdcard_cfg);
    err = esp_periph_start(set, sdcard_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ERROR periph start sdcard [%d]", __LINE__);
        return err;
    }
    while (!periph_sdcard_is_mounted(sdcard_handle)) {
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }

    return err;
}

// esp_err_t wifi_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
// {
//     ESP_LOGD(TAG, "event type:%d,source:%p, data:%p,len:%d,ctx:%p",
//              evt->type, evt->source, evt->data, evt->len, ctx);
//     if (evt->type == WIFI_SERV_EVENT_CONNECTED) {
//         ESP_LOGI(TAG, "PERIPH_WIFI_CONNECTED [%d]", __LINE__);
//         // audio_service_connect(duer_serv_handle);
//         // display_service_set_pattern(disp_serv, DISPLAY_PATTERN_WIFI_CONNECTED, 0);
//         // wifi_setting_flag = false;
//     } else if (evt->type == WIFI_SERV_EVENT_DISCONNECTED) {
//         ESP_LOGI(TAG, "PERIPH_WIFI_DISCONNECTED [%d]", __LINE__);
//         // display_service_set_pattern(disp_serv, DISPLAY_PATTERN_WIFI_DISCONNECTED, 0);

//     } else if (evt->type == WIFI_SERV_EVENT_SETTING_TIMEOUT) {
//         // wifi_setting_flag = false;
//     }

//     return ESP_OK;
// }

display_service_handle_t display_service_init(void)
{
    display_service_handle_t disp_serv = NULL;

    led_indicator_handle_t led = led_indicator_init((gpio_num_t)get_green_led_gpio());
    display_service_config_t display = {
        .based_cfg = {
            .task_stack = 0,
            .task_prio  = 0,
            .task_core  = 0,
            .task_func  = NULL,
            .service_start = NULL,
            .service_stop = NULL,
            .service_destroy = NULL,
            .service_ioctl = led_indicator_pattern,
            .service_name = "DISPLAY_serv",
            .user_data = NULL,
        },
        .instance = led,
    };
    disp_serv = display_service_create(&display);

    return disp_serv;
}

esp_err_t display_service_destory(display_service_handle_t service)
{
    return display_destroy(service);
}

periph_service_handle_t wifi_service_init(periph_service_cb cb)
{
    periph_service_handle_t wifi_serv = NULL;

    wifi_config_t sta_cfg = {0};
    strncpy((char *)&sta_cfg.sta.ssid, CONFIG_WIFI_SSID, strlen(CONFIG_WIFI_SSID));
    strncpy((char *)&sta_cfg.sta.password, CONFIG_WIFI_PASSWORD, strlen(CONFIG_WIFI_PASSWORD));

    wifi_service_config_t cfg = WIFI_SERVICE_DEFAULT_CONFIG();
    cfg.evt_cb = cb;
    cfg.cb_ctx = NULL;
    cfg.setting_timeout_s = 60;
    wifi_serv = wifi_service_create(&cfg);

    int reg_idx = 0;
    esp_wifi_setting_handle_t h = NULL;
#ifdef CONFIG_AIRKISS_ENCRYPT
    airkiss_config_info_t air_info = AIRKISS_CONFIG_INFO_DEFAULT();
    air_info.lan_pack.appid = CONFIG_AIRKISS_APPID;
    air_info.lan_pack.deviceid = CONFIG_AIRKISS_DEVICEID;
    air_info.aes_key = CONFIG_AIRBAN_AIRKISS_KEY;
    h = airkiss_config_create(&air_info);
#elif (defined CONFIG_ESP_SMARTCONFIG)
    smart_config_info_t info = SMART_CONFIG_INFO_DEFAULT();
    h = smart_config_create(&info);
#endif
    esp_wifi_setting_regitster_notify_handle(h, (void *)wifi_serv);
    wifi_service_register_setting_handle(wifi_serv, h, &reg_idx);
    wifi_service_set_sta_info(wifi_serv, &sta_cfg);
    wifi_service_connect(wifi_serv);

    return wifi_serv;
}

esp_err_t wifi_service_destory2(periph_service_handle_t service)
{
    return wifi_service_destroy(service);
}

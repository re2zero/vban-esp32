/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 INFOMEDIA
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include "esp_err.h"
#include "manager.h"

static const char *TAG = "TAG_Manager";

typedef struct service_manager {
    periph_service_handle_t wifi_service;
    display_service_handle_t disp_service;
    esp_periph_set_handle_t periph_set;
    bool wifi_setting_flag;
    bool stream_play_flag;
    bool time_synced;
    bool play_runing;
    bool rec_runing;
} service_manager_t;

static service_manager_t *g_service_manager = NULL;

static void service_play_task(void * parm);
static void service_rec_task(void * parm);

static void set_time(void);
static esp_err_t periph_callback(audio_event_iface_msg_t *event, void *context);
static esp_err_t wifi_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx);

void set_time(void)
{
    ESP_LOGI(TAG, "Initializing SNTP & set time");
    struct timeval tv = {
        .tv_sec = 1509449941,
    };
    struct timezone tz = {
        0, 0
    };
    settimeofday(&tv, &tz);

    /* Start SNTP service */
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp1.aliyun.com");  //pool.ntp.org
    sntp_init();

    setenv("TZ", "CST-8", 1);
    tzset();
}

esp_err_t periph_callback(audio_event_iface_msg_t *event, void *context)
{
    ESP_LOGD(TAG, "Periph Event received: src_type:%x, source:%p cmd:%d, data:%p, data_len:%d",
             event->source_type, event->source, event->cmd, event->data, event->data_len);
    switch (event->source_type) {
        case PERIPH_ID_BUTTON: {
            if ((int)event->data == get_input_rec_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
                    ESP_LOGI(TAG, "[ * ] [Rec] button tap event");
                    g_service_manager->stream_play_flag = false;
                    if (g_service_manager->rec_runing == false) {
                        xTaskCreate(service_rec_task, "service_rec_task", 4096, NULL, 3, NULL);
                    } else {
                        // set rec as false in order to stop recording task.
                        g_service_manager->rec_runing = false;
                    }
                } else if ((int)event->data == get_input_mode_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
                    ESP_LOGI(TAG, "[ * ] [Mode] button tap event");
                }
                break;
            }
        case PERIPH_ID_TOUCH: {
            audio_board_handle_t bhd = audio_board_get_handle();
            if ((int) event->data == get_input_play_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
                ESP_LOGI(TAG, "[ * ] [Play] touch tap event");
            } else if ((int) event->data == get_input_set_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
                ESP_LOGI(TAG, "[ * ] [Set] touch tap event");
                if (g_service_manager->wifi_setting_flag == false) {
                    wifi_service_setting_start(g_service_manager->wifi_service, 0);
                    g_service_manager->wifi_setting_flag = true;
                    display_service_set_pattern(g_service_manager->disp_service, DISPLAY_PATTERN_WIFI_SETTING, 0);
                    ESP_LOGI(TAG, "AUDIO_USER_KEY_WIFI_SET, WiFi setting started.");
                } else {
                    ESP_LOGW(TAG, "AUDIO_USER_KEY_WIFI_SET, WiFi setting will be stopped.");
                    wifi_service_setting_stop(g_service_manager->wifi_service, 0);
                    g_service_manager->wifi_setting_flag = false;
                    display_service_set_pattern(g_service_manager->disp_service, DISPLAY_PATTERN_TURN_OFF, 0);
                }
            } else if ((int) event->data == get_input_volup_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
                int pur_vol;
                audio_hal_get_volume(bhd->audio_hal, &pur_vol);
                int cur_vol = pur_vol + 10;
                if (cur_vol > 100) {
                    cur_vol = 100;
                }
                ESP_LOGI(TAG, "[ * ] [Vol+] touch tap event, set vol: %d", cur_vol);
                audio_hal_set_volume(bhd->audio_hal, cur_vol);
            } else if ((int) event->data == get_input_voldown_id() && event->cmd == PERIPH_BUTTON_PRESSED) {
                int pur_vol;
                audio_hal_get_volume(bhd->audio_hal, &pur_vol);
                int cur_vol = pur_vol - 10;
                if (cur_vol < 0) {
                    cur_vol = 0;
                }
                ESP_LOGI(TAG, "[ * ] [Vol-] touch tap event, set vol: %d", cur_vol);
                audio_hal_set_volume(bhd->audio_hal, cur_vol);
            }
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

esp_err_t wifi_service_cb(periph_service_handle_t handle, periph_service_event_t *evt, void *ctx)
{
    ESP_LOGD(TAG, "event type:%d,source:%p, data:%p,len:%d,ctx:%p",
             evt->type, evt->source, evt->data, evt->len, ctx);
    if (evt->type == WIFI_SERV_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "PERIPH_WIFI_CONNECTED [%d]", __LINE__);
        if (g_service_manager->time_synced == false) {
            set_time();
            g_service_manager->time_synced = true;
        }
        display_service_set_pattern(g_service_manager->disp_service, DISPLAY_PATTERN_WIFI_CONNECTED, 0);
        g_service_manager->wifi_setting_flag = false;
        if (g_service_manager->play_runing == false) {
            xTaskCreate(service_play_task, "service_play_task", 4096, NULL, 3, NULL);
        }
    } else if (evt->type == WIFI_SERV_EVENT_DISCONNECTED) {
        ESP_LOGI(TAG, "PERIPH_WIFI_DISCONNECTED [%d]", __LINE__);
        display_service_set_pattern(g_service_manager->disp_service, DISPLAY_PATTERN_WIFI_DISCONNECTED, 0);
    } else if (evt->type == WIFI_SERV_EVENT_SETTING_TIMEOUT) {
        g_service_manager->wifi_setting_flag = false;
    }

    return ESP_OK;
}

void service_play_task(void * parm)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t vban_stream_reader, i2s_stream_writer;

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);

    ESP_LOGI(TAG, "[2.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_WRITER;
    i2s_stream_writer = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.2] Create VBan stream to read data");
    vban_stream_cfg_t vban_cfg = VBAN_STREAM_CFG_DEFAULT();
    vban_cfg.type = AUDIO_STREAM_READER;
    vban_stream_reader = vban_stream_init(&vban_cfg);

    ESP_LOGI(TAG, "[3.1] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, vban_stream_reader, "vban");
    audio_pipeline_register(pipeline, i2s_stream_writer, "i2s");

    ESP_LOGI(TAG, "[3.2] Link it together [UDP]-->vban_stream_reader-->i2s_stream_writer-->[codec_chip]");
    audio_pipeline_link(pipeline, (const char *[]) {"vban", "i2s"}, 2);

    ESP_LOGI(TAG, "[3.3] Set up  uri (and default output is i2s)");
    audio_element_set_uri(vban_stream_reader, "192.168.20.158:6980");

    ESP_LOGI(TAG, "[ 4 ] Initialize peripherals");
    esp_periph_set_handle_t set = g_service_manager->periph_set;


    ESP_LOGI(TAG, "[4.2] Start all peripherals");

    ESP_LOGI(TAG, "[ 5 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[5.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[5.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 6 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 7 ] Listen for all pipeline events");
    g_service_manager->play_runing = true;

    while (1) {
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) vban_stream_reader
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(vban_stream_reader, &music_info);

            ESP_LOGI(TAG, "[ * ] Receive music info from VBan, sample_rates=%d, bits=%d, ch=%d, codec=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels, music_info.reserve_data.user_data_0);
            if (music_info.reserve_data.user_data_0 == VBAN_CODEC_OPUS) {
                ESP_LOGI(TAG, "[ * ] Should use the opus decoder!! TODO");
            }

            audio_element_setinfo(i2s_stream_writer, &music_info);
            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_writer in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int) msg.data == AEL_STATUS_STATE_STOPPED) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 8 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, vban_stream_reader);
    audio_pipeline_unregister(pipeline, i2s_stream_writer);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    // esp_periph_set_stop_all(set); //donot stop periph set.
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(vban_stream_reader);
    audio_element_deinit(i2s_stream_writer);

    g_service_manager->play_runing = false;
    vTaskDelete(NULL);
}


void service_rec_task(void * parm)
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t vban_stream_writer, i2s_stream_reader;

    ESP_LOGI(TAG, "[ 2 ] Create audio pipeline for playback");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(pipeline);

    ESP_LOGI(TAG, "[2.1] Create i2s stream to write data to codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
    i2s_cfg.type = AUDIO_STREAM_READER;
    i2s_stream_reader = i2s_stream_init(&i2s_cfg);

    ESP_LOGI(TAG, "[2.2] Create VBan stream to read data");
    vban_stream_cfg_t vban_cfg = VBAN_STREAM_CFG_DEFAULT();
    vban_cfg.type = AUDIO_STREAM_WRITER;
    vban_stream_writer = vban_stream_init(&vban_cfg);

    ESP_LOGI(TAG, "[3.1] Register all elements to audio pipeline");
    audio_pipeline_register(pipeline, i2s_stream_reader, "i2s");
    audio_pipeline_register(pipeline, vban_stream_writer, "vban");

    ESP_LOGI(TAG, "[3.2] Link it together [codec_chip]-->i2s_stream->vban_stream-->[UDP]");
    audio_pipeline_link(pipeline, (const char *[]) {"i2s", "vban"}, 2);

    ESP_LOGI(TAG, "[3.3] Set up  uri (and default input is i2s)");
    // audio_element_set_uri(vban_stream_reader, "0.0.0.0:6980");
    audio_element_set_uri(vban_stream_writer, "192.168.20.158:6980");

    ESP_LOGI(TAG, "[ 4 ] Initialize peripherals");
    esp_periph_set_handle_t set = g_service_manager->periph_set;


    ESP_LOGI(TAG, "[4.2] Start all peripherals");

    // ESP_LOGI(TAG, "[ 5 ] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);

    ESP_LOGI(TAG, "[5.1] Listening event from all elements of pipeline");
    audio_pipeline_set_listener(pipeline, evt);

    ESP_LOGI(TAG, "[5.2] Listening event from peripherals");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);

    ESP_LOGI(TAG, "[ 6 ] Start audio_pipeline");
    audio_pipeline_run(pipeline);

    ESP_LOGI(TAG, "[ 7 ] Listen for all pipeline events");
    g_service_manager->rec_runing = true;

    while (g_service_manager->rec_runing == true) {
        // vTaskDelay(2000 / portTICK_PERIOD_MS);

        audio_event_iface_msg_t msg;
        // esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        esp_err_t ret = audio_event_iface_listen(evt, &msg, 100);
        if (ret != ESP_OK) {
            // ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
            continue;
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) vban_stream_writer
            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
            audio_element_info_t music_info = {0};
            audio_element_getinfo(vban_stream_writer, &music_info);

            ESP_LOGI(TAG, "[ *REC ] Receive music info from VBan, sample_rates=%d, bits=%d, ch=%d, codec=%d",
                     music_info.sample_rates, music_info.bits, music_info.channels, music_info.reserve_data.user_data_0);
            if (music_info.reserve_data.user_data_0 == VBAN_CODEC_OPUS) {
                ESP_LOGI(TAG, "[ *REC ] Should use the opus decoder!! TODO");
            }

            audio_element_setinfo(i2s_stream_reader, &music_info);
            i2s_stream_set_clk(i2s_stream_reader, music_info.sample_rates, music_info.bits, music_info.channels);
            continue;
        }

        /* Stop when the last pipeline element (i2s_stream_reader in this case) receives stop event */
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_reader
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int) msg.data == AEL_STATUS_STATE_STOPPED) {
            ESP_LOGW(TAG, "[ * ] Stop event received");
            break;
        }
    }

    ESP_LOGI(TAG, "[ 8 ] Stop audio_pipeline");
    audio_pipeline_terminate(pipeline);

    audio_pipeline_unregister(pipeline, vban_stream_writer);
    audio_pipeline_unregister(pipeline, i2s_stream_reader);

    /* Terminate the pipeline before removing the listener */
    audio_pipeline_remove_listener(pipeline);

    /* Stop all peripherals before removing the listener */
    // esp_periph_set_stop_all(set); //donot stop periph set.
    audio_event_iface_remove_listener(esp_periph_set_get_event_iface(set), evt);

    /* Make sure audio_pipeline_remove_listener & audio_event_iface_remove_listener are called before destroying event_iface */
    audio_event_iface_destroy(evt);

    /* Release all resources */
    audio_pipeline_deinit(pipeline);
    audio_element_deinit(vban_stream_writer);
    audio_element_deinit(i2s_stream_reader);

    g_service_manager->rec_runing = false;
    vTaskDelete(NULL);
}

void init_board_codec()
{
    ESP_LOGI(TAG, "[ 0 ] Start codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_BOTH, AUDIO_HAL_CTRL_START);
}

esp_err_t manager_start_service()
{
    esp_periph_set_handle_t set = periph_set_init(periph_callback);
    if (set == NULL) {
        ESP_LOGE(TAG, "Error create periph set!");
        return ESP_FAIL;
    }

    display_service_handle_t disp = display_service_init();
    if (disp == NULL) {
        ESP_LOGE(TAG, "Error create display service!");
        esp_periph_set_destroy(set);
        return ESP_FAIL;
    }

    periph_service_handle_t wifi = wifi_service_init(wifi_service_cb);
    if (wifi == NULL) {
        ESP_LOGE(TAG, "Error create wifi service!");
        display_service_destory(disp);
        esp_periph_set_destroy(set);
        return ESP_FAIL;
    }

    g_service_manager = calloc(1, sizeof(service_manager_t));
    if (g_service_manager == NULL) {
        return ESP_ERR_NO_MEM;
    }
    g_service_manager->periph_set = set;
    g_service_manager->disp_service = disp;
    g_service_manager->wifi_service = wifi;
    g_service_manager->wifi_setting_flag = false;
    g_service_manager->stream_play_flag = false;
    g_service_manager->time_synced = false;
    g_service_manager->play_runing = false;
    g_service_manager->rec_runing = false;

    init_board_codec();

    periph_start_handle(set);

    return ESP_OK;
}

esp_err_t manager_stop_service()
{
    if (g_service_manager == NULL) {
        return ESP_OK;
    }

    if (g_service_manager->disp_service != NULL) {
        display_service_destory(g_service_manager->disp_service);
    }
    if (g_service_manager->wifi_service != NULL) {
        wifi_service_destory2(g_service_manager->wifi_service);
    }
    if (g_service_manager->periph_set != NULL) {
        esp_periph_set_stop_all(g_service_manager->periph_set);
        esp_periph_set_destroy(g_service_manager->periph_set);
    }

    free(g_service_manager);

    return ESP_OK;
}
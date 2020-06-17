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

#include <sys/unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "errno.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "audio_common.h"
#include "audio_mem.h"
#include "audio_element.h"
#include "wav_head.h"
#include "esp_log.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

#include "i2s_stream.h"
#include "vban_stream.h"
#include "socket.h"

static const char *TAG = "VBAN_STREAM";

#define APP_STREAM_NAME             CONFIG_APP_STREAM_NAME

#define SOCKET_MULTICAST_DEFAULT_IF CONFIG_SOCKET_MULTICAST_DEFAULT_IF
#define SOCKET_MULTICAST_LOOPBACK   CONFIG_SOCKET_MULTICAST_LOOPBACK
#define SOCKET_MULTICAST_TTL        CONFIG_SOCKET_MULTICAST_TTL
#define SOCKET_MULTICAST_ADDR       CONFIG_SOCKET_MULTICAST_ADDR

struct stream_info_t
{
    VBanCodec               codec;
    unsigned int            channels;
    unsigned int            rates;
    unsigned int            bits;
};

typedef struct vban_stream {
    audio_stream_type_t         type;
    socket_handle_t             socket;
    char                        buffer[VBAN_PROTOCOL_MAX_SIZE];
    struct socket_config_t      socket_cfg;
    struct socket_multicast_t   mcast_cfg;
    char                        stream_name[VBAN_STREAM_NAME_SIZE];
    bool                        is_init;
    struct stream_info_t        stream_info;
} vban_stream_t;

int check_info(char const* streamname, char const* buffer, struct stream_info_t* info)
{
    struct VBanHeader const* const hdr = PACKET_HEADER_PTR(buffer);

    if ((streamname == 0) || (buffer == 0))
    {
        ESP_LOGE(TAG, "%s: null pointer argument", __func__);
        return -EINVAL;
    }

    if (hdr->vban != VBAN_HEADER_FOURC)
    {
        ESP_LOGE(TAG, "%s: invalid vban magic fourc", __func__);
        return -EINVAL;
    }

    if (strncmp(streamname, hdr->streamname, VBAN_STREAM_NAME_SIZE))
    {
        ESP_LOGE(TAG, "%s: different streamname", __func__);
        return -EINVAL;
    }

    VBanCodec codec = hdr->format_bit & VBAN_CODEC_MASK;
    unsigned int nb_channels = hdr->format_nbc + 1;
    unsigned int sample_rate = VBanSRList[hdr->format_SR & VBAN_SR_MASK];
    unsigned int bit_fmt = stream_int_bit_fmt(hdr->format_bit & VBAN_BIT_RESOLUTION_MASK);
    if (info->codec != codec) {
        info->codec = codec;
        info->channels = nb_channels;
        info->rates = sample_rate;
        info->bits = bit_fmt;
        return 1;
    } else if (info->channels != nb_channels || info->rates != sample_rate || info->bits != bit_fmt) {
        info->channels = nb_channels;
        info->rates = sample_rate;
        info->bits = bit_fmt;
        return 2;
    }

    return 0;
}

static esp_err_t _vban_open(audio_element_handle_t self)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);

    audio_element_info_t info;
    char *uri = audio_element_get_uri(self);
    if (uri == NULL) {
        ESP_LOGE(TAG, "Error, uri is not set");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "_vban_open, uri:%s", uri);

    const char* lcolon = strchr(uri, ':');
    if (!lcolon || lcolon == uri || !lcolon[1]) {
        ESP_LOGE(TAG, "parse port: bad format: expected ADDR:PORT");
        return ESP_FAIL;
    }

    const char* addr = NULL;

    char addr_buf[256] = {};
    if ((lcolon - uri) > sizeof(addr_buf) - 1) {
        ESP_LOGE(TAG, "parse port: bad address: too long");
        return ESP_FAIL;
    }

    memcpy(addr_buf, uri, (lcolon - uri));
    addr_buf[lcolon - uri] = '\0';
    addr = addr_buf;

    const char* port = &lcolon[1];
    if (!isdigit((int)*port)) {
        ESP_LOGE(TAG, "parse port: bad port: not a number");
        return ESP_FAIL;
    }

    char* port_end = NULL;
    long port_num = strtol(port, &port_end, 10);

    if (port_num < 0 || port_num > 65535) {
        ESP_LOGE(TAG, "parse port: bad port: not in range [1; 65535]");
        return ESP_FAIL;
    }

    audio_element_getinfo(self, &info);
    if (vban->is_init) {
        ESP_LOGE(TAG, "already opened");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "_vban_open, ip:%s, port=%d", addr, (int)port_num);

    strncpy(vban->socket_cfg.ip_address, addr, SOCKET_IP_ADDRESS_SIZE-1);
    vban->socket_cfg.port = (int)port_num;
    vban->socket_cfg.direction = vban->type == AUDIO_STREAM_READER ? SOCKET_IN : SOCKET_OUT;

    vban->mcast_cfg.default_if = SOCKET_MULTICAST_DEFAULT_IF;
    vban->mcast_cfg.loopback = SOCKET_MULTICAST_LOOPBACK;
    vban->mcast_cfg.ttl = SOCKET_MULTICAST_TTL;
    strncpy(vban->mcast_cfg.multicast_address, SOCKET_MULTICAST_ADDR, SOCKET_IP_ADDRESS_SIZE-1);

    strncpy(vban->stream_name, APP_STREAM_NAME, VBAN_STREAM_NAME_SIZE-1);
    int ret = socket_init(&(vban->socket), &(vban->socket_cfg), &(vban->mcast_cfg));
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to open vban socket");
        return ESP_FAIL;
    }

    vban->is_init = true;
    return audio_element_setinfo(self, &info);
}

static int _vban_read(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);
    audio_element_info_t info;
    audio_element_getinfo(self, &info);
    ESP_LOGD(TAG, "read len=%d, pos=%d/%d", len, (int)info.byte_pos, (int)info.total_bytes);

    int payload_size = 0;
    int size = socket_read(vban->socket, vban->buffer, VBAN_PROTOCOL_MAX_SIZE);
    if (size < 0) {
        ESP_LOGE(TAG, "socket_read failed: errno %d", errno);
        return 0;
    }

    int ret = check_info(vban->stream_name, vban->buffer, &(vban->stream_info));
    if (ret < 0) {
        ESP_LOGE(TAG, "socket read invalid stream");
        return 0;
    } else {
        bool report = false;
        payload_size = PACKET_PAYLOAD_SIZE(size);

        info.byte_pos += payload_size;
        if (ret > 0) {
            info.sample_rates = vban->stream_info.rates;
            info.channels = vban->stream_info.channels;
            info.bits = vban->stream_info.bits;
            if (ret == 1 && vban->stream_info.codec != VBAN_CODEC_PCM) {
                info.reserve_data.user_data_0 = VBAN_CODEC_OPUS;
            }
            report = true;
            audio_element_setinfo(self, &info);
        }

        if (report) {
            audio_element_report_info(self);
        }

        //copy the received data to buffer
        memcpy(buffer, PACKET_PAYLOAD_PTR(vban->buffer), payload_size);
    }
    // if (packet_check(vban->stream_name, vban->buffer, size) == 0) {
    //     struct stream_config_t stream_config;
    //     packet_get_stream_config(vban->buffer, &stream_config);

    //     info.byte_pos += payload_size;
    //     info.sample_rates = stream_config.sample_rate;
    //     info.channels = stream_config.nb_channels;
    //     info.bits = stream_int_bit_fmt(stream_config.bit_fmt);
    //     audio_element_setinfo(self, &info);
    //     if (!vban->info_report) {
    //         audio_element_report_info(self);
    //         vban->info_report = true;
    //     }

    //     //copy the received data to i2s buffer
    //     payload_size = PACKET_PAYLOAD_SIZE(size);
    //     memcpy(buffer, PACKET_PAYLOAD_PTR(vban->buffer), payload_size);
    // }
    return payload_size;
}

static int _vban_write(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);
    audio_element_info_t info;
    audio_element_getinfo(self, &info);

    struct stream_config_t stream_config;
    stream_config.sample_rate = info.sample_rates;
    stream_config.nb_channels = info.channels;
    stream_config.bit_fmt = info.bits;

    packet_init_header(vban->buffer, &stream_config, vban->stream_name);
    int max_size = packet_get_max_payload_size(vban->buffer);
    if (len > max_size) {
        len = max_size;
    }

    memcpy(PACKET_PAYLOAD_PTR(vban->buffer), buffer, len);
    packet_set_new_content(vban->buffer, len);
    int packet_size = len + sizeof(struct VBanHeader);
    int wlen = 0;
    if (packet_check(vban->stream_name, vban->buffer, packet_size) == 0) {
        wlen = socket_write(vban->socket, vban->buffer, packet_size);
        if (wlen < 0) {
            ESP_LOGE(TAG, "socket_write failed: errno %d", errno);
        }
    }

    ESP_LOGD(TAG, "write,%d, errno:%d,pos:%d", wlen, errno, (int)info.byte_pos);
    if (wlen > 0) {
        info.byte_pos += wlen;
        audio_element_setinfo(self, &info);
    }
    return wlen;
}

static int _vban_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    int r_size = audio_element_input(self, in_buffer, in_len);
    int w_size = 0;
    if (r_size > 0) {
        w_size = audio_element_output(self, in_buffer, r_size);
        audio_element_multi_output(self, in_buffer, r_size, 0);
    } else {
        w_size = r_size;
    }
    return w_size;
}

static esp_err_t _vban_close(audio_element_handle_t self)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);

    if (vban->is_init) {
        vban->is_init = false;
    }
    if (AEL_STATE_PAUSED != audio_element_get_state(self)) {
        audio_element_info_t info = {0};
        audio_element_getinfo(self, &info);
        info.byte_pos = 0;
        audio_element_setinfo(self, &info);
    }
    return ESP_OK;
}

static esp_err_t _vban_destroy(audio_element_handle_t self)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);

    socket_release(&(vban->socket));
    audio_free(vban);
    return ESP_OK;
}

audio_element_handle_t vban_stream_init(vban_stream_cfg_t *config)
{
    audio_element_handle_t el;
    vban_stream_t *vban = audio_calloc(1, sizeof(vban_stream_t));

    AUDIO_MEM_CHECK(TAG, vban, return NULL);

    audio_element_cfg_t cfg = DEFAULT_AUDIO_ELEMENT_CONFIG();
    cfg.open = _vban_open;
    cfg.close = _vban_close;
    cfg.process = _vban_process;
    cfg.destroy = _vban_destroy;
    cfg.task_stack = config->task_stack;
    cfg.task_prio = config->task_prio;
    cfg.task_core = config->task_core;
    cfg.out_rb_size = config->out_rb_size;
    cfg.buffer_len = config->buf_sz;
    cfg.enable_multi_io = true;
    cfg.tag = "vban";
    if (cfg.buffer_len == 0) {
        cfg.buffer_len = VBAN_STREAM_BUF_SIZE;
    }

    vban->type = config->type;
    if (config->type == AUDIO_STREAM_WRITER) {
        cfg.write = _vban_write;
    } else {
        cfg.read = _vban_read;
    }
    ESP_LOGI(TAG, "vban_stream_init");
    el = audio_element_init(&cfg);

    AUDIO_MEM_CHECK(TAG, el, goto _vban_init_exit);
    audio_element_setdata(el, vban);
    return el;
_vban_init_exit:
    audio_free(vban);
    return NULL;
}

esp_err_t vban_stream_join_group(audio_element_handle_t self, const char* multi_ip)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);

    int ret = socket_join_group(vban->socket, multi_ip);
    if (ret < 0) {
        ESP_LOGE(TAG, "join group failed: %d", ret);
    }
    return ESP_OK;
}

esp_err_t vban_stream_leave_group(audio_element_handle_t self, const char* multi_ip)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);

    int ret = socket_leave_group(vban->socket, multi_ip);
    if (ret < 0) {
        ESP_LOGE(TAG, "leave group failed: %d", ret);
    }

    return ESP_OK;
}

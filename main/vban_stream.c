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

static const char *TAG = "VBAN_STREAM";

typedef struct vban_stream {
    audio_stream_type_t type;
#ifdef CONFIG_EXAMPLE_IPV4
    struct sockaddr_in destAddr;
#else // IPV6
    struct sockaddr_in6 destAddr;
#endif
    int sock;
    bool is_init;
} vban_stream_t;

static int packet_pcm_check(char const* buffer, size_t size)
{
    /** the packet is already a valid vban packet and buffer already checked before */

    struct VBanHeader const* const hdr = PACKET_HEADER_PTR(buffer);
    // enum VBanBitResolution const bit_resolution = (VBanBitResolution)(hdr->format_bit & VBAN_BIT_RESOLUTION_MASK);
    const VBanBitResolution bit_resolution = (VBanBitResolution)(hdr->format_bit & VBAN_BIT_RESOLUTION_MASK);
    int const sample_rate   = hdr->format_SR & VBAN_SR_MASK;
    int const nb_samples    = hdr->format_nbs + 1;
    int const nb_channels   = hdr->format_nbc + 1;
    size_t sample_size      = 0;
    size_t payload_size     = 0;

    // ESP_LOGI(TAG, "%s: packet is vban: %u, sr: %d, nbs: %d, nbc: %d, bit: %d, name: %s, nu: %u",
    //    __func__, hdr->vban, hdr->format_SR, hdr->format_nbs, hdr->format_nbc, hdr->format_bit, hdr->streamname, hdr->nuFrame);

    if (bit_resolution >= VBAN_BIT_RESOLUTION_MAX)
    {
        ESP_LOGE(TAG, "invalid bit resolution");
        return -EINVAL;
    }

    if (sample_rate >= VBAN_SR_MAXNUMBER)
    {
        ESP_LOGE(TAG, "invalid sample rate");
        return -EINVAL;
    }

    sample_size = VBanBitResolutionSize[bit_resolution];
    payload_size = nb_samples * sample_size * nb_channels;

    if (payload_size != (size - VBAN_HEADER_SIZE))
    {
        ESP_LOGE(TAG, "invalid payload size");
        return -EINVAL;
    }

    return 0;
}

static int vban_packet_check(char const* streamname, char const* buffer, size_t size)
{
    struct VBanHeader const* const hdr = PACKET_HEADER_PTR(buffer);
    // enum VBanProtocol protocol = VBAN_PROTOCOL_UNDEFINED_4;
    // enum VBanCodec codec = (VBanCodec)VBAN_BIT_RESOLUTION_MAX;
    VBanProtocol protocol = VBAN_PROTOCOL_UNDEFINED_4;
    VBanCodec codec = (VBanCodec)VBAN_BIT_RESOLUTION_MAX;

    if ((streamname == 0) || (buffer == 0))
    {
        ESP_LOGE(TAG, "null pointer argument");
        return -EINVAL;
    }

    if (size <= VBAN_HEADER_SIZE)
    {
        ESP_LOGE(TAG, "packet too small");
        return -EINVAL;
    }

    if (hdr->vban != VBAN_HEADER_FOURC)
    {
        ESP_LOGE(TAG, "invalid vban magic fourc");
        return -EINVAL;
    }

    if (strncmp(streamname, hdr->streamname, VBAN_STREAM_NAME_SIZE))
    {
        ESP_LOGE(TAG, "different streamname");
        return -EINVAL;
    }

    /** check the reserved bit : it must be 0 */
    if (hdr->format_bit & VBAN_RESERVED_MASK)
    {
        ESP_LOGE(TAG, "reserved format bit invalid value");
        return -EINVAL;
    }

    /** check protocol and codec */
    protocol        = (VBanProtocol)(hdr->format_SR & VBAN_PROTOCOL_MASK);
    codec           = (VBanCodec)(hdr->format_bit & VBAN_CODEC_MASK);

    switch (protocol)
    {
        case VBAN_PROTOCOL_AUDIO:
            return (codec == VBAN_CODEC_PCM) ? packet_pcm_check(buffer, size) : -EINVAL;
        case VBAN_PROTOCOL_SERIAL:
        case VBAN_PROTOCOL_TXT:
        case VBAN_PROTOCOL_UNDEFINED_1:
        case VBAN_PROTOCOL_UNDEFINED_2:
        case VBAN_PROTOCOL_UNDEFINED_3:
        case VBAN_PROTOCOL_UNDEFINED_4:
            /** not supported yet */
            return -EINVAL;

        default:
            ESP_LOGI(TAG, "packet with unknown protocol");
            return -EINVAL;
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

    char addr_str[128];
    int addr_family;
    int ip_protocol;

    if (vban->type == AUDIO_STREAM_READER) {
#ifdef CONFIG_EXAMPLE_IPV4
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = htonl(INADDR_ANY);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons((int)port_num);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
#else // IPV6
        struct sockaddr_in6 destAddr;
        bzero(&destAddr.sin6_addr.un, sizeof(destAddr.sin6_addr.un));
        destAddr.sin6_family = AF_INET6;
        destAddr.sin6_port = htons((int)port_num);
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
        inet6_ntoa_r(destAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
#endif
        vban->destAddr = destAddr;
        vban->sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (vban->sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Socket created");

        int err = bind(vban->sock, (struct sockaddr *)&destAddr, sizeof(destAddr));
        if (err < 0) {
            ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        }
        ESP_LOGI(TAG, "Socket binded");
    } else if (vban->type == AUDIO_STREAM_WRITER) {
#ifdef CONFIG_EXAMPLE_IPV4
        struct sockaddr_in destAddr;
        destAddr.sin_addr.s_addr = inet_addr(addr);
        destAddr.sin_family = AF_INET;
        destAddr.sin_port = htons((int)port_num);
        addr_family = AF_INET;
        ip_protocol = IPPROTO_IP;
        inet_ntoa_r(destAddr.sin_addr, addr_str, sizeof(addr_str) - 1);
#else // IPV6
        struct sockaddr_in6 destAddr;
        inet6_aton(addr, &destAddr.sin6_addr);
        destAddr.sin6_family = AF_INET6;
        destAddr.sin6_port = htons((int)port_num);
        addr_family = AF_INET6;
        ip_protocol = IPPROTO_IPV6;
        inet6_ntoa_r(destAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
#endif
        vban->destAddr = destAddr;
        vban->sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (vban->sock < 0) {
            ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Socket created");
    } else {
        ESP_LOGE(TAG, "vban must be sender or receiver");
        return ESP_FAIL;
    }

    if (vban->sock < 0) {
        ESP_LOGE(TAG, "Failed to open vban sock");
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
    char rx_buffer[VBAN_PROTOCOL_MAX_SIZE];
    int payload_size = 0;

    // ESP_LOGI(TAG, "read len=%d, pos=%d/%d", len, (int)info.byte_pos, (int)info.total_bytes);

    // ESP_LOGI(TAG, "Waiting for data, %d", ticks_to_wait);
    struct sockaddr_in6 sourceAddr; // Large enough for both IPv4 or IPv6
    socklen_t socklen = sizeof(sourceAddr);
    len = recvfrom(vban->sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&sourceAddr, &socklen);

    // Error occured during receiving
    if (len < 0) {
        ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
        return 0;
    } else {
        // Data received
        // Get the sender's ip address as string
        char addr_str[128];
        if (sourceAddr.sin6_family == PF_INET) {
            inet_ntoa_r(((struct sockaddr_in *)&sourceAddr)->sin_addr.s_addr, addr_str, sizeof(addr_str) - 1);
        } else if (sourceAddr.sin6_family == PF_INET6) {
            inet6_ntoa_r(sourceAddr.sin6_addr, addr_str, sizeof(addr_str) - 1);
        }

        rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string...
        // ESP_LOGI(TAG, "Received %d bytes from %s:", len, addr_str);

        if (vban_packet_check("esp32", rx_buffer, len) == 0)
        {
            //copy the received data to i2s buffer
            payload_size = PACKET_PAYLOAD_SIZE(len);//(len - VBAN_HEADER_SIZE);
            memcpy(buffer, (rx_buffer) + VBAN_HEADER_SIZE, payload_size);

            struct VBanHeader const* const hdr = PACKET_HEADER_PTR(rx_buffer);
            info.byte_pos += payload_size;
            info.sample_rates = (int)VBanSRList[hdr->format_SR & VBAN_SR_MASK];
            info.channels = hdr->format_nbc + 1;
            info.bits = 16;
            audio_element_setinfo(self, &info);
            // audio_element_report_info(self);
        }
    }
    return payload_size;
}

static int _vban_write(audio_element_handle_t self, char *buffer, int len, TickType_t ticks_to_wait, void *context)
{
    vban_stream_t *vban = (vban_stream_t *)audio_element_getdata(self);
    audio_element_info_t info;
    audio_element_getinfo(self, &info);
    //TODO: create vban packet with the data(buffer).
    int wlen = sendto(vban->sock, buffer, strlen(buffer), 0, (struct sockaddr *)&(vban->destAddr), sizeof(vban->destAddr));
    if (wlen < 0) {
        ESP_LOGE(TAG, "Error occured during sending: errno %d", errno);
        return 0;
    }
    // ESP_LOGI(TAG, "write,%d, errno:%d,pos:%d", wlen, errno, (int)info.byte_pos);
    if (wlen > 0) {
        info.byte_pos += wlen;
        audio_element_setinfo(self, &info);
    }
    return wlen;
}

static int _vban_process(audio_element_handle_t self, char *in_buffer, int in_len)
{
    // ESP_LOGW(TAG, "_vban_process ,%d", in_len);
    // vTaskDelay(3/portTICK_PERIOD_MS);
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
    if (vban->sock != -1) {
        shutdown(vban->sock, 0);
        close(vban->sock);
    }
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

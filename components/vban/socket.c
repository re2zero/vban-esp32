/*
 *  This file is part of vban.
 *  Copyright (c) 2015 by Benoît Quiniou <quiniouben@yahoo.fr>
 *
 *  vban is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  vban is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with vban.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "socket.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/poll.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_system.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

struct socket_t
{
    struct socket_config_t  config;
    int                     fd;
};

#define UDP_PORT CONFIG_EXAMPLE_PORT

#define MULTICAST_LOOPBACK CONFIG_EXAMPLE_LOOPBACK

#define MULTICAST_TTL CONFIG_EXAMPLE_MULTICAST_TTL

#define MULTICAST_IPV4_ADDR CONFIG_EXAMPLE_MULTICAST_IPV4_ADDR
#define MULTICAST_IPV6_ADDR CONFIG_EXAMPLE_MULTICAST_IPV6_ADDR

#define LISTEN_DEFAULT_IF CONFIG_EXAMPLE_LISTEN_DEFAULT_IF

static const char *TAG = "VBAN_SOCKET";
#ifdef CONFIG_EXAMPLE_IPV4
static const char *V4TAG = "mcast-ipv4";
#endif
#ifdef CONFIG_EXAMPLE_IPV6
static const char *V6TAG = "mcast-ipv6";
#endif

static int socket_open(socket_handle_t handle);
static int socket_close(socket_handle_t handle);
static int socket_is_broadcast_address(char const* ip);

#ifdef CONFIG_EXAMPLE_IPV4
/* Add a socket, either IPV4-only or IPV6 dual mode, to the IPV4
   multicast group */
static int socket_add_ipv4_multicast_group(int sock, bool assign_source_if)
{
    struct ip_mreq imreq = { 0 };
    struct in_addr iaddr = { 0 };
    int err = 0;
    // Configure source interface
#if USE_DEFAULT_IF
    imreq.imr_interface.s_addr = IPADDR_ANY;
#else
    tcpip_adapter_ip_info_t ip_info = { 0 };
    err = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(V4TAG, "Failed to get IP address info. Error 0x%x", err);
        goto err;
    }
    inet_addr_from_ip4addr(&iaddr, &ip_info.ip);
#endif
    // Configure multicast address to listen to
    err = inet_aton(MULTICAST_IPV4_ADDR, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid.", MULTICAST_IPV4_ADDR);
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(V4TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV4_ADDR);
    }

    if (assign_source_if) {
        // Assign the IPv4 multicast source interface, via its IP
        // (only necessary if this socket is IPV4 only)
        err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &iaddr,
                         sizeof(struct in_addr));
        if (err < 0) {
            ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_IF. Error %d", errno);
            goto err;
        }
    }

    err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }

 err:
    return err;
}
#endif /* CONFIG_EXAMPLE_IPV4 */

#ifdef CONFIG_EXAMPLE_IPV4_ONLY
static int create_ipv4_socket(bool socket_in)
{
    struct sockaddr_in saddr = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(V4TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }
    if (socket_in) {
        // Bind the socket to any address
        saddr.sin_family = PF_INET;
        saddr.sin_port = htons(UDP_PORT);
        saddr.sin_addr.s_addr = htonl(INADDR_ANY);
        err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in));
        if (err < 0) {
            ESP_LOGE(V4TAG, "Failed to bind socket. Error %d", errno);
            goto err;
        }
    }

    // All set, socket is configured for sending and receiving
    return sock;

err:
    close(sock);
    return -1;
}

static int join_ipv4_group(int sock)
{
    int err = 0;

    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_TTL. Error %d", errno);
        goto err;
    }

#if MULTICAST_LOOPBACK
    // select whether multicast traffic should be received by this device, too
    // (if setsockopt() is not called, the default is no)
    uint8_t loopback_val = MULTICAST_LOOPBACK;
    err = setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                     &loopback_val, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_MULTICAST_LOOP. Error %d", errno);
        goto err;
    }
#endif

    // this is also a listening socket, so add it to the multicast
    // group for listening...
    err = socket_add_ipv4_multicast_group(sock, true);
    if (err < 0) {
        goto err;
    }

    // All set, socket is configured for sending and receiving
    return 0;

err:
    close(sock);
    return -1;
}

static int leave_ipv4_group(int sock)
{
    struct ip_mreq imreq = { 0 };
    int err = 0;
    // Configure source interface
#if USE_DEFAULT_IF
    imreq.imr_interface.s_addr = IPADDR_ANY;
#else
    tcpip_adapter_ip_info_t ip_info = { 0 };
    err = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(V4TAG, "Failed to get IP address info. Error 0x%x", err);
        goto err;
    }
    imreq.imr_interface.s_addr = &ip_info.ip;
#endif
    // Configure multicast address to listen to
    err = inet_aton(MULTICAST_IPV4_ADDR, &imreq.imr_multiaddr.s_addr);
    if (err != 1) {
        ESP_LOGE(V4TAG, "Configured IPV4 multicast address '%s' is invalid.", MULTICAST_IPV4_ADDR);
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV4 Multicast address %s", inet_ntoa(imreq.imr_multiaddr.s_addr));
    if (!IP_MULTICAST(ntohl(imreq.imr_multiaddr.s_addr))) {
        ESP_LOGW(V4TAG, "Configured IPV4 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV4_ADDR);
    }

    err = setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         &imreq, sizeof(struct ip_mreq));
    if (err < 0) {
        ESP_LOGE(V4TAG, "Failed to set IP_DROP_MEMBERSHIP. Error %d", errno);
        goto err;
    }
    return 0;

 err:
    return err;
}
#endif /* CONFIG_EXAMPLE_IPV4_ONLY */

#ifdef CONFIG_EXAMPLE_IPV6
static int create_ipv6_socket(bool socket_in)
{
    struct sockaddr_in6 saddr = { 0 };
    struct in6_addr if_inaddr = { 0 };
    struct ip6_addr if_ipaddr = { 0 };
    struct ip6_mreq v6imreq = { 0 };
    int sock = -1;
    int err = 0;

    sock = socket(PF_INET6, SOCK_DGRAM, IPPROTO_IPV6);
    if (sock < 0) {
        ESP_LOGE(V6TAG, "Failed to create socket. Error %d", errno);
        return -1;
    }
    
    if (socket_in) {
        // Bind the socket to any address
        saddr.sin6_family = AF_INET6;
        saddr.sin6_port = htons(UDP_PORT);
        bzero(&saddr.sin6_addr.un, sizeof(saddr.sin6_addr.un));
        err = bind(sock, (struct sockaddr *)&saddr, sizeof(struct sockaddr_in6));
        if (err < 0) {
            ESP_LOGE(V6TAG, "Failed to bind socket. Error %d", errno);
            goto err;
        }
    }
    ESP_LOGI(TAG, "Socket set IPV6-only");

    // All set, socket is configured for sending and receiving
    return sock;

err:
    close(sock);
    return -1;
}

static int join_ipv6_group(int sock)
{
    struct in6_addr if_inaddr = { 0 };
    struct ip6_addr if_ipaddr = { 0 };
    struct ip6_mreq v6imreq = { 0 };
    int err = 0;
    
    // Selct the interface to use as multicast source for this socket.
#if USE_DEFAULT_IF
    bzero(&if_inaddr.un, sizeof(if_inaddr.un));
#else
    // Read interface adapter link-local address and use it
    // to bind the multicast IF to this socket.
    //
    // (Note the interface may have other non-LL IPV6 addresses as well,
    // but it doesn't matter in this context as the address is only
    // used to identify the interface.)
    err = tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr);
    inet6_addr_from_ip6addr(&if_inaddr, &if_ipaddr);
    if (err != ESP_OK) {
        ESP_LOGE(V6TAG, "Failed to get IPV6 LL address. Error 0x%x", err);
        goto err;
    }
#endif

    // Assign the multicast source interface, via its IP
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_IF, &if_inaddr,
                     sizeof(struct in6_addr));
    if (err < 0) {
        ESP_LOGE(V6TAG, "Failed to set IPV6_MULTICAST_IF. Error %d", errno);
        goto err;
    }

    // Assign multicast TTL (set separately from normal interface TTL)
    uint8_t ttl = MULTICAST_TTL;
    setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V6TAG, "Failed to set IPV6_MULTICAST_HOPS. Error %d", errno);
        goto err;
    }

#if MULTICAST_LOOPBACK
    // select whether multicast traffic should be received by this device, too
    // (if setsockopt() is not called, the default is no)
    uint8_t loopback_val = MULTICAST_LOOPBACK;
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                     &loopback_val, sizeof(uint8_t));
    if (err < 0) {
        ESP_LOGE(V6TAG, "Failed to set IPV6_MULTICAST_LOOP. Error %d", errno);
        goto err;
    }
#endif

    // this is also a listening socket, so add it to the multicast
    // group for listening...

    // Configure source interface
#if USE_DEFAULT_IF
    v6imreq.imr_interface.s_addr = IPADDR_ANY;
#else
    inet6_addr_from_ip6addr(&v6imreq.ipv6mr_interface, &if_ipaddr);
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    // Configure multicast address to listen to
    err = inet6_aton(MULTICAST_IPV6_ADDR, &v6imreq.ipv6mr_multiaddr);
    if (err != 1) {
        ESP_LOGE(V6TAG, "Configured IPV6 multicast address '%s' is invalid.", MULTICAST_IPV6_ADDR);
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV6 Multicast address %s", inet6_ntoa(v6imreq.ipv6mr_multiaddr));
    ip6_addr_t multi_addr;
    inet6_addr_to_ip6addr(&multi_addr, &v6imreq.ipv6mr_multiaddr);
    if (!ip6_addr_ismulticast(&multi_addr)) {
        ESP_LOGW(V6TAG, "Configured IPV6 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV6_ADDR);
    }

    err = setsockopt(sock, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP,
                     &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        ESP_LOGE(V6TAG, "Failed to set IPV6_ADD_MEMBERSHIP. Error %d", errno);
        goto err;
    }
#endif

#if CONFIG_EXAMPLE_IPV4_V6
    // Add the common IPV4 config options
    err = socket_add_ipv4_multicast_group(sock, false);
    if (err < 0) {
        goto err;
    }
#endif

#if CONFIG_EXAMPLE_IPV4_V6
    int only = 0;
#else
    int only = 1; /* IPV6-only socket */
#endif
    err = setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &only, sizeof(int));
    if (err < 0) {
        ESP_LOGE(V6TAG, "Failed to set IPV6_V6ONLY. Error %d", errno);
        goto err;
    }
    ESP_LOGI(TAG, "Socket set IPV6-only");

    // All set, socket is configured for sending and receiving
    return 0;

err:
    close(sock);
    return -1;
}

static int leave_ipv6_group(int sock)
{
    struct ip6_addr if_ipaddr = { 0 };
    struct ip6_mreq v6imreq = { 0 };
    int err = 0;

        // Configure source interface
#if USE_DEFAULT_IF
    v6imreq.imr_interface.s_addr = IPADDR_ANY;
#else
    err = tcpip_adapter_get_ip6_linklocal(TCPIP_ADAPTER_IF_STA, &if_ipaddr);
    if (err != ESP_OK) {
        ESP_LOGE(V6TAG, "Failed to get IPV6 LL address. Error 0x%x", err);
        goto err;
    }
    inet6_addr_from_ip6addr(&v6imreq.ipv6mr_interface, &if_ipaddr);
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    // Configure multicast address to listen to
    err = inet6_aton(MULTICAST_IPV6_ADDR, &v6imreq.ipv6mr_multiaddr);
    if (err != 1) {
        ESP_LOGE(V6TAG, "Configured IPV6 multicast address '%s' is invalid.", MULTICAST_IPV6_ADDR);
        goto err;
    }
    ESP_LOGI(TAG, "Configured IPV6 Multicast address %s", inet6_ntoa(v6imreq.ipv6mr_multiaddr));
    ip6_addr_t multi_addr;
    inet6_addr_to_ip6addr(&multi_addr, &v6imreq.ipv6mr_multiaddr);
    if (!ip6_addr_ismulticast(&multi_addr)) {
        ESP_LOGW(V6TAG, "Configured IPV6 multicast address '%s' is not a valid multicast address. This will probably not work.", MULTICAST_IPV6_ADDR);
    }

    err = setsockopt(sock, IPPROTO_IPV6, IPV6_DROP_MEMBERSHIP,
                     &v6imreq, sizeof(struct ip6_mreq));
    if (err < 0) {
        ESP_LOGE(V6TAG, "Failed to set IPV6_DROP_MEMBERSHIP. Error %d", errno);
        goto err;
    }
#endif

#if CONFIG_EXAMPLE_IPV4_V6
    // Add the common IPV4 config options
    err = leave_ipv4_group(sock);
    if (err < 0) {
        goto err;
    }
#endif

    // All set, socket is configured for sending and receiving
    return 0;
err:
    return err;
}
#endif

int socket_init(socket_handle_t* handle, struct socket_config_t const* config)
{
    int ret = 0;

    if ((handle == 0) || (config == 0))
    {
        ESP_LOGE(TAG, "%s: null handle or config pointer", __func__);
        return -EINVAL;
    }

    *handle = calloc(1, sizeof(struct socket_t));
    if (*handle == 0)
    {
        ESP_LOGE(TAG, "%s: could not allocate memory", __func__);
        return -ENOMEM;
    }

    (*handle)->config = *config;

    ret = socket_open(*handle);
    if (ret != 0)
    {
        socket_release(handle);
    }

    return ret;
}

int socket_release(socket_handle_t* handle)
{
    int ret = 0;

    if (handle == 0)
    {
        ESP_LOGE(TAG, "%s: null handle pointer", __func__);
        return -EINVAL;
    }

    if (*handle != 0)
    {
        ret = socket_close(*handle);
        free(*handle);
        *handle = 0;
    }

    return ret;
}

int socket_is_broadcast_address(char const* ip)
{
    return strncmp(ip + strlen(ip) - 3, "255", 3) == 0;
}

int socket_open(socket_handle_t handle)
{
    int ret = 0;

    if (handle == 0)
    {
        ESP_LOGE(TAG, "%s: one parameter is a null pointer", __func__);
        return -EINVAL;
    }

    ESP_LOGD(TAG, "%s: opening socket with port %d", __func__, handle->config.port);

    if (handle->fd != 0)
    {
        ret = socket_close(handle);
        if (ret != 0)
        {
            ESP_LOGE(TAG, "%s: socket was open and unable to close it", __func__);
            return ret;
        }
    }

#ifdef CONFIG_EXAMPLE_IPV4_ONLY
        handle->fd = create_ipv4_socket(handle->config.direction == SOCKET_IN);
#else
        handle->fd = create_ipv6_socket(handle->config.direction == SOCKET_IN);
#endif

    // handle->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle->fd < 0)
    {
        ESP_LOGE(TAG, "%s: unable to create socket", __func__);
        ret = handle->fd;
        handle->fd = 0;
        return ret;
    }

    ESP_LOGI(TAG, "%s with port: %d, fd=%d", __func__, handle->config.port, handle->fd);

    return 0;
}

int socket_close(socket_handle_t handle)
{
    int ret = 0;

    if (handle == 0)
    {
        ESP_LOGE(TAG, "%s: handle parameter is a null pointer", __func__);
        return -EINVAL;
    }

    ESP_LOGI(TAG, "%s: closing socket with port %d", __func__, handle->config.port);

    if (handle->fd != 0)
    {
        shutdown(handle->fd, 0);
        ret = close(handle->fd);
        handle->fd = 0;
        if (ret != 0)
        {
            ESP_LOGE(TAG, "%s: unable to close socket", __func__);
            return ret;
        }
    }

    return 0;
}

int socket_read(socket_handle_t handle, char* buffer, size_t size)
{
    int ret = 0;

    struct sockaddr_in6 raddr; // Large enough for both IPv4 or IPv6
    socklen_t socklen = sizeof(raddr);
    char raddr_name[SOCKET_IP_ADDRESS_SIZE] = { 0 };

    ESP_LOGD(TAG, "%s invoked, fd=%d", __func__, handle->fd);

    if ((handle == 0) || (buffer == 0))
    {
        ESP_LOGE(TAG, "%s: one parameter is a null pointer", __func__);
        return -EINVAL;
    }

    ESP_LOGD(TAG, "%s ip %s", __func__, handle->config.ip_address);

    if (handle->fd == 0)
    {
        ESP_LOGE(TAG, "%s: socket is not open", __func__);
        return -ENODEV;
    }

again:
    ret = recvfrom(handle->fd, buffer, size, 0, (struct sockaddr *)&raddr, &socklen);
    if (ret < 0)
    {
        if (errno != EINTR)
        {
            ESP_LOGE(TAG, "%s: recvfrom error %d %s", __func__, errno, strerror(errno));
        }
        return ret;
    }

    // Get the sender's address as a string
#ifdef CONFIG_EXAMPLE_IPV4
    if (raddr.sin6_family == PF_INET) {
        inet_ntoa_r(((struct sockaddr_in *)&raddr)->sin_addr.s_addr,
                    raddr_name, sizeof(raddr_name)-1);
    }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    if (raddr.sin6_family == PF_INET6) {
        inet6_ntoa_r(raddr.sin6_addr, raddr_name, sizeof(raddr_name)-1);
    }
#endif
    // ESP_LOGI(TAG, "received %d bytes from %s:", ret, raddr_name);

    if (strncmp(handle->config.ip_address, raddr_name, SOCKET_IP_ADDRESS_SIZE))
    {
        ESP_LOGD(TAG, "%s: packet received from wrong ip", __func__);
        goto again;
    }

    return ret;
}

int socket_write(socket_handle_t handle, char const* buffer, size_t size)
{
    int ret = 0;

    ESP_LOGD(TAG, "%s invoked", __func__);

    if ((handle == 0) || (buffer == 0))
    {
        ESP_LOGE(TAG, "%s: one parameter is a null pointer", __func__);
        return -EINVAL;
    }

    if (handle->fd == 0)
    {
        ESP_LOGE(TAG, "%s: socket is not open", __func__);
        return -ENODEV;
    }
#if 1
    char addrbuf[SOCKET_IP_ADDRESS_SIZE] = { 0 };
    struct addrinfo hints = {
        .ai_flags = AI_PASSIVE,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *res;

#ifdef CONFIG_EXAMPLE_IPV4 // Send an IPv4 multicast packet

#ifdef CONFIG_EXAMPLE_IPV4_ONLY
    hints.ai_family = AF_INET; // For an IPv4 socket
#else
    hints.ai_family = AF_INET6; // For an IPv4 socket with V4 mapped addresses
    hints.ai_flags |= AI_V4MAPPED;
#endif
    int err = getaddrinfo(handle->config.ip_address,
                            NULL,
                            &hints,
                            &res);
    if (err < 0) {
        ESP_LOGE(TAG, "getaddrinfo() failed for IPV4 destination address. error: %d", err);
        return err;
    }
#ifdef CONFIG_EXAMPLE_IPV4_ONLY
    ((struct sockaddr_in *)res->ai_addr)->sin_port = htons(handle->config.port);
    inet_ntoa_r(((struct sockaddr_in *)res->ai_addr)->sin_addr, addrbuf, sizeof(addrbuf)-1);
    ESP_LOGI(TAG, "Sending to IPV4 multicast address %s:%d...",  addrbuf, handle->config.port);
#else
    ((struct sockaddr_in6 *)res->ai_addr)->sin6_port = htons(handle->config.port);
    inet6_ntoa_r(((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, addrbuf, sizeof(addrbuf)-1);
    ESP_LOGI(TAG, "Sending to IPV6 (V4 mapped) multicast address %s port %d (%s)...",  addrbuf, handle->config.port, handle->config.ip_address);
#endif
    ret = sendto(handle->fd, buffer, size, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret < 0) {
        ESP_LOGE(TAG, "IPV4 sendto failed. errno: %d", errno);
        return ret;
    }
#endif
#ifdef CONFIG_EXAMPLE_IPV6
    hints.ai_family = AF_INET6;
    hints.ai_protocol = 0;
    err = getaddrinfo(handle->config.ip_address,
                        NULL,
                        &hints,
                        &res);
    if (err < 0) {
        ESP_LOGE(TAG, "getaddrinfo() failed for IPV6 destination address. error: %d", err);
        return ret;
    }

    struct sockaddr_in6 *s6addr = (struct sockaddr_in6 *)res->ai_addr;
    s6addr->sin6_port = htons(handle->config.port);
    inet6_ntoa_r(s6addr->sin6_addr, addrbuf, sizeof(addrbuf)-1);
    ESP_LOGI(TAG, "Sending to IPV6 multicast address %s port %d...",  addrbuf, handle->config.port);
    ret = sendto(handle->fd, buffer, size, 0, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (ret < 0) {
        ESP_LOGE(TAG, "IPV6 sendto failed. errno: %d", errno);
        if (errno != EINTR)
        {
            ESP_LOGE(TAG, "%s: sendto error %d %s", __func__, errno, strerror(errno));
        }
        return ret;
    }
#endif
#else
    struct sockaddr_in si_other;
    socklen_t slen = sizeof(si_other);
    memset(&si_other, 0, sizeof(si_other));
    si_other.sin_family        = AF_INET;
    si_other.sin_port          = htons(handle->config.port);
    si_other.sin_addr.s_addr   = inet_addr(handle->config.ip_address);

    ret = sendto(handle->fd, buffer, size, 0, (struct sockaddr*)&si_other, slen);
    if (ret < 0)
    {
        if (errno != EINTR)
        {
            ESP_LOGE(TAG, "%s: sendto error %d %s", __func__, errno, strerror(errno));
        }
        return ret;
    }
#endif
    return ret;
}


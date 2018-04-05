/*
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 *
 * Ethernet driver for native posix board. This is meant for network
 * connectivity between host and Zephyr.
 */

#define SYS_LOG_DOMAIN "eth-posix"
#define SYS_LOG_LEVEL CONFIG_SYS_LOG_ETHERNET_LEVEL

#include <logging/sys_log.h>
#include <stdio.h>

#include <kernel.h>

#include <stdbool.h>
#include <errno.h>
#include <stddef.h>

#include <net/net_pkt.h>
#include <net/net_core.h>
#include <net/net_if.h>
#include <net/ethernet.h>

#include "eth_native_posix_priv.h"

#if defined(CONFIG_NET_L2_ETHERNET)
#define _ETH_L2_LAYER ETHERNET_L2
#define _ETH_L2_CTX_TYPE NET_L2_GET_CTX_TYPE(ETHERNET_L2)
#define _ETH_MTU 1500
#endif

#define NET_BUF_TIMEOUT MSEC(10)

struct eth_context {
	u8_t recv[_ETH_MTU + sizeof(struct net_eth_hdr)];
	u8_t send[_ETH_MTU + sizeof(struct net_eth_hdr)];
	u8_t mac_addr[6];
	struct net_linkaddr ll_addr;
	struct net_if *iface;
	const char *if_name;
	int dev_fd;
	bool init_done;
	bool status;
};

NET_STACK_DEFINE(RX_ZETH, eth_rx_stack,
		 CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE,
		 CONFIG_ARCH_POSIX_RECOMMENDED_STACK_SIZE);
static struct k_thread rx_thread_data;

/* TODO: support multiple interfaces */
static struct eth_context eth_context_data;

static struct eth_context *get_context(struct net_if *iface)
{
	return net_if_get_device(iface)->driver_data;
}

static int eth_send(struct net_if *iface, struct net_pkt *pkt)
{
	struct eth_context *ctx = get_context(iface);
	struct net_buf *frag;
	int count = 0;

	/* First fragment contains link layer (Ethernet) headers.
	 */
	count = net_pkt_ll_reserve(pkt) + pkt->frags->len;
	memcpy(ctx->send, net_pkt_ll(pkt), count);

	/* Then the remaining data */
	frag = pkt->frags->frags;
	while (frag) {
		memcpy(ctx->send + count, frag->data, frag->len);
		count += frag->len;
		frag = frag->frags;
	}

	net_pkt_unref(pkt);

	SYS_LOG_DBG("Send pkt %p len %d", pkt, count);

	eth_write_data(ctx->dev_fd, ctx->send, count);

	return 0;
}

static int eth_init(struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static struct net_linkaddr *eth_get_mac(struct eth_context *ctx)
{
	ctx->ll_addr.addr = ctx->mac_addr;
	ctx->ll_addr.len = sizeof(ctx->mac_addr);

	return &ctx->ll_addr;
}

static int read_data(struct eth_context *ctx, int fd)
{
	struct net_pkt *pkt;
	struct net_buf *frag;
	int ret;

	ret = eth_read_data(fd, ctx->recv, sizeof(ctx->recv));
	if (ret <= 0) {
		return 0;
	}

	pkt = net_pkt_get_reserve_rx(0, NET_BUF_TIMEOUT);
	if (!pkt) {
		return -ENOMEM;
	}

	do {
		int count = 0;

		frag = net_pkt_get_frag(pkt, NET_BUF_TIMEOUT);
		if (!frag) {
			net_pkt_unref(pkt);
			return -ENOMEM;
		}

		net_pkt_frag_add(pkt, frag);

		net_buf_add_mem(frag, ctx->recv + count,
				min(net_buf_tailroom(frag), ret));
		ret -= frag->len;
		count += frag->len;
	} while (ret > 0);

	SYS_LOG_DBG("Recv pkt %p len %d", pkt, net_pkt_get_len(pkt));

	if (net_recv_data(ctx->iface, pkt) < 0) {
		net_pkt_unref(pkt);
	}

	return 0;
}

static void eth_rx(struct eth_context *ctx)
{
	int ret;

	SYS_LOG_DBG("Starting ZETH RX thread");

	while (1) {
		if (net_if_is_up(ctx->iface)) {
			ret = eth_wait_data(ctx->dev_fd);
			if (!ret) {
				read_data(ctx, ctx->dev_fd);
			}
		}

		k_sleep(MSEC(50));
	}
}

static void create_rx_handler(struct eth_context *ctx)
{
	k_thread_create(&rx_thread_data, eth_rx_stack,
			K_THREAD_STACK_SIZEOF(eth_rx_stack),
			(k_thread_entry_t)eth_rx,
			ctx, NULL, NULL, K_PRIO_COOP(14),
			0, K_NO_WAIT);
}

static void eth_iface_init(struct net_if *iface)
{
	struct eth_context *ctx = net_if_get_device(iface)->driver_data;
	struct net_linkaddr *ll_addr = eth_get_mac(ctx);

	if (ctx->init_done) {
		return;
	}

	ctx->init_done = true;
	ctx->iface = iface;

#if defined(CONFIG_ETH_NATIVE_POSIX_RANDOM_MAC)
	/* 00-00-5E-00-53-xx Documentation RFC 7042 */
	ctx->mac_addr[0] = 0x00;
	ctx->mac_addr[1] = 0x00;
	ctx->mac_addr[2] = 0x5E;
	ctx->mac_addr[3] = 0x00;
	ctx->mac_addr[4] = 0x53;
	ctx->mac_addr[5] = sys_rand32_get();

	/* The TUN/TAP setup script will by default set the MAC address of host
	 * interface to 00:00:5E:00:53:FF so do not allow that.
	 */
	if (ctx->mac_addr[5] == 0xff) {
		ctx->mac_addr[5] = 0x01;
	}
#else
	if (CONFIG_ETH_NATIVE_POSIX_MAC_ADDR[0] != 0) {
		if (net_bytes_from_str(ctx->mac_addr, sizeof(ctx->mac_addr),
				       CONFIG_ETH_NATIVE_POSIX_MAC_ADDR) < 0) {
			SYS_LOG_ERR("Invalid MAC address %s",
				    CONFIG_ETH_NATIVE_POSIX_MAC_ADDR);
		}
	}
#endif

	net_if_set_link_addr(iface, ll_addr->addr, ll_addr->len,
			     NET_LINK_ETHERNET);

	ctx->if_name = CONFIG_ETH_NATIVE_POSIX_DRV_NAME;

	ctx->dev_fd = eth_iface_create(ctx->if_name, false);
	if (ctx->dev_fd < 0) {
		SYS_LOG_ERR("Cannot create %s (%d)", ctx->if_name,
			    ctx->dev_fd);
	} else {
		/* Create a thread that will handle incoming data from host */
		create_rx_handler(ctx);

		eth_setup_host(ctx->if_name);
	}
}

static const struct ethernet_api eth_if_api = {
	.iface_api.init = eth_iface_init,
	.iface_api.send = eth_send,
};

NET_DEVICE_INIT(eth_native_posix, CONFIG_ETH_NATIVE_POSIX_DRV_NAME,
		eth_init, &eth_context_data, NULL,
		CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &eth_if_api,
		_ETH_L2_LAYER, _ETH_L2_CTX_TYPE, _ETH_MTU);

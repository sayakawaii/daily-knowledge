// SPDX-License-Identifier: GPL-2.0
/*
 * tvcan：虚拟 CAN 网卡驱动。加载后出现一对网卡 tvcan0 和 tvcan1，
 * 像用一根 CAN 线连起来一样：从一个发出的帧，在另一个上收到。
 *
 * 用来学网络设备驱动的基本结构：net_device、net_device_ops、sk_buff、netif_rx、统计计数。
 * 内核里功能完整的同类驱动是 drivers/net/can/vxcan.c。
 *
 * 和真 CAN 控制器驱动（比如 mttcan）的区别：真驱动在 ndo_start_xmit 里把帧写进控制器的发送缓冲区，
 * 发完后在中断里回收 skb、更新统计；收到帧时在中断（或 NAPI 轮询）里分配 skb、填数据、交给协议栈。
 */
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/can.h>
#include <linux/can/dev.h>
#include <linux/can/skb.h>
#include <linux/can/can-ml.h>

struct tvcan_priv {
	struct can_ml_priv ml;		/* CAN 协议栈需要的每网卡数据，必须有 */
	struct net_device *peer;
};

static struct net_device *tvcan_dev[2];

/*
 * 协议栈要发一帧时调用。可能在软中断上下文，不能睡眠。
 * 返回 NETDEV_TX_OK 表示这个 skb 已经归驱动处理（发出去或者丢掉都算），协议栈不会再碰它。
 */
static netdev_tx_t tvcan_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct tvcan_priv *priv = netdev_priv(dev);
	struct net_device *peer = priv->peer;
	struct canfd_frame *cf = (struct canfd_frame *)skb->data;
	unsigned int len;

	/* 检查 skb 是不是合法的 CAN 帧，不合法就丢掉并计数（返回 true） */
	if (can_dropped_invalid_skb(dev, skb))
		return NETDEV_TX_OK;

	/* 「线」那头的网卡没打开：真 CAN 总线上没人应答，这里直接丢掉 */
	if (!netif_running(peer)) {
		dev->stats.tx_dropped++;
		kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	len = (cf->can_id & CAN_RTR_FLAG) ? 0 : cf->len;

	/* 把同一个 skb 交给对端网卡的接收路径 */
	skb->dev = peer;
	skb->pkt_type = PACKET_BROADCAST;
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	skb->csum_start = 0;

	if (netif_rx(skb) == NET_RX_SUCCESS) {
		dev->stats.tx_packets++;
		dev->stats.tx_bytes += len;
		peer->stats.rx_packets++;
		peer->stats.rx_bytes += len;
	} else {
		peer->stats.rx_dropped++;
	}
	return NETDEV_TX_OK;
}

static int tvcan_open(struct net_device *dev)
{
	netif_carrier_on(dev);
	netif_start_queue(dev);		/* 允许协议栈调用 ndo_start_xmit */
	return 0;
}

static int tvcan_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	netif_carrier_off(dev);
	return 0;
}

static const struct net_device_ops tvcan_ops = {
	.ndo_open = tvcan_open,
	.ndo_stop = tvcan_stop,
	.ndo_start_xmit = tvcan_xmit,
	.ndo_change_mtu = can_change_mtu,	/* 允许 ip link set mtu 72 切到 CAN FD */
};

static void tvcan_setup(struct net_device *dev)
{
	dev->type = ARPHRD_CAN;
	dev->mtu = CANFD_MTU;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	/*
	 * 不设 IFF_ECHO：由 CAN 协议栈在软件里做「本地回环」，同一台机器上监听这个网卡的其他 socket
	 * 也能看到发出去的帧。真控制器驱动设 IFF_ECHO，等帧真的发到总线上以后才回送（can_put_echo_skb）。
	 */
	dev->flags = IFF_NOARP;
	dev->netdev_ops = &tvcan_ops;
	dev->needs_free_netdev = true;
}

static struct net_device *tvcan_create(const char *name)
{
	struct net_device *dev;
	struct tvcan_priv *priv;

	dev = alloc_netdev(sizeof(struct tvcan_priv), name, NET_NAME_USER, tvcan_setup);
	if (!dev)
		return NULL;
	priv = netdev_priv(dev);
	can_set_ml_priv(dev, &priv->ml);
	return dev;
}

static int __init tvcan_init(void)
{
	int ret;

	tvcan_dev[0] = tvcan_create("tvcan0");
	tvcan_dev[1] = tvcan_create("tvcan1");
	if (!tvcan_dev[0] || !tvcan_dev[1]) {
		ret = -ENOMEM;
		goto err_free;
	}
	((struct tvcan_priv *)netdev_priv(tvcan_dev[0]))->peer = tvcan_dev[1];
	((struct tvcan_priv *)netdev_priv(tvcan_dev[1]))->peer = tvcan_dev[0];

	ret = register_netdev(tvcan_dev[0]);
	if (ret)
		goto err_free;
	ret = register_netdev(tvcan_dev[1]);
	if (ret) {
		unregister_netdev(tvcan_dev[0]);	/* needs_free_netdev：注销时自动释放 */
		tvcan_dev[0] = NULL;
		goto err_free;
	}
	pr_info("tvcan: tvcan0 <-> tvcan1 ready\n");
	return 0;

err_free:
	if (tvcan_dev[0])
		free_netdev(tvcan_dev[0]);
	if (tvcan_dev[1])
		free_netdev(tvcan_dev[1]);
	return ret;
}

static void __exit tvcan_exit(void)
{
	/* 两个一起注销：rtnl 锁内完成，期间不会有一个已经释放、另一个还在往它发帧的情况 */
	rtnl_lock();
	unregister_netdevice(tvcan_dev[0]);
	unregister_netdevice(tvcan_dev[1]);
	rtnl_unlock();
}

module_init(tvcan_init);
module_exit(tvcan_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("jetson-drivers ch08: virtual CAN interface pair");

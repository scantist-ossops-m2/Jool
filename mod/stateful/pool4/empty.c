#include "nat64/mod/stateful/pool4/empty.h"
#include <linux/inetdevice.h>
#include <linux/in_route.h>
#include <linux/netdevice.h>
#include "nat64/common/constants.h"
#include "nat64/mod/common/ipv6_hdr_iterator.h"

bool pool4empty_contains(const struct ipv4_transport_addr *addr)
{
	struct net_device *dev;
	struct in_device *in_dev;
	bool found = false;

	if (addr->l4 < DEFAULT_POOL4_MIN_PORT)
		return false;
	/* I sure hope this gets compiled out :p */
	if (DEFAULT_POOL4_MAX_PORT < addr->l4)
		return false;

	rcu_read_lock();

	for_each_netdev_rcu(&init_net, dev) {
		in_dev = __in_dev_get_rcu(dev);
		if (!in_dev)
			continue;

		for_primary_ifa(in_dev) {
			if (ifa->ifa_scope != RT_SCOPE_UNIVERSE)
				continue;
			if (ifa->ifa_address == addr->l3.s_addr) {
				found = true;
				goto out;
			}
		} endfor_ifa(in_dev);
	}

out:
	rcu_read_unlock();
	return found;
}

/* TODO address */
#include "nat64/mod/common/rfc6052.h"
#include "nat64/mod/common/route.h"

static int ____route4(struct packet *in, const struct tuple *tuple6,
		struct in_addr *daddr)
{
	struct ipv6hdr *hdr6 = pkt_ip6_hdr(in);
	__u8 tos;
	__u8 proto;
	struct hdr_iterator iterator;

	/* TODO differs from 6to4.c behaviour. */
	tos = RT_TOS(get_traffic_class(hdr6));
	/* TODO differs from 6to4.c behaviour. */
	hdr_iterator_init(&iterator, hdr6);
	hdr_iterator_last(&iterator);
	proto = iterator.hdr_type;

	return __route4(in, daddr->s_addr, tos, proto);
}

/**
 * Normally picks the first primary global address of @dst's interface.
 * If there's a primary global address that matches @daddr however, it takes
 * precedence.
 */
static int __pick_addr(struct dst_entry *dst, struct in_addr *daddr,
		struct in_addr *result)
{
	struct in_device *in_dev;
	__be32 saddr = 0;

	in_dev = __in_dev_get_rcu(dst->dev);
	if (!in_dev) {
		log_debug("IPv4 route doesn't involve an IPv4 device.");
		return -EINVAL;
	}

	for_primary_ifa(in_dev) {
		if (ifa->ifa_scope != RT_SCOPE_UNIVERSE)
			continue;
		if (inet_ifa_match(daddr->s_addr, ifa)) {
			result->s_addr = ifa->ifa_local;
			return 0;
		}
		if (!saddr)
			saddr = ifa->ifa_local;
	} endfor_ifa(in_dev);

	if (!saddr) {
		log_debug("Huh? IPv4 device doesn't have an IPv4 address.");
		return -ESRCH;
	}

	result->s_addr = saddr;
	return 0;
}

static int pick_addr(struct dst_entry *dst, struct in_addr *daddr,
		struct in_addr *result)
{
	int error;

	rcu_read_lock();
	error = __pick_addr(dst, daddr, result);
	rcu_read_unlock();

	return error;
}

static int select_port(struct in_addr *saddr,
		int (*func)(struct ipv4_transport_addr *, void *), void *arg,
		unsigned int offset)
{
	const unsigned int MIN = DEFAULT_POOL4_MIN_PORT;
	const unsigned int MAX = DEFAULT_POOL4_MAX_PORT;
	struct ipv4_transport_addr tmp;
	int error;

	offset = MIN + (offset % (MAX - MIN + 1));
	tmp.l3 = *saddr;

	for (tmp.l4 = offset; tmp.l4 <= MAX; tmp.l4++) {
		error = func(&tmp, arg);
		if (error)
			return error;
	}

	for (tmp.l4 = DEFAULT_POOL4_MIN_PORT; tmp.l4 < offset; tmp.l4++) {
		error = func(&tmp, arg);
		if (error)
			return error;
	}

	return 0;
}

int pool4empty_foreach_taddr4(struct packet *in, const struct tuple *tuple6,
		int (*func)(struct ipv4_transport_addr *, void *), void *arg,
		unsigned int offset)
{
	struct in_addr saddr;
	struct in_addr daddr;
	int error;

	/*
	 * TODO duplicate code.
	 * What I want is the same translation we'll later use to create the
	 * session.
	 */
	error = rfc6052_6to4(&tuple6->dst.addr6.l3, &daddr);
	if (error)
		return error;

	error = ____route4(in, tuple6, &daddr);
	if (error)
		return error;

	error = pick_addr(in->dst, &daddr, &saddr);
	if (error)
		return error;

	return select_port(&saddr, func, arg, offset);
}

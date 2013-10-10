#include "nat64/mod/filtering_and_updating.h"
#include "nat64/comm/constants.h"
#include "nat64/comm/config_proto.h"
#include "nat64/mod/rfc6052.h"
#include "nat64/mod/pool4.h"
#include "nat64/mod/pool6.h"
#include "nat64/mod/send_packet.h"

/**
 * @file
 * Second step of the stateful Nat64 translation algorithm: "Filtering and Updating Binding and
 * Session Information", as defined in RFC6146 section 3.5.
 *
 * @author Roberto Aceves
 * @author Alberto Leiva
 */

#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/icmpv6.h>
#include <net/tcp.h>
#include <net/icmp.h>

/** Current valid configuration for the filtering and updating module. */
static struct filtering_config config;
/** Synchronizes access to the "config" variable. */
static DEFINE_SPINLOCK(config_lock);

/**
 * Prepares this module for future use. Avoid calling the rest of the functions unless this has
 * already been executed once.
 *
 * @return zero on success, nonzero on failure.
 */
int filtering_init(void)
{
	spin_lock_bh(&config_lock);

	config.to.udp = UDP_DEFAULT;
	config.to.icmp = ICMP_DEFAULT;
	config.to.tcp_trans = TCP_TRANS;
	config.to.tcp_est = TCP_EST;

	config.drop_by_addr = FILT_DEF_ADDR_DEPENDENT_FILTERING;
	config.drop_external_tcp = FILT_DEF_DROP_EXTERNAL_CONNECTIONS;
	config.drop_icmp6_info = FILT_DEF_FILTER_ICMPV6_INFO;

	spin_unlock_bh(&config_lock);

	return 0;
}

/**
 * Frees any memory allocated by this module.
 */
void filtering_destroy(void)
{
	/* No code. */
}

/**
 * Copies this module's current configuration to "clone".
 *
 * @param[out] clone a copy of the current config will be placed here. Must be already allocated.
 * @return zero on success, nonzero on failure.
 */
int clone_filtering_config(struct filtering_config *clone)
{
	spin_lock_bh(&config_lock);
	*clone = config;
	spin_unlock_bh(&config_lock);

	return 0;
}

/**
 * Updates the configuration of this module.
 *
 * @param[in] operation indicator of which fields from "new_config" should be taken into account.
 * @param[in] new configuration values.
 * @return zero on success, nonzero on failure.
 */
int set_filtering_config(__u32 operation, struct filtering_config *new_config)
{
	int error = 0;

	spin_lock_bh(&config_lock);

	if (operation & DROP_BY_ADDR_MASK)
		config.drop_by_addr = new_config->drop_by_addr;
	if (operation & DROP_ICMP6_INFO_MASK)
		config.drop_icmp6_info = new_config->drop_icmp6_info;
	if (operation & DROP_EXTERNAL_TCP_MASK)
		config.drop_external_tcp = new_config->drop_external_tcp;

	if (operation & UDP_TIMEOUT_MASK) {
		if (new_config->to.udp < UDP_MIN) {
			error = -EINVAL;
			log_err(ERR_UDP_TO_RANGE, "The UDP timeout must be at least %u.", UDP_MIN);
		} else {
			config.to.udp = new_config->to.udp;
		}
	}
	if (operation & ICMP_TIMEOUT_MASK)
		config.to.icmp = new_config->to.icmp;
	if (operation & TCP_EST_TIMEOUT_MASK) {
		if (new_config->to.tcp_est < TCP_EST) {
			error = -EINVAL;
			log_err(ERR_TCPEST_TO_RANGE, "The TCP est timeout must be at least %u.", TCP_EST);
		} else {
			config.to.tcp_est = new_config->to.tcp_est;
		}
	}
	if (operation & TCP_TRANS_TIMEOUT_MASK) {
		if (new_config->to.tcp_trans < TCP_TRANS) {
			error = -EINVAL;
			log_err(ERR_TCPTRANS_TO_RANGE, "The TCP trans timeout must be at least %u.", TCP_TRANS);
		} else {
			config.to.tcp_trans = new_config->to.tcp_trans;
		}
	}

	spin_unlock_bh(&config_lock);
	return error;
}

/**
 * Use this function to safely update a session_entry's dying_time field.
 *
 * This is needed because the possible values dying_time can obtain always come from variables that
 * need to be synchronized.
 *
 * @param[out] session the structure you want to update.
 * @param[in] a pointer to the value you want to set session->dying_time to.
 */
static void update_session_lifetime(struct session_entry *session, unsigned int *timeout)
{
	unsigned int ttl;

	spin_lock_bh(&config_lock);
	ttl = *timeout;
	spin_unlock_bh(&config_lock);

	session->dying_time = jiffies_to_msecs(jiffies) + 1000 * ttl;
}

/**
 * Use this function to safely obtain the configuration value which dictates whether Jool should
 * drop all informational ICMP packets that are traveling from IPv6 to IPv4.
 *
 * @return whether Jool should drop all ICMPv6 info packets.
 */
static bool filter_icmpv6_info(void)
{
	bool result;

	spin_lock_bh(&config_lock);
	result = config.drop_icmp6_info;
	spin_unlock_bh(&config_lock);

	return result;
}

/**
 * Use this function to safely obtain the configuration value which dictates whether Jool should
 * be applying "address-dependent filtering" (Look that up in the RFC).
 *
 * @return whether Jool should apply "address-dependent filtering".
 */
static bool address_dependent_filtering(void)
{
	bool result;

	spin_lock_bh(&config_lock);
	result = config.drop_by_addr;
	spin_unlock_bh(&config_lock);

	return result;
}

/**
 * Use this function to safaly obtain the configuration value which dictates whether IPv4 nodes
 * should be allowed to initiate conversations with IPv6 nodes.
 *
 * @return whether IPv4 nodes should be allowed to initiate conversations with IPv6 nodes.
 */
static bool drop_external_connections(void)
{
	bool result;

	spin_lock_bh(&config_lock);
	result = config.drop_external_tcp;
	spin_unlock_bh(&config_lock);

	return result;
}

/**
 * Join a IPv4 address and a port (or ICMP ID) to create a transport (or tuple) address.
 *
 * @param[in] addr the address component of the transport address you want to init.
 * @param[in] l4_id port or ICMP ID component of the transport address you want to init.
 * @param[out] ta the resulting transport address. Must be already allocated.
 */
static void transport_address_ipv4(struct in_addr addr, __u16 l4_id, struct ipv4_tuple_address *ta)
{
	ta->address = addr;
	ta->l4_id = l4_id;
}

/**
 * Join a IPv6 address and a port (or ICMP ID) to create a transport (or tuple) address.
 *
 * @param[in] addr the address component of the transport address you want to init.
 * @param[in] l4_id port or ICMP ID component of the transport address you want to init.
 * @param[out] ta the resulting transport address. Must be already allocated.
 */
static void transport_address_ipv6(struct in6_addr addr, __u16 l4_id, struct ipv6_tuple_address *ta)
{
	ta->address = addr;
	ta->l4_id = l4_id;
}

/**
 * "Allocates" from the IPv4 pool a new transport address for use by the UDP BIB.
 *
 * Sorry, we're using the term "allocate" because the RFC does. A more appropriate name in this
 * context would be "borrow".
 *
 * RFC6146 - Section 3.5.1.1
 *
 * @param[in] tuple this should contain the IPv6 source address you want the IPv4 address for.
 * @param[out] result the transport address we borrowed from the pool.
 * @return true if everything went OK, false otherwise.
 */
static bool allocate_ipv4_transport_address(struct tuple *tuple, struct ipv4_tuple_address *result)
{
	struct bib_entry *bib;

	/* Check if the BIB has a previous entry from the same IPv6 source address (X’). */
	bib = bib_get_by_ipv6_only(&tuple->src.addr.ipv6, L4PROTO_UDP);

	if (bib) {
		/* Use the same IPv4 address (T). */
		struct ipv4_tuple_address temp;
		transport_address_ipv4(bib->ipv4.address, tuple->src.l4_id, &temp);
		return pool4_get_similar(L4PROTO_UDP, &temp, result);
	} else {
		/* Don't care; use any address. */
		return pool4_get_any(L4PROTO_UDP, tuple->src.l4_id, result);
	}
}

/**
 * "Allocates" from the IPv4 pool a new transport address. Attemps to make this address as similar
 * to already existing data as possible.
 *
 * Sorry, we're using the term "allocate" because the RFC does. A more appropriate name in this
 * context would be "borrow".
 *
 * RFC6146 - Section 3.5.2.3
 *
 * @param[in] tuple this should contain the IPv6 source address you want the IPv4 address for.
 * @param[in] protocol protocol of the IPv4 pool the transport address should be borrowed from.
 * @param[out] result the transport address we borrowed from the pool.
 * @return true if everything went OK, false otherwise.
 */
static bool allocate_ipv4_transport_address_digger(struct tuple *tuple, enum l4_proto protocol,
		struct ipv4_tuple_address *result)
{
	unsigned char ii = 0;
	u_int8_t proto[] = { IPPROTO_TCP, IPPROTO_UDP, IPPROTO_ICMP };
	struct in_addr *address = NULL;

	/* Look for S' in all three BIBs. */
	for (ii = 0; ii < 3; ii++) {
		struct bib_entry *bib;

		bib = bib_get_by_ipv6_only(&tuple->src.addr.ipv6, proto[ii]);
		if (bib) {
			address = &bib->ipv4.address;
			break; /* We found one entry! */
		}
	}

	if (address) {
		/* Use the same address */
		struct ipv4_tuple_address temp;
		transport_address_ipv4(*address, tuple->src.l4_id, &temp);
		return pool4_get_similar(protocol, &temp, result);
	} else {
		/* Use whichever address */
		return pool4_get_any(protocol, tuple->src.l4_id, result);
	}
}

/**
 * Returns true if frag's SYN flag is ON.
 *
 * @param[in] frag fragment you want to read the flag from.
 * @return true if frag's SYN flag is ON, false otherwise.
 */
static inline bool packet_is_syn(struct fragment* frag)
{
	struct tcphdr *hdr = frag_get_tcp_hdr(frag);
	BUG_ON(!hdr);
	return hdr->syn;
}

/**
 * Returns true if frag's FIN flag is ON.
 *
 * @param[in] frag fragment you want to read the flag from.
 * @return true if frag's FIN flag is ON, false otherwise.
 */
static inline bool packet_is_fin(struct fragment* frag)
{
	struct tcphdr *hdr = frag_get_tcp_hdr(frag);
	BUG_ON(!hdr);
	return hdr->fin;
}

/**
 * Returns true if frag's RST flag is ON.
 *
 * @param[in] frag fragment you want to read the flag from.
 * @return true if frag's RST flag is ON, false otherwise.
 */
static inline bool packet_is_rst(struct fragment* frag)
{
	struct tcphdr *hdr = frag_get_tcp_hdr(frag);
	BUG_ON(!hdr);
	return hdr->rst;
}

/**
 * Sends a probe packet to "session"'s IPv6 endpoint.
 *
 * From RFC 6146 page 30.
 *
 * @param[in] session the established session that has been inactive for too long.
 * @return true if the packet could be sent, false otherwise.
 */
static bool send_probe_packet(struct session_entry *session)
{
	struct tcphdr *th;
	struct ipv6hdr *iph;
	struct sk_buff* skb;
	struct dst_entry *dst;
	int error;

	unsigned int l3_hdr_len = sizeof(*iph);
	unsigned int l4_hdr_len = sizeof(*th);

	skb = alloc_skb(LL_MAX_HEADER + l3_hdr_len + l4_hdr_len, GFP_ATOMIC);
	if (!skb)
		return false;

	skb_reserve(skb, LL_MAX_HEADER);
	skb_put(skb, l3_hdr_len + l4_hdr_len);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, l3_hdr_len);

	iph = ipv6_hdr(skb);
	iph->version = 6;
	iph->priority = 0;
	iph->flow_lbl[0] = 0;
	iph->flow_lbl[1] = 0;
	iph->flow_lbl[2] = 0;
	iph->payload_len = l4_hdr_len;
	iph->nexthdr = IPPROTO_TCP;
	iph->hop_limit = 255;
	iph->saddr = session->ipv6.local.address;
	iph->daddr = session->ipv6.remote.address;

	th = tcp_hdr(skb);
	th->source = cpu_to_be16(session->ipv6.local.l4_id);
	th->dest = cpu_to_be16(session->ipv6.remote.l4_id);
	th->seq = htonl(0);
	th->ack_seq = htonl(0);
	th->res1 = 0;
	th->doff = l4_hdr_len / 4;
	th->fin = 0;
	th->syn = 0;
	th->rst = 0;
	th->psh = 0;
	th->ack = 1;
	th->urg = 0;
	th->ece = 0;
	th->cwr = 0;
	th->window = htons(8192);
	th->check = 0;
	th->urg_ptr = 0;

	th->check = csum_ipv6_magic(&iph->saddr, &iph->daddr, l4_hdr_len, IPPROTO_TCP,
			csum_partial(th, l4_hdr_len, 0));
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	dst = route_ipv6(iph, th, L4PROTO_TCP, 0);
	if (!dst)
		return false;
	skb->dev = dst->dev;
	skb_dst_set(skb, dst);

	error = ip6_local_out(skb);
	if (error) {
		log_err(ERR_SEND_FAILED, "The kernel's packet dispatch function returned errcode %d. "
							"Cannot send packet.", error);
		return false;
	}

	return true;
}

/**
 * Wrapper for the 6to4 function of the rfc6052 module. Extracts the prefix from "src" and returns
 * the result as a IPv4 address on "dst".
 *
 * FIXME (error) if the user configures several prefixes w/different lengths, this is going to do
 * something weird.
 *
 * @param[in] src IPv6 address you want to translate to IPv4.
 * @param[out] dst IPv4 version of "src".
 * @return true on success, false on failure.
 */
static bool extract_ipv4(struct in6_addr *src, struct in_addr *dst)
{
	struct ipv6_prefix prefix;
	if (!pool6_peek(&prefix))
		return false;

	return addr_6to4(src, &prefix, dst);
}

/**
 * Wrapper for the 4to6 function of the rfc6052 module. Adds any known prefix to "src" and returns
 * the result as a IPv6 address on "dst".
 *
 * @param[in] src IPv6 address you want to translate to IPv6.
 * @param[out] dst IPv6 version of "src".
 * @return true on success, false on failure.
 */
static bool append_ipv4(struct in_addr *src, struct in6_addr *dst)
{
	struct ipv6_prefix prefix;
	if (!pool6_peek(&prefix))
		return false;

	return addr_4to6(src, &prefix, dst);
}

/**
 * Decides whether the packet should be filtered or not. Not yet implemented.
 */
static inline void apply_policies(void)
{
	/* TODO (Issue #41) decide whether resources and policy allow filtering to continue. */
}

/**
 * Assumes that "tuple" represents a IPv6-UDP packet, and filters and updates based on it.
 *
 * This is RFC 6146 section 3.5.1, first half.
 *
 * @param[in] frag first fragment of tuple's packet. This is actually only used for error reporting.
 * @param[in] tuple summary of the packet Jool is currently translating.
 * @return VER_CONTINUE if everything went OK, VER_DROP otherwise.
 */
static enum verdict ipv6_udp(struct fragment *frag, struct tuple *tuple)
{
	struct bib_entry *bib;
	struct session_entry *session;
	struct ipv4_tuple_address bib_ipv4_addr;
	struct in_addr destination_as_ipv4;
	struct ipv6_tuple_address source;
	struct ipv4_pair pair4;
	struct ipv6_pair pair6;
	u_int8_t protocol = IPPROTO_UDP;
	bool bib_is_local = false;

	/* Pack source address into transport address */
	transport_address_ipv6(tuple->src.addr.ipv6, tuple->src.l4_id, &source);

	/* Check if a previous BIB entry exist, look for IPv6 source transport address (X’,x). */
	spin_lock_bh(&bib_session_lock);
	bib = bib_get_by_ipv6(&source, protocol);

	/* If not found, try to create a new one. */
	if (bib == NULL) {
		/* Find a similar transport address (T, t) */
		if (!allocate_ipv4_transport_address(tuple, &bib_ipv4_addr)) {
			log_warning("Could not 'allocate' a compatible transport address for the packet.");
			goto bib_failure;
		}

		/* Create the BIB entry */
		bib = bib_create(&bib_ipv4_addr, &source, false);
		if (bib == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a BIB entry.");
			goto bib_failure;
		}

		bib_is_local = true;

		apply_policies();

		/* Add the BIB entry */
		if (bib_add(bib, protocol) != 0) {
			kfree(bib);
			log_err(ERR_ADD_BIB_FAILED, "Could not add the BIB entry to the table.");
			goto bib_failure;
		}
	}

	/* Once we have a BIB entry do ... */

	session = session_get(tuple);

	/* If session was not found, then try to create a new one. */
	if (session == NULL) {
		/* Translate address */
		if (!extract_ipv4(&tuple->dst.addr.ipv6, &destination_as_ipv4)) /* Z(Y') */
		{
			log_err(ERR_EXTRACT_FAILED, "Could not translate the packet's address.");
			goto session_failure;
		}

		/* Create the session entry */
		pair6.remote.address = tuple->src.addr.ipv6; /* X' */
		pair6.remote.l4_id = tuple->src.l4_id; /* x */
		pair6.local.address = tuple->dst.addr.ipv6; /* Y' */
		pair6.local.l4_id = tuple->dst.l4_id; /* y */
		pair4.local = bib->ipv4; /* (T, t) */
		pair4.remote.address = destination_as_ipv4; /* Z or Z(Y’) */
		pair4.remote.l4_id = tuple->dst.l4_id; /* z or y */
		session = session_create(&pair4, &pair6, protocol);
		if (session == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
			goto session_failure;
		}

		apply_policies();

		/* Add the session entry */
		if (session_add(session) != 0) {
			kfree(session);
			log_err(ERR_ADD_SESSION_FAILED, "Could not add the session entry to the table.");
			goto session_failure;
		}

		/* Cross-reference them. */
		session->bib = bib;
		list_add(&session->entries_from_bib, &bib->sessions);
	}

	/* Reset session entry's lifetime. */
	update_session_lifetime(session, &config.to.udp);
	spin_unlock_bh(&bib_session_lock);

	return VER_CONTINUE;

session_failure:
	if (bib_is_local) {
		bib_remove(bib, protocol);
		pool4_return(protocol, &bib->ipv4);
		kfree(bib);
	}
	/* Fall through. */

bib_failure:
	spin_unlock_bh(&bib_session_lock);
	/* This is specified in section 3.5.1.1. */
	icmpv6_send(frag->skb, ICMPV6_DEST_UNREACH, ICMPV6_ADDR_UNREACH, 0);
	return VER_DROP;
}

/**
 * Assumes that "tuple" represents a IPv4-UDP packet, and filters and updates based on it.
 *
 * This is RFC 6146 section 3.5.1, second half.
 *
 * @param[in] frag first fragment of tuple's packet. This is actually only used for error reporting.
 * @param[in] tuple summary of the packet Jool is currently translating.
 * @return VER_CONTINUE if everything went OK, VER_DROP otherwise.
 */
static enum verdict ipv4_udp(struct fragment* frag, struct tuple *tuple)
{
	struct bib_entry *bib;
	struct session_entry *session;
	struct in6_addr source_as_ipv6;
	struct ipv4_tuple_address destination;
	struct ipv4_pair pair4;
	struct ipv6_pair pair6;
	u_int8_t protocol = IPPROTO_UDP;
	/*
	 * We don't want to call icmp_send() while the spinlock is held, so this will tell whether and
	 * what should be sent.
	 */
	int icmp_error = -1;

	/* Pack source address into transport address */
	transport_address_ipv4(tuple->dst.addr.ipv4, tuple->dst.l4_id, &destination);

	spin_lock_bh(&bib_session_lock);

	/* Check if a previous BIB entry exist, look for IPv4 destination transport address (T,t). */
	bib = bib_get_by_ipv4(&destination, protocol);
	if (bib == NULL) {
		log_warning("There is no BIB entry for the incoming IPv4 UDP packet.");
		icmp_error = ICMP_HOST_UNREACH;
		goto failure;
	}

	/* If we're applying address-dependent filtering in the IPv4 interface, */
	if (address_dependent_filtering() && !session_allow(tuple)) {
		log_info("Packet was blocked by address-dependent filtering.");
		icmp_error = ICMP_PKT_FILTERED;
		goto failure;
	}

	/* Find the Session Table Entry corresponding to the incoming tuple */
	session = session_get(tuple);

	if (session == NULL) {
		/* Translate address */
		if (!append_ipv4(&tuple->src.addr.ipv4, &source_as_ipv6)) /* Y’(W) */
		{
			log_err(ERR_APPEND_FAILED, "Could not translate the packet's address.");
			icmp_error = ICMP_HOST_UNREACH;
			goto failure;
		}

		/* Create the session entry */
		pair6.remote = bib->ipv6; /* (X', x) */
		pair6.local.address = source_as_ipv6; /* Y’(W) */
		pair6.local.l4_id = tuple->src.l4_id; /* w */
		pair4.local.address = tuple->dst.addr.ipv4; /* T */
		pair4.local.l4_id = tuple->dst.l4_id; /* t */
		pair4.remote.address = tuple->src.addr.ipv4; /* W */
		pair4.remote.l4_id = tuple->src.l4_id; /* w */
		session = session_create(&pair4, &pair6, protocol);
		if (session == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
			icmp_error = ICMP_HOST_UNREACH;
			goto failure;
		}

		apply_policies();

		/* Add the session entry */
		if (session_add(session) != 0) {
			kfree(session);
			log_err(ERR_ADD_SESSION_FAILED, "Could not add the session entry to the table.");
			icmp_error = ICMP_HOST_UNREACH;
			goto failure;
		}

		/* Cross-reference them. */
		session->bib = bib;
		list_add(&session->entries_from_bib, &bib->sessions);
	}

	/* Reset session entry's lifetime. */
	update_session_lifetime(session, &config.to.udp);
	spin_unlock_bh(&bib_session_lock);

	return VER_CONTINUE;

failure:
	spin_unlock_bh(&bib_session_lock);

	/*
	 * This is is not specified most of the time, but I assume we're supposed to do it, in order
	 * to maintain symmetry with IPv6-UDP.
	 */
	if (icmp_error != -1)
		icmp_send(frag->skb, ICMP_DEST_UNREACH, icmp_error, 0);

	return VER_DROP;
}

/**
 * Assumes that "tuple" represents a IPv6-ICMP packet, and filters and updates based on it.
 *
 * This is RFC 6146 section 3.5.3, first half.
 *
 * @param[in] frag first fragment of tuple's packet. This is actually only used for error reporting.
 * @param[in] tuple summary of the packet Jool is currently translating.
 * @return VER_CONTINUE if everything went OK, VER_DROP otherwise.
 */
static enum verdict ipv6_icmp6(struct fragment *frag, struct tuple *tuple)
{
	struct bib_entry *bib;
	struct session_entry *session;
	struct ipv4_tuple_address bib_ipv4_addr;
	struct in_addr destination_as_ipv4;
	struct ipv6_tuple_address source;
	struct ipv4_pair pair4;
	struct ipv6_pair pair6;
	u_int8_t protocol = IPPROTO_ICMP;
	bool bib_is_local = false;

	if (filter_icmpv6_info()) {
		log_info("Packet is ICMPv6 info; dropping due to policy.");
		return VER_DROP;
	}

	/* Pack source address into transport address */
	transport_address_ipv6(tuple->src.addr.ipv6, tuple->icmp_id, &source);

	/* Search for an ICMPv6 Query BIB entry that matches the (X’,i1) pair. */
	spin_lock_bh(&bib_session_lock);
	bib = bib_get_by_ipv6(&source, protocol);

	/* If not found, try to create a new one. */
	if (bib == NULL) {
		/* Look in the BIB tables for a previous packet from the same origin (X') */
		if (!allocate_ipv4_transport_address_digger(tuple, protocol, &bib_ipv4_addr)) {
			log_warning("Could not 'allocate' a compatible transport address for the packet.");
			goto bib_failure;
		}

		/* Create the BIB entry */
		bib = bib_create(&bib_ipv4_addr, &source, false);
		if (bib == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a BIB entry.");
			goto bib_failure;
		}

		bib_is_local = true;

		apply_policies();

		/* Add the new BIB entry */
		if (bib_add(bib, protocol) != 0) {
			kfree(bib);
			log_err(ERR_ADD_BIB_FAILED, "Could not add the BIB entry to the table.");
			goto bib_failure;
		}
	}

	/* OK, we have a BIB entry to work with... */

	/* Search an ICMP STE corresponding to the incoming 3-tuple (X’,Y’,i1). */
	session = session_get(tuple);

	/* If NO session was found: */
	if (session == NULL) {
		/* Translate address from IPv6 to IPv4 */
		if (!extract_ipv4(&tuple->dst.addr.ipv6, &destination_as_ipv4)) /* Z(Y') */
		{
			log_err(ERR_EXTRACT_FAILED, "Could not translate the packet's address.");
			goto session_failure;
		}

		/* Create the session entry */
		pair6.remote.address = tuple->src.addr.ipv6; /* (X') */
		pair6.remote.l4_id = tuple->icmp_id; /* (i1) */
		pair6.local.address = tuple->dst.addr.ipv6; /* (Y') */
		pair6.local.l4_id = tuple->icmp_id; /* (i1) */
		pair4.local = bib->ipv4; /* (T, i2) */
		pair4.remote.address = destination_as_ipv4; /* (Z(Y’)) */
		pair4.remote.l4_id = bib->ipv4.l4_id; /* (i2) */
		session = session_create(&pair4, &pair6, protocol);
		if (session == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
			goto session_failure;
		}

		apply_policies();

		/* Add the session entry */
		if (session_add(session) != 0) {
			kfree(session);
			log_err(ERR_ADD_SESSION_FAILED, "Could not add the session entry to the table.");
			goto session_failure;
		}

		/* Cross-reference them. */
		session->bib = bib;
		list_add(&session->entries_from_bib, &bib->sessions);
	}

	/* Reset session entry's lifetime. */
	update_session_lifetime(session, &config.to.icmp);
	spin_unlock_bh(&bib_session_lock);

	return VER_CONTINUE;

session_failure:
	if (bib_is_local) {
		bib_remove(bib, protocol);
		pool4_return(protocol, &bib->ipv4);
		kfree(bib);
	}
	/* Fall through. */

bib_failure:
	spin_unlock_bh(&bib_session_lock);
	/*
	 * This is is not specified, but I assume we're supposed to do it, since otherwise this entire
	 * thing is so similar to UDP.
	 */
	icmpv6_send(frag->skb, ICMPV6_DEST_UNREACH, ICMPV6_ADDR_UNREACH, 0);
	return VER_DROP;
}


/**
 * Assumes that "tuple" represents a IPv4-ICMP packet, and filters and updates based on it.
 *
 * This is RFC 6146 section 3.5.3, second half.
 *
 * @param[in] frag first fragment of tuple's packet. This is actually only used for error reporting.
 * @param[in] tuple summary of the packet Jool is currently translating.
 * @return VER_CONTINUE if everything went OK, VER_DROP otherwise.
 */
static enum verdict ipv4_icmp4(struct fragment* frag, struct tuple *tuple)
{
	struct bib_entry *bib;
	struct session_entry *session;
	struct in6_addr source_as_ipv6;
	struct ipv4_tuple_address destination;
	struct ipv4_pair pair4;
	struct ipv6_pair pair6;
	u_int8_t protocol = IPPROTO_ICMP;

	/*
	 * We don't want to call icmp_send() while the spinlock is held, so this will tell whether and
	 * what should be sent.
	 */
	int icmp_error = -1;

	/* Pack source address into transport address */
	transport_address_ipv4(tuple->dst.addr.ipv4, tuple->icmp_id, &destination);

	spin_lock_bh(&bib_session_lock);

	/* Find the packet's BIB entry. */
	bib = bib_get_by_ipv4(&destination, protocol);
	if (bib == NULL) {
		log_warning("There is no BIB entry for the incoming IPv4 ICMP packet.");
		icmp_error = ICMP_HOST_UNREACH;
		goto failure;
	}

	/* If we're applying address-dependent filtering in the IPv4 interface, */
	if (address_dependent_filtering() && !session_allow(tuple)) {
		log_info("Packet was blocked by address-dependent filtering.");
		icmp_error = ICMP_PKT_FILTERED;
		goto failure;
	}

	/* Search the Session Table Entry corresponding to the incoming tuple */
	session = session_get(tuple);

	if (session == NULL) {
		/* Translate the address */
		if (!append_ipv4(&tuple->src.addr.ipv4, &source_as_ipv6)) /* Y’(Z) */
		{
			log_err(ERR_APPEND_FAILED, "Could not translate the packet's address.");
			icmp_error = ICMP_HOST_UNREACH;
			goto failure;
		}

		/* Create the session entry. */
		pair6.remote = bib->ipv6; /* X', i1 */
		pair6.local.address = source_as_ipv6; /* Y'(Z) */
		pair6.local.l4_id = bib->ipv6.l4_id; /* i1 */
		pair4.local.address = tuple->dst.addr.ipv4; /* T */
		pair4.local.l4_id = tuple->icmp_id; /* i2 */
		pair4.remote.address = tuple->src.addr.ipv4; /* Z */
		pair4.remote.l4_id = tuple->icmp_id; /* i2 */
		session = session_create(&pair4, &pair6, protocol);
		if (session == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
			icmp_error = ICMP_HOST_UNREACH;
			goto failure;
		}

		apply_policies();

		/* Add the session entry */
		if (session_add(session) != 0) {
			kfree(session);
			log_err(ERR_ADD_SESSION_FAILED, "Could not add the session entry to the table.");
			icmp_error = ICMP_HOST_UNREACH;
			goto failure;
		}

		/* Cross-reference them. */
		session->bib = bib;
		list_add(&session->entries_from_bib, &bib->sessions);
	}

	/* Reset session entry's lifetime. */
	update_session_lifetime(session, &config.to.icmp);
	spin_unlock_bh(&bib_session_lock);

	return VER_CONTINUE;

failure:
	spin_unlock_bh(&bib_session_lock);

	/*
	 * Sending an ICMP error is not specified, but I assume we're supposed to do it, since
	 * otherwise this entire thing is so similar to UDP.
	 */
	if (icmp_error != -1)
		icmp_send(frag->skb, ICMP_DEST_UNREACH, icmp_error, 0);

	return VER_DROP;
}

/** The states from the TCP state machine; RFC 6146 section 3.5.2. */
enum tcp_states {
	/** No traffic has been seen; state is fictional. */
	CLOSED = 0,
	/** A SYN packet arrived from the IPv6 side; some IPv4 node is trying to start a connection. */
	V6_INIT,
	/** A SYN packet arrived from the IPv4 side; some IPv4 node is trying to start a connection. */
	V4_INIT,
	/** The handshake is complete and the sides are exchanging upper-layer data. */
	ESTABLISHED,
	/**
	 * The IPv4 node wants to terminate the connection. Data can still flow.
	 * Awaiting a IPv6 FIN...
	 */
	V4_FIN_RCV,
	/**
	 * The IPv6 node wants to terminate the connection. Data can still flow.
	 * Awaiting a IPv4 FIN...
	 */
	V6_FIN_RCV,
	/** Both sides issued a FIN. Packets can still flow for a short time. */
	V4_FIN_V6_FIN_RCV,
	/** The session might die in a short while. */
	TRANS,
};

static bool tcp_closed_v6_syn(struct fragment* frag, struct tuple *tuple)
{
	struct bib_entry *bib;
	struct session_entry *session;
	struct ipv6_tuple_address source;
	struct ipv4_tuple_address bib_ipv4_addr;
	struct in_addr destination_as_ipv4;
	struct ipv6_pair pair6;
	struct ipv4_pair pair4;
	u_int8_t protocol = IPPROTO_TCP;
	bool bib_is_local = false;

	/* Pack source address into transport address */
	transport_address_ipv6(tuple->src.addr.ipv6, tuple->src.l4_id, &source);

	/* Check if a previous BIB entry exist, look for IPv6 source transport address (X’,x). */
	bib = bib_get_by_ipv6(&source, protocol);

	/* If bib does not exist, try to create a new one, */
	if (bib == NULL) {
		/* Obtain a new BIB IPv4 transport address (T,t), put it in new_ipv4_transport_address. */
		if (!allocate_ipv4_transport_address_digger(tuple, protocol, &bib_ipv4_addr)) {
			log_warning("Could not 'allocate' a compatible transport address for the packet.");
			goto bib_failure;
		}

		/* Create the BIB entry */
		bib = bib_create(&bib_ipv4_addr, &source, false);
		if (bib == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a BIB entry.");
			goto bib_failure;
		}

		bib_is_local = true;

		apply_policies();

		/* Add the new BIB entry */
		if (bib_add(bib, protocol) != 0) {
			log_err(ERR_ADD_BIB_FAILED, "Could not add the BIB entry to the table.");
			goto bib_failure;
		}
	}

	/* Now that we have a BIB entry... */

	/* Translate address*/
	if (!extract_ipv4(&tuple->dst.addr.ipv6, &destination_as_ipv4)) { /* Z(Y') */
		log_err(ERR_EXTRACT_FAILED, "Could not translate the packet's address.");
		goto session_failure;
	}

	/* Create the session entry. */
	pair6.remote.address = tuple->src.addr.ipv6; /* X' */
	pair6.remote.l4_id = tuple->src.l4_id; /* x */
	pair6.local.address = tuple->dst.addr.ipv6; /* Y' */
	pair6.local.l4_id = tuple->dst.l4_id; /* y */
	pair4.local = bib->ipv4; /* (T, t) */
	pair4.remote.address = destination_as_ipv4; /* Z or Z(Y’) */
	pair4.remote.l4_id = tuple->dst.l4_id; /* z or y */

	session = session_create(&pair4, &pair6, protocol);
	if (session == NULL) {
		log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
		goto session_failure;
	}

	update_session_lifetime(session, &config.to.tcp_trans);
	session->state = V6_INIT;

	apply_policies();

	if (session_add(session) != 0) {
		kfree(session);
		log_err(ERR_ADD_SESSION_FAILED, "Could not add the session entry to the table.");
		goto session_failure;
	}

	/* Cross-reference them. */
	session->bib = bib;
	list_add(&session->entries_from_bib, &bib->sessions);

	return true;

session_failure:
	if (bib_is_local) {
		bib_remove(bib, protocol);
		pool4_return(protocol, &bib->ipv4);
		kfree(bib);
	}
	/* Fall through. */

bib_failure:
	/* TODO (later) We're sending this while the spinlock is held; this might be really slow. */
	icmpv6_send(frag->skb, ICMPV6_DEST_UNREACH, ICMPV6_ADDR_UNREACH, 0);
	return false;
}

static bool tcp_closed_v4_syn(struct fragment* frag, struct tuple *tuple)
{
	struct bib_entry *bib = NULL;
	struct session_entry *session = NULL;
	struct ipv4_tuple_address destination;
	struct in6_addr ipv6_local;
	struct ipv6_pair pair6;
	struct ipv4_pair pair4;
	u_int8_t protocol = IPPROTO_TCP;

	if (drop_external_connections()) {
		log_info("Applying policy: Dropping externally initiated TCP connections.");
		return false;
	}

	/* Pack addresses and ports into transport address */
	transport_address_ipv4(tuple->dst.addr.ipv4, tuple->dst.l4_id, &destination);

	/* Translate address */
	if (!append_ipv4(&tuple->src.addr.ipv4, &ipv6_local)) { /* Y'(Y) */
		log_err(ERR_APPEND_FAILED, "Could not translate the packet's address.");
		goto failure;
	}

	/* Look for the destination transport address (X,x) in the BIB */
	bib = bib_get_by_ipv4(&destination, protocol);

	if (bib == NULL) {
		/* Try to create a new session entry anyway! */
		unsigned int temp = TCP_INCOMING_SYN;

		log_warning("Unknown TCP connections started from the IPv4 side is still unsupported. "
		"Dropping packet...");
		goto failure;

		/*
		 * Side:   <-------- IPv6 -------->  N  <------- IPv4 ------->
		 * Packet: dest(X',x) <--- src(Y',y) A  dest(X,x) <-- src(Y,y)
		 * NAT64:  remote              local T  local           remote
		 */

		/* Create the session entry */
		/* pair6.remote = Not_Available; (X', x) INTENTIONALLY LEFT UNSPECIFIED! */
		pair6.local.address = ipv6_local; /* (Y', y) */
		pair6.local.l4_id = tuple->src.l4_id;
		pair4.local.address = tuple->dst.addr.ipv4; /* (X, x)  (T, t) */
		pair4.local.l4_id = tuple->dst.l4_id;
		pair4.remote.address = tuple->src.addr.ipv4; /* (Z(Y’),y) or (Z, z) */
		pair4.remote.l4_id = tuple->src.l4_id;

		session = session_create(&pair4, &pair6, protocol);
		if (session == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
			goto failure;
		}

		session->state = V4_INIT;
		update_session_lifetime(session, &temp);

		/* TODO (later) store the packet.
		 *          The result is that the NAT64 will not drop the packet based on the filtering,
		 *          nor create a BIB entry.  Instead, the NAT64 will only create the Session
		 *          Table Entry and store the packet. The motivation for this is to support
		 *          simultaneous open of TCP connections. */

	} else {

		/* BIB entry exists; create the session entry. */
		pair6.remote = bib->ipv6; /* (X', x) */
		pair6.local.address = ipv6_local; /* (Y', y) */
		pair6.local.l4_id = tuple->src.l4_id;
		pair4.local.address = tuple->dst.addr.ipv4; /* (X, x)  (T, t) */
		pair4.local.l4_id = tuple->dst.l4_id;
		pair4.remote.address = tuple->src.addr.ipv4; /* (Z(Y’),y) or (Z, z) */
		pair4.remote.l4_id = tuple->src.l4_id;

		session = session_create(&pair4, &pair6, protocol);
		if (session == NULL) {
			log_err(ERR_ALLOC_FAILED, "Failed to allocate a session entry.");
			goto failure;
		}

		session->state = V4_INIT;
		if (address_dependent_filtering()) {
			unsigned int temp = TCP_INCOMING_SYN;
			update_session_lifetime(session, &temp);
		} else {
			update_session_lifetime(session, &config.to.tcp_trans);
		}
	}

	apply_policies();

	if (session_add(session) != 0) {
		kfree(session);
		log_err(ERR_ADD_SESSION_FAILED, "Could not add the session entry to the table.");
		goto failure;
	}

	/* Cross-reference them. */
	session->bib = bib;
	list_add(&session->entries_from_bib, &bib->sessions);

	return true;

failure:
	icmp_send(frag->skb, ICMP_DEST_UNREACH, ICMP_HOST_UNREACH, 0);
	return false;
}

/**
 * CLOSED state
 *
 * Handle SYN packets.
 *
 * @param[in]   packet  The incoming packet.
 * @param[in]   tuple   Tuple of the incoming packet.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_closed_state_handle(struct fragment* frag, struct tuple *tuple)
{
	struct bib_entry *bib = NULL;
	struct ipv6_tuple_address ipv6_ta;
	struct ipv4_tuple_address ipv4_ta;
	u_int8_t protocol = IPPROTO_TCP;

	switch (frag->l3_hdr.proto) {
	case L3PROTO_IPV6:
		if (packet_is_syn(frag))
			return tcp_closed_v6_syn(frag, tuple);

		/* Pack source address into transport address */
		transport_address_ipv6(tuple->src.addr.ipv6, tuple->src.l4_id, &ipv6_ta);

		/* Look if there is a corresponding entry in the TCP BIB */
		bib = bib_get_by_ipv6(&ipv6_ta, protocol);
		if (!bib)
			log_warning("BIB entry not found for %pI6c#%u.",
					&tuple->src.addr.ipv6, tuple->src.l4_id);
		break;

	case L3PROTO_IPV4:
		if (packet_is_syn(frag))
			return tcp_closed_v4_syn(frag, tuple);

		/* Pack addresses and ports into transport address */
		transport_address_ipv4(tuple->dst.addr.ipv4, tuple->dst.l4_id, &ipv4_ta);

		/* Look for the destination transport address (X,x) in the BIB */
		bib = bib_get_by_ipv4(&ipv4_ta, protocol);
		if (!bib)
			log_warning("BIB entry not found for %pI4#%u.",
					&tuple->dst.addr.ipv4, tuple->dst.l4_id);
		break;
	}

	return (bib != NULL);
}

/**
 * V4 INIT state
 *
 * Handle IPv6 SYN packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_v4_init_state_handle(struct fragment* frag, struct session_entry *session)
{
	if (frag->l3_hdr.proto == L3PROTO_IPV6 && packet_is_syn(frag)) {
		update_session_lifetime(session, &config.to.tcp_est);
		session->state = ESTABLISHED;
	} /* else, the state remains unchanged. */

	return true;
}

/**
 * V6 INIT state.
 *
 * Handle IPv4 & IPv6 SYN packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_v6_init_state_handle(struct fragment* frag, struct session_entry *session)
{
	if (packet_is_syn(frag)) {
		switch (frag->l3_hdr.proto) {
		case L3PROTO_IPV4:
			update_session_lifetime(session, &config.to.tcp_est);
			session->state = ESTABLISHED;
			break;
		case L3PROTO_IPV6:
			update_session_lifetime(session, &config.to.tcp_trans);
			break;
		}
	} /* else, the state remains unchanged */

	return true;
}

/**
 * ESTABLISHED state.
 *
 * Handles V4 FIN, V6 FIN, V4 RST, & V6 RST packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_established_state_handle(struct fragment* frag, struct session_entry *session)
{
	if (packet_is_fin(frag)) {
		switch (frag->l3_hdr.proto) {
		case L3PROTO_IPV4:
			session->state = V4_FIN_RCV;
			break;
		case L3PROTO_IPV6:
			session->state = V6_FIN_RCV;
			break;
		}

	} else if (packet_is_rst(frag)) {
		update_session_lifetime(session, &config.to.tcp_trans);
		session->state = TRANS;

	} else {
		update_session_lifetime(session, &config.to.tcp_est);
	}

	return true;
}

/**
 * V4 FIN RCV state.
 *
 * Handles V6 FIN packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_v4_fin_rcv_state_handle(struct fragment* frag, struct session_entry *session)
{
	if (frag->l3_hdr.proto == L3PROTO_IPV6 && packet_is_fin(frag)) {
		update_session_lifetime(session, &config.to.tcp_trans);
		session->state = V4_FIN_V6_FIN_RCV;
	} else {
		update_session_lifetime(session, &config.to.tcp_est);
	}
	return true;
}

/**
 * V6 FIN RCV state.
 *
 * Handles V4 FIN packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_v6_fin_rcv_state_handle(struct fragment* frag, struct session_entry *session)
{
	if (frag->l3_hdr.proto == L3PROTO_IPV4 && packet_is_fin(frag)) {
		update_session_lifetime(session, &config.to.tcp_trans);
		session->state = V4_FIN_V6_FIN_RCV;
	} else {
		update_session_lifetime(session, &config.to.tcp_est);
	}
	return true;
}

/**
 * V6 FIN + V4 FIN RCV state.
 *
 * Handles all packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_v4_fin_v6_fin_rcv_state_handle(struct fragment *frag,
		struct session_entry *session)
{
	/* Only the timeout can change this state. */
	return true;
}

/**
 * TRANS state.
 *
 * Handles not RST packets.
 *
 * @param[in]   session   Session the packet participates in.
 * @return  true if everything went OK, false otherwise.
 */
static bool tcp_trans_state_handle(struct fragment *frag, struct session_entry *session)
{
	if (!packet_is_rst(frag)) {
		update_session_lifetime(session, &config.to.tcp_est);
		session->state = ESTABLISHED;
	}

	return true;
}

/**
 * This function should be called when "session" expires. If F&U has reasons to prevent its murder,
 * this function will update its lifetime and return true.
 *
 * @param[in]   session_entry   The entry whose lifetime just expired.
 * @return true: remove STE. false: keep STE.
 */
bool session_expired(struct session_entry *session)
{
	switch (session->l4_proto) {
	case IPPROTO_UDP:
		return false;
	case IPPROTO_ICMP:
	case IPPROTO_ICMPV6:
		return false;
	case IPPROTO_TCP:
		switch (session->state) {
		case V4_INIT:
			/* TODO (later) send the stored packet.
			 * If the lifetime expires, an ICMP Port Unreachable error (Type 3, Code 3) containing the
			 * IPv4 SYN packet stored is sent back to the source of the v4 SYN, the Session Table Entry
			 * is deleted, and the state is moved to CLOSED. */
			/* send_icmp_error_message(skb, DESTINATION_UNREACHABLE, ADDRESS_UNREACHABLE); */
			session->state = CLOSED;
			return false;
		case ESTABLISHED:
			send_probe_packet(session);
			session->state = TRANS;
			update_session_lifetime(session, &config.to.tcp_trans);
			return true;
		case V6_INIT:
		case V4_FIN_RCV:
		case V6_FIN_RCV:
		case V4_FIN_V6_FIN_RCV:
		case TRANS:
			session->state = CLOSED;
			return false;
		default:
			/*
			 * Because closed sessions are not supposed to be stored,
			 * CLOSED is known to fall through here.
			 */
			log_err(ERR_INVALID_STATE, "Invalid state found; removing session entry.");
			return false;
		}
		return false;
	default:
		log_err(ERR_L4PROTO, "Unsupported transport protocol: %u.", session->l4_proto);
		return false;
	}
}

/**
 * Assumes that "tuple" represents a TCP packet, and filters and updates based on it.
 * Encapsulates the TCP state machine.
 *
 * This is RFC 6146 section 3.5.2.
 *
 * @param[in] frag first fragment of tuple's packet.
 * @param[in] tuple summary of the packet Jool is currently translating.
 * @return VER_CONTINUE if everything went OK, VER_DROP otherwise.
 */
static enum verdict tcp(struct fragment* frag, struct tuple *tuple)
{
	struct session_entry *session;
	bool result;

	spin_lock_bh(&bib_session_lock);
	session = session_get(tuple);

	/* If NO session was found: */
	if (session == NULL) {
		result = tcp_closed_state_handle(frag, tuple);
		goto end;
	}

	/* Act according the current state. */
	switch (session->state) {
	case V4_INIT:
		result = tcp_v4_init_state_handle(frag, session);
		break;
	case V6_INIT:
		result = tcp_v6_init_state_handle(frag, session);
		break;
	case ESTABLISHED:
		result = tcp_established_state_handle(frag, session);
		break;
	case V4_FIN_RCV:
		result = tcp_v4_fin_rcv_state_handle(frag, session);
		break;
	case V6_FIN_RCV:
		result = tcp_v6_fin_rcv_state_handle(frag, session);
		break;
	case V4_FIN_V6_FIN_RCV:
		result = tcp_v4_fin_v6_fin_rcv_state_handle(frag, session);
		break;
	case TRANS:
		result = tcp_trans_state_handle(frag, session);
		break;
	default:
		/*
		 * Because closed sessions are not supposed to be stored,
		 * CLOSED is known to fall through here.
		 */
		log_err(ERR_INVALID_STATE, "Invalid state found: %u.", session->state);
		result = false;
	}
	/* Fall through. */

end:
	spin_unlock_bh(&bib_session_lock);
	return result ? VER_CONTINUE : VER_DROP;
}

/**
 * Main F&U routine. Called during the processing of every packet.
 *
 * Decides if a packet must be processed, updating binding and session information,
 * and if it may be also filtered.
 *
 * @param[in] pkt packet being translated.
 * @param[in] tuple packet's summary.
 * @return indicator of what should happen to pkt.
 */
enum verdict filtering_and_updating(struct packet* pkt, struct tuple *tuple)
{
	struct fragment *frag = pkt->first_fragment;
	struct icmp6hdr *hdr_icmp6;
	struct icmphdr *hdr_icmp4;
	enum verdict result = VER_CONTINUE;

	log_debug("Step 2: Filtering and Updating");

	switch (tuple->l3_proto) {
	case L3PROTO_IPV6:
		hdr_icmp6 = frag_get_icmp6_hdr(frag);
		/* ICMP errors should not affect the tables. */
		if (L4PROTO_ICMP == tuple->l4_proto && is_icmp6_error(hdr_icmp6->icmp6_type)) {
			log_debug("Packet is ICMPv6 info, ignoring...");
			return VER_CONTINUE;
		}
		/* Get rid of hairpinning loops and unwanted packets. */
		if (pool6_contains(&tuple->src.addr.ipv6) || !pool6_contains(&tuple->dst.addr.ipv6)) {
			log_info("Packet was rejected by pool6, dropping...");
			return VER_DROP;
		}
		break;
	case L3PROTO_IPV4:
		hdr_icmp4 = frag_get_icmp4_hdr(frag);
		/* ICMP errors should not affect the tables. */
		if (L4PROTO_ICMP == tuple->l4_proto && is_icmp4_error(hdr_icmp4->type)) {
			log_debug("Packet is ICMPv4 info, ignoring...");
			return VER_CONTINUE;
		}
		/* Get rid of unexpected packets */
		if (!pool4_contains(&tuple->dst.addr.ipv4)) {
			log_info("Packet was rejected by pool4, dropping...");
			return VER_DROP;
		}
		break;
	}

	/* Process packet, according to its protocol. */
	switch (tuple->l4_proto) {
	case L4PROTO_UDP:
		switch (tuple->l3_proto) {
		case L3PROTO_IPV6:
			result = ipv6_udp(frag, tuple);
			break;
		case L3PROTO_IPV4:
			result = ipv4_udp(frag, tuple);
			break;
		}
		break;

	case L4PROTO_TCP:
		result = tcp(frag, tuple);
		break;

	case L4PROTO_ICMP:
		switch (tuple->l3_proto) {
		case L3PROTO_IPV6:
			result = ipv6_icmp6(frag, tuple);
			break;
		case L3PROTO_IPV4:
			result = ipv4_icmp4(frag, tuple);
			break;
		}
		break;

	case L4PROTO_NONE:
		log_err(ERR_L4PROTO, "Tuples should not contain the 'NONE' transport protocol.");
		result = VER_DROP;
		break;
	}

	log_debug("Done: Step 2.");
	return result;
}

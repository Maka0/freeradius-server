/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License, version 2 of the
 *   License as published by the Free Software Foundation.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file net.c
 * @brief Functions to parse raw packets.
 *
 * @author Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 * @copyright 2014-2015 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
#include <freeradius-devel/libradius.h>
#include <freeradius-devel/net.h>

/** Strings for L4 protocols
 *
 */
FR_NAME_NUMBER const fr_net_ip_proto_table[] = {
	{ "UDP",	IPPROTO_UDP },
	{ "TCP",	IPPROTO_TCP },
	{ NULL, 0 }
};

/** Strings for socket types
 *
 */
FR_NAME_NUMBER const fr_net_sock_type_table[] = {
	{ "UDP",	SOCK_DGRAM },
	{ "TCP",	SOCK_STREAM },
	{ NULL, 0 }
};

/** Strings for address families
 *
 */
FR_NAME_NUMBER const fr_net_af_table[] = {
	{ "IPv4",	AF_INET },
	{ "IPv6",	AF_INET6 },
	{ NULL, 0 }
};

/** Check whether fr_link_layer_offset can process a link_layer
 *
 * @param link_layer to check.
 * @return
 *	- true if supported.
 *	- false if not supported.
 */
bool fr_link_layer_supported(int link_layer)
{
	switch (link_layer) {
	case DLT_EN10MB:
	case DLT_RAW:
	case DLT_NULL:
	case DLT_LOOP:
	case DLT_LINUX_SLL:
	case DLT_PFLOG:
		return true;

	default:
		return false;
	}
}

/** Returns the length of the link layer header
 *
 * Libpcap does not include a decoding function to skip the L2 header, but it does
 * at least inform us of the type.
 *
 * Unfortunately some headers are of variable length (like ethernet), so additional
 * decoding logic is required.
 *
 * @note No header data is returned, this is only meant to be used to determine how
 * data to consume before attempting to parse the IP header.
 *
 * @param data start of packet data.
 * @param len caplen.
 * @param link_layer value returned from pcap_linktype.
 * @return
 *	- Length of the header.
 *	- -1 on failure.
 */
ssize_t fr_link_layer_offset(uint8_t const *data, size_t len, int link_layer)
{
	uint8_t const *p = data;

	switch (link_layer) {
	case DLT_RAW:
		break;

	case DLT_NULL:
	case DLT_LOOP:
		p += 4;
		if (((size_t)(p - data)) > len) {
		ood:
			fr_strerror_printf("Out of data, needed %zu bytes, have %zu bytes",
					   (size_t)(p - data), len);
			return -1;
		}
		break;

	case DLT_EN10MB:
	{
		uint16_t ether_type;	/* Ethernet type */
		int i;

		p += 12;		/* SRC/DST Mac-Addresses */
		if (((size_t)(p - data)) > len) {
			goto ood;
		}

		for (i = 0; i < 3; i++) {
			ether_type = ntohs(*((uint16_t const *) p));
			switch (ether_type) {
			/*
			 *	There are a number of devices out there which
			 *	double tag with 0x8100 *sigh*
			 */
			case 0x8100:	/* CVLAN */
			case 0x9100:	/* SVLAN */
			case 0x9200:	/* SVLAN */
			case 0x9300:	/* SVLAN */
				p += 4;
				if (((size_t)(p - data)) > len) {
					goto ood;
				}
				break;

			default:
				p += 2;
				if (((size_t)(p - data)) > len) {
					goto ood;
				}
				goto done;
			}
		}
		fr_strerror_printf("Exceeded maximum level of VLAN tag nesting (2)");
		return -1;
	}

	case DLT_LINUX_SLL:
		p += 16;
		if (((size_t)(p - data)) > len) {
			goto ood;
		}
		break;

	case DLT_PFLOG:
		p += 28;
		if (((size_t)(p - data)) > len) {
			goto ood;
		}
		break;

	default:
		fr_strerror_printf("Unsupported link layer type %i", link_layer);
	}

done:
	return p - data;
}

/** Check UDP header is valid
 *
 * @param data Pointer to the start of the UDP header
 * @param remaining bits of received packet
 * @param ip pointer to IP header structure
 * @return
 *	- 1 if checksum is incorrect.
 *	- 0 if UDP payload lenght and checksum are correct
 *	- -1 on validation error.
 */
 int fr_udp_header_check(uint8_t const *data, uint16_t remaining, ip_header_t const * ip)
 {
	int ret = 0;
	udp_header_t const	*udp;

	/*
	 *	UDP header validation.
	 */
	udp = (udp_header_t const *)data;
	uint16_t udp_len;
	ssize_t diff;
	uint16_t expected;

	udp_len = ntohs(udp->len);
	diff = udp_len - remaining;
	/* Truncated data */
	if (diff > 0) {
		fr_strerror_printf("packet too small by %zi bytes, UDP header + Payload should be %hu bytes",
				   diff, udp_len);
		return -1;
	}
	/* Trailing data */
	else if (diff < 0) {
		fr_strerror_printf("Packet too big by %zi bytes, UDP header + Payload should be %hu bytes",
				   diff * -1, udp_len);
		return -1;
	}

	expected = fr_udp_checksum((uint8_t const *) udp, ntohs(udp->len), udp->checksum,
				   ip->ip_src, ip->ip_dst);
	if (udp->checksum != expected) {
		fr_strerror_printf("DHCP: UDP checksum invalid, packet: 0x%04hx calculated: 0x%04hx",
				   ntohs(udp->checksum), ntohs(expected));
		/* Not a fatal error */
		ret = 1;
	}

	return ret;
 }

/** Calculate UDP checksum
 *
 * Zero out UDP checksum in UDP header before calling #fr_udp_checksum to get 'expected' checksum.
 *
 * @param data Pointer to the start of the UDP header
 * @param len value of udp length field in host byte order. Must be validated to make
 *	  sure it won't overrun data buffer.
 * @param checksum current checksum, leave as 0 to just enable validation.
 * @param src_addr in network byte order.
 * @param dst_addr in network byte order.
 * @return
 *	- 0 if the checksum is correct.
 *	- !0 if checksum is incorrect.
 */
uint16_t fr_udp_checksum(uint8_t const *data, uint16_t len, uint16_t checksum,
			 struct in_addr const src_addr, struct in_addr const dst_addr)
{
	uint64_t sum = 0;	/* using 64bits avoids overflow check */
	uint16_t const *p = (uint16_t const *)data;

	uint16_t const *ip_src = (void const *) &src_addr.s_addr;
	uint16_t const *ip_dst = (void const *) &dst_addr.s_addr;
	uint16_t i;

	sum += *(ip_src++);
	sum += *ip_src;
	sum += *(ip_dst++);
	sum += *ip_dst;

	sum += htons(IPPROTO_UDP);
	sum += htons(len);

	for (i = len; i > 1; i -= 2) sum += *p++;
	if (i) sum += (0xff & *(uint8_t const *)p) << 8;

	sum -= checksum;

	while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);

	return ((uint16_t) ~sum);
}

/** Calculate IP header checksum.
 *
 * Zero out IP header checksum in IP header before calling fr_ip_header_checksum to get 'expected' checksum.
 *
 * @param data Pointer to the start of the IP header
 * @param ihl value of ip header length field (number of 32 bit words)
 */
uint16_t fr_ip_header_checksum(uint8_t const *data, uint8_t ihl)
{
	uint64_t sum = 0;
	uint16_t const *p = (uint16_t const *)data;

	uint8_t nwords = (ihl << 1); /* number of 16-bit words */

	for (sum = 0; nwords > 0; nwords--) {
		sum += *p++;
	}
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return ((uint16_t) ~sum);
}

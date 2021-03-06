/*
 * Part of `snmp-query-engine`.
 *
 * Copyright 2012-2014, Anton Berezin <tobez@tobez.org>
 * Modified BSD license.
 * (See LICENSE file in the distribution.)
 *
 */
#include "sqe.h"

static struct socket_info *snmp = NULL;

static void
snmp_process_datagram(struct socket_info *snmp, struct sockaddr_in *from, char *buf, int n)
{
	struct ber enc, *e;
	unsigned char t;
	unsigned l;
	unsigned sid;
	char *trace;
	struct destination *dest;
	struct sid_info *si;

	PS.octets_received += n;
	dest = find_destination(&from->sin_addr, ntohs(from->sin_port));
	if (!dest) {
		fprintf(stderr, "destination %s:%d is not knowing, ignoring packet\n", inet_ntoa(from->sin_addr), ntohs(from->sin_port));
		return;
	}
	dest->octets_received += n;
	dest->packets_on_the_wire--;
	if (dest->packets_on_the_wire < 0)
		dest->packets_on_the_wire = 0;
	PS.packets_on_the_wire--;
	if (PS.packets_on_the_wire < 0)
		PS.packets_on_the_wire = 0;
// fprintf(stderr, "%s: snmp_receive->(%d)\n", inet_ntoa(dest->ip), dest->packets_on_the_wire);

	enc = ber_init(buf, n); e = &enc;

	#define CHECK(prob, val) if ((val) < 0) { trace = "decoding" # prob; goto bad_snmp_packet; }
	CHECK("start sequence", decode_sequence(e, NULL));
	CHECK("version", decode_integer(e, -1, NULL));

	trace = "community type/len";
	if (decode_type_len(e, &t, &l) < 0)	goto bad_snmp_packet;
	trace = "community type";
	if (t != AT_STRING)	goto bad_snmp_packet;
	e->b += l;  e->len += l;  // XXX skip community

	CHECK("PDU", decode_composite(e, PDU_GET_RESPONSE, NULL));
	CHECK("decoding request id", decode_integer(e, -1, &sid));
	#undef CHECK

	si = find_sid_info(dest, sid);
	if (!si) {
		fprintf(stderr, "unable to find sid_info with sid %u, ignoring packet\n", sid);
		return;
	}

//	fprintf(stderr, "this packet appears to be legit, sid %u(%u)\n", sid, si->sid);
	if (process_sid_info_response(si, e))
		free_sid_info(si);
	maybe_query_destination(dest);

	return;

bad_snmp_packet:
	PS.bad_snmp_responses++;
	fprintf(stderr, "bad SNMP packet, ignoring: %s\n", trace);
	maybe_query_destination(dest);
}

static void
snmp_receive(struct socket_info *snmp)
{
	struct sockaddr_in from;
	socklen_t len;
	char buf[65000];
	int n;

	while (1) {
		len = sizeof(from);
		if ( (n = recvfrom(snmp->fd, buf, 65000, 0, (struct sockaddr *)&from, &len)) < 0) {
			if (errno == EAGAIN)
				return;
			croak(1, "snmp_receive: recvfrom");
		}
		snmp_process_datagram(snmp, &from, buf, n);
	}
}

void
create_snmp_socket(void)
{
	int fd;
	int n;
	int flags;

	if (snmp)
		croakx(1, "create_snmp_socket: socket already exists");

	fd = socket(PF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		croak(1, "create_snmp_socket: socket");

	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		croak(1, "create_snmp_socket: fcntl(getfl)");
	flags |= O_NONBLOCK;
	if (fcntl(fd, F_SETFL, flags) < 0)
		croak(1, "create_snmp_socket: fcntl(setfl)");

	/* try a very large receive buffer size */
	n = 100 * 1024 * 1024;
	while (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n)) < 0)
		n /= 2;
	PS.udp_receive_buffer_size = n;

	/* additionally, try a very large send buffer size :) */
	n = 100 * 1024 * 1024;
	while (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n)) < 0)
		n /= 2;
	PS.udp_send_buffer_size = n;

	snmp = new_socket_info(fd);
	on_read(snmp, snmp_receive);
}

void snmp_send(struct destination *dest, struct ber *packet)
{
	destination_start_timing(dest);

	dest->packets_on_the_wire++;
	PS.packets_on_the_wire++;
//fprintf(stderr, "%s: snmp_send->(%d)\n", inet_ntoa(dest->ip), dest->packets_on_the_wire);
	if (sendto(snmp->fd, packet->buf, packet->len, 0,
			   (struct sockaddr *)&dest->dest_addr,
			   sizeof(dest->dest_addr))
		!= packet->len)
	{
		if (errno == EAGAIN) {
			PS.udp_send_buffer_overflow++;
			return;
		}
		croak(1, "snmp_send: sendto");
	}
//fprintf(stderr, "UDP datagram of %d bytes sent to %s:%d\n", packet->len, inet_ntoa(dest->dest_addr.sin_addr), ntohs(dest->dest_addr.sin_port));
//dump_buf(stderr, packet->buf, packet->len);

	dest->octets_sent += packet->len;
	PS.octets_sent += packet->len;
}


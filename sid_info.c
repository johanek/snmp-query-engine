#include "sqe.h"

struct sid_info *
new_sid_info(struct client_requests_info *cri)
{
	struct sid_info *si, **si_slot;
	unsigned sid;
	struct destination *dest;

	sid = next_sid();
	dest = cri->dest;
	JLI(si_slot, dest->sid_info, sid);
	if (si_slot == PJERR)
		croak(2, "new_sid_info: JLI(sid_info) failed");
	if (*si_slot)
		croak(2, "new_sid_info: sid_info must not be there");
	si = malloc(sizeof(*si));
	if (!si)
		croak(2, "new_sid_info: malloc(sid_info)");
	bzero(si, sizeof(*si));
	si->sid = sid;
	si->cri = cri;
	si->retries_left = dest->retries;
	TAILQ_INIT(&si->oids_being_queried);
	if (start_snmp_packet(&si->pb, dest->version, dest->community, sid) < 0)
		croak(2, "new_sid_info: start_snmp_get_packet");
	*si_slot = si;
	TAILQ_INSERT_TAIL(&cri->sid_infos, si, sid_list);
	return si;
}

struct sid_info *
find_sid_info(struct destination *dest, unsigned sid)
{
	struct sid_info **si_slot;

	JLG(si_slot, dest->sid_info, sid);
	if (si_slot == PJERR || !si_slot)	return NULL;
	return *si_slot;
}

void
build_snmp_query(struct client_requests_info *cri)
{
	struct oid_info *oi, *oi_temp;
	struct sid_info *si = NULL;
	struct destination *dest;
	struct cid_info *ci;

	if (TAILQ_EMPTY(&cri->oids_to_query))	return; /* XXX */

	dest = cri->dest;
	si = new_sid_info(cri);

	oi = TAILQ_FIRST(&cri->oids_to_query);
	if (oi->last_known_table_entry) {
		/* GETTABLE */
		if (add_encoded_oid_to_snmp_packet(&si->pb, &oi->last_known_table_entry->oid) < 0)
			croak(2, "build_snmp_query: add_encoded_oid_to_snmp_packet");
		TAILQ_REMOVE(&cri->oids_to_query, oi, oid_list);
		oi->sid = si->sid;
		ci = get_cid_info(cri, oi->cid);
		if (!ci || ci->n_oids == 0)
			croakx(2, "build_snmp_query: cid_info unexpectedly missing");
		ci->n_oids_being_queried++;

		si->table_oid = oi;
		PS.oids_requested++;
		cri->si->PS.oids_requested++;

		if ( (si->sid_offset_in_a_packet =
			  finalize_snmp_packet(&si->pb, &si->packet,
								   dest->version == 0 ? PDU_GET_NEXT_REQUEST : PDU_GET_BULK_REQUEST,
								   10)) < 0)
			croak(2, "build_snmp_query: finalize_snmp_packet");

		fprintf(stderr, "see gettable packet we are sending (sid %u):\n", si->sid);
		ber_dump(stderr, &si->packet);
	} else {
		TAILQ_FOREACH_SAFE(oi, &cri->oids_to_query, oid_list, oi_temp) {
			if (oi->last_known_table_entry) continue; /* Skip GETTABLE requests */
			if (si->pb.e.len + oi->oid.len >= dest->max_request_packet_size)
				break;
			PS.oids_requested++;
			cri->si->PS.oids_requested++;
			if (add_encoded_oid_to_snmp_packet(&si->pb, &oi->oid) < 0)
				croak(2, "build_snmp_query: add_encoded_oid_to_snmp_packet");
			TAILQ_REMOVE(&cri->oids_to_query, oi, oid_list);
			oi->sid = si->sid;
			ci = get_cid_info(cri, oi->cid);
			if (!ci || ci->n_oids == 0)
				croakx(2, "build_snmp_query: cid_info unexpectedly missing");
			ci->n_oids_being_queried++;
			TAILQ_INSERT_TAIL(&si->oids_being_queried, oi, oid_list);
		}
		if ( (si->sid_offset_in_a_packet = finalize_snmp_packet(&si->pb, &si->packet, PDU_GET_REQUEST, 0)) < 0)
			croak(2, "build_snmp_query: finalize_snmp_packet");
	}
	sid_start_timing(si);
	si->retries_left--;

	PS.snmp_sends++;
	si->cri->si->PS.snmp_sends++;
	snmp_send(dest, &si->packet);
}

static void *by_time = NULL;

void
sid_start_timing(struct sid_info *si)
{
	void **sec_slot;
	struct sid_info_head **usec_slot, *list;
	struct timeval timeout;

	gettimeofday(&si->will_timeout_at, NULL);
	timeout.tv_sec = si->cri->dest->timeout / 1000;
	timeout.tv_usec = 1000 * (si->cri->dest->timeout % 1000);
	timeout.tv_usec += si->will_timeout_at.tv_usec;
	timeout.tv_sec  += si->will_timeout_at.tv_sec;
	timeout.tv_sec  += timeout.tv_usec / 1000000;
	timeout.tv_usec %= 1000000;
	si->will_timeout_at = timeout;

	JLI(sec_slot, by_time, si->will_timeout_at.tv_sec);
	if (sec_slot == PJERR)
		croak(2, "sid_start_timing: JLI(by_time) failed");
	JLI(usec_slot, *sec_slot, si->will_timeout_at.tv_usec);
	if (usec_slot == PJERR)
		croak(2, "sid_start_timing: JLI(*sec_slot) failed");
	if (!*usec_slot) {
		list = malloc(sizeof(*list));
		if (!list)
			croak(2, "sid_start_timing: malloc(sid_info_head)");
		TAILQ_INIT(list);
		*usec_slot = list;
	}
	list = *usec_slot;
	TAILQ_INSERT_TAIL(list, si, same_timeout);
}

void
sid_stop_timing(struct sid_info *si)
{
	void **sec_slot;
	struct sid_info_head **usec_slot, *list;
	Word_t rc;
	struct timeval tv;

	tv = si->will_timeout_at;
	bzero(&si->will_timeout_at, sizeof(si->will_timeout_at));
	JLG(sec_slot, by_time, tv.tv_sec);
	if (sec_slot == PJERR)
		croak(2, "sid_stop_timing: JLG(by_time) failed");
	if (!sec_slot) return;

	JLG(usec_slot, *sec_slot, tv.tv_usec);
	if (usec_slot == PJERR)
		croak(2, "sid_stop_timing: JLG(*sec_slot) failed");
	if (!usec_slot) return;
	list = *usec_slot;
	if (!list) {
cleanup:
		JLD(rc, *sec_slot, tv.tv_usec);
		fprintf(stderr, "sid_stop_timing: no or empty sid list, deleting usec %u at sec %u\n",
				(unsigned)tv.tv_usec, (unsigned)tv.tv_sec);
		JLG(sec_slot, by_time, tv.tv_sec);
		if (sec_slot == PJERR)
			croak(2, "sid_stop_timing: 2: JLG(by_time) failed");
		if (!sec_slot) return;
		if (!*sec_slot) {
			JLD(rc, by_time, tv.tv_sec);
			fprintf(stderr, "sid_stop_timing: empty by_type slot at sec %u, deleting\n",
					(unsigned)tv.tv_sec);
		}
		return;
	}
	TAILQ_REMOVE(list, si, same_timeout);
	if (TAILQ_EMPTY(list)) {
		free(list);
		goto cleanup;
	}
	return;
}

int
sid_next_timeout(void)
{
	Word_t sec, usec;
	void **sec_slot;
	struct sid_info_head **usec_slot;
	struct timeval now;

	gettimeofday(&now, NULL);
	sec = 0;
	JLF(sec_slot, by_time, sec);
	while (sec_slot) {
		usec = 0;
		JLF(usec_slot, *sec_slot, usec);
		while (usec_slot) {
			if (*usec_slot && !TAILQ_EMPTY(*usec_slot)) {
				if (now.tv_sec > sec) {
					return 0;
				} else if (now.tv_sec == sec) {
					if (now.tv_usec > usec)
						return 0;
					else
						return (usec - now.tv_usec)/1000;
				} else {
					if (sec - now.tv_sec > 5)
						return 5000;
					return 1000*(sec - now.tv_sec) + ((int)usec - (int)now.tv_usec)/1000;
				}
			}
			fprintf(stderr, "BUG: empty usec %u for sec %u\n", (unsigned)usec, (unsigned)sec);
			JLN(usec_slot, *sec_slot, usec);
		}
		fprintf(stderr, "BUG: empty sec %u\n", (unsigned)sec);
		JLN(sec_slot, by_time, sec);
	}
	return 5000;
}

struct sid_info*
get_timed_out_sid_info(void)
{
	Word_t sec, usec;
	void **sec_slot;
	struct sid_info_head **usec_slot;
	struct timeval now;

	gettimeofday(&now, NULL);
	sec = 0;
	JLF(sec_slot, by_time, sec);
	while (sec_slot) {
		usec = 0;
		JLF(usec_slot, *sec_slot, usec);
		while (usec_slot) {
			if (*usec_slot && !TAILQ_EMPTY(*usec_slot)) {
				if (now.tv_sec > sec) {
					return TAILQ_FIRST(*usec_slot);
				} else if (now.tv_sec == sec) {
					if (now.tv_usec >= usec)
						return TAILQ_FIRST(*usec_slot);
					else
						return NULL;
				} else {
					return NULL;
				}
			}
			fprintf(stderr, "BUG: empty usec %u for sec %u\n", (unsigned)usec, (unsigned)sec);
			JLN(usec_slot, *sec_slot, usec);
		}
		fprintf(stderr, "BUG: empty sec %u\n", (unsigned)sec);
		JLN(sec_slot, by_time, sec);
	}
	return NULL;
}

void
free_sid_info(struct sid_info *si)
{
	Word_t rc;
	/* The equivalent of
	 * TAILQ_REMOVE(&si->cri->sid_infos, si, sid_list);
	 * should be done by the caller.
	 * Reason: free_client_request_info() does it more
	 * efficiently and thus does not need to TAILQ_REMOVE.
	 */
fprintf(stderr, "   freeing sid_info %u\n", si->sid);
	JLD(rc, si->cri->dest->sid_info, si->sid);
	sid_stop_timing(si);
	free(si->packet.buf);
	free_oid_info_list(&si->oids_being_queried);
	free(si);
}

void resend_query_with_new_sid(struct sid_info *si)
{
	struct sid_info **si_slot;
	struct oid_info *oi;
	Word_t rc;

	JLD(rc, si->cri->dest->sid_info, si->sid);
	si->sid = next_sid();
	si->packet.buf[si->sid_offset_in_a_packet+0] = (si->sid >> 24) & 0xff;
	si->packet.buf[si->sid_offset_in_a_packet+1] = (si->sid >> 16) & 0xff;
	si->packet.buf[si->sid_offset_in_a_packet+2] = (si->sid >> 8) & 0xff;
	si->packet.buf[si->sid_offset_in_a_packet+3] = si->sid & 0xff;

	TAILQ_FOREACH(oi, &si->oids_being_queried, oid_list) {
		oi->sid = si->sid;
	}

	JLI(si_slot, si->cri->dest->sid_info, si->sid);
	if (si_slot == PJERR)
		croak(2, "resend_query_with_new_sid: JLI(sid_info) failed");
	if (*si_slot)
		croak(2, "resend_query_with_new_sid: sid_info must not be there");
	*si_slot = si;

	// fprintf(stderr, "see packet we are resending (sid %u, retries left %d):\n", si->sid, si->retries_left);
	// ber_dump(stderr, &si->packet);
	sid_start_timing(si);
	si->retries_left--;

	PS.snmp_sends++;
	si->cri->si->PS.snmp_sends++;
	PS.snmp_retries++;
	si->cri->si->PS.snmp_retries++;
	snmp_send(si->cri->dest, &si->packet);
}

void
check_timed_out_requests(void)
{
	struct sid_info *si;
	while ( (si = get_timed_out_sid_info())) {
		sid_stop_timing(si);
		if (si->retries_left > 0) {
			resend_query_with_new_sid(si);
			continue;
		}
		fprintf(stderr, "sid %u is timed out, cleaning up\n", si->sid);
		all_oids_done(si, &BER_TIMEOUT);
		TAILQ_REMOVE(&si->cri->sid_infos, si, sid_list);
		free_sid_info(si);
	}
}

void
oid_done(struct sid_info *si, struct oid_info *oi, struct ber *val)
{
	struct client_requests_info *cri;
	struct cid_info *ci;

	cri = si->cri;
	ci = get_cid_info(cri, oi->cid);
	if (!ci || ci->n_oids == 0)
		croakx(2, "oid_done: cid_info unexpectedly missing");
	/* XXX free old value? */
	oi->value = ber_rewind(ber_dup(val));
	oi->sid = 0;
	TAILQ_REMOVE(&si->oids_being_queried, oi, oid_list);
	TAILQ_INSERT_TAIL(&ci->oids_done, oi, oid_list);
	ci->n_oids_being_queried--;
	ci->n_oids_done++;
	if (ci->n_oids_done == ci->n_oids)
		cid_reply(ci, RT_GET);
}

void
got_table_oid(struct sid_info *si, struct oid_info *table_oi, struct ber *oid, struct ber *val)
{
	struct client_requests_info *cri;
	struct cid_info *ci;
	struct oid_info *oi;

fprintf(stderr, "MEOW\n");
	cri = si->cri;
	ci = get_cid_info(cri, table_oi->cid);
	if (!ci || ci->n_oids == 0)
		croakx(2, "got_table_oid: cid_info unexpectedly missing");

	oi = malloc(sizeof(*oi));
	if (!oi)
		croak(2, "got_table_oid: malloc(oid_info)");
	bzero(oi, sizeof(*oi));
	oi->cid = table_oi->cid;
	oi->fd  = ci->fd;
	oi->oid = ber_dup(oid);
	oi->value = ber_rewind(ber_dup(val));
	oi->sid = 0;
	table_oi->last_known_table_entry = oi;

	TAILQ_INSERT_TAIL(&ci->oids_done, oi, oid_list);
	ci->n_oids_done++;
	ci->n_oids++;
}

void
all_oids_done(struct sid_info *si, struct ber *val)
{
	struct oid_info *oi, *oi_temp;

	/* XXX handle si->table_oid stuff as well */
	TAILQ_FOREACH_SAFE(oi, &si->oids_being_queried, oid_list, oi_temp) {
		oid_done(si, oi, val);
	}
}

void
process_sid_info_response(struct sid_info *si, struct ber *e)
{
	unsigned error_status;
	unsigned error_index;
	char *trace;
	int oids_stop;
	struct ber oid, val;
	struct oid_info *oi;
	int table_done = 0;
	struct cid_info *ci;

	/* SNMP packet must be positioned past request id field */
	fprintf(stderr, "GOT packet\n");
	ber_dump(stderr, e);

	#define CHECK(prob, val) if ((val) < 0) { trace = prob; goto bad_snmp_packet; }
	CHECK("decoding error status", decode_integer(e, -1, &error_status));
	CHECK("decoding error index", decode_integer(e, -1, &error_index));
	CHECK("oids sequence", decode_sequence(e, &oids_stop));
	while (inside_sequence(e, oids_stop)) {
		CHECK("bindvar", decode_sequence(e, NULL));
		CHECK("oid", decode_oid(e, &oid));
		CHECK("value", decode_any(e, &val));
		if (si->table_oid) {
			if (oid_belongs_to_table(&oid, &si->table_oid->oid)) {
				got_table_oid(si, si->table_oid, &oid, &val);
			} else {
				table_done = 1;
			}
		} else {
			TAILQ_FOREACH(oi, &si->oids_being_queried, oid_list) {
				if (ber_equal(&oid, &oi->oid)) {
					oid_done(si, oi, &val);
					break;
				}
			}
		}
	}
	if (si->table_oid) {
		ci = get_cid_info(si->cri, si->table_oid->cid);
		ci->n_oids_being_queried--;
		if (table_done) {
			si->table_oid = NULL;
			fprintf(stderr, "TABLE IS DONE!\n");
			ci->n_oids--;
			fprintf(stderr, "done table, stats: N%d, Q%d, D%d\n", ci->n_oids, ci->n_oids_being_queried, ci->n_oids_done);
			if (ci->n_oids_done == ci->n_oids)
				cid_reply(ci, RT_GETTABLE);
		} else {
			TAILQ_INSERT_TAIL(&si->cri->oids_to_query, si->table_oid, oid_list);
			si->table_oid = NULL;
			maybe_query_destination(si->cri->dest);
		}
	} else {
		if (!TAILQ_EMPTY(&si->oids_being_queried)) {
			fprintf(stderr, "SID %u: unexpectedly, not all oids are accounted for!\n", si->sid);
			all_oids_done(si, &BER_MISSING);
		}
	}
	#undef CHECK

	return;
bad_snmp_packet:
	fprintf(stderr, "sid %u: bad SNMP packet, ignoring: %s\n", si->sid, trace);
}

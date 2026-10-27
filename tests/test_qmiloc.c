/* Host test for qmiloc.c: a fake LOC service behind stubbed socket calls.
 * Build: gcc -std=gnu11 -Iqmi tests/test_qmiloc.c -lm; run: ./a.out [XTRA file] (default: synthetic data) */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <linux/qrtr.h>

static ssize_t t_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tl);
static ssize_t t_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fl);
static int t_poll(struct pollfd *p, nfds_t n, int timeout);
static int t_socket(int d, int t, int p) { (void)d; (void)t; (void)p; return 77; }
static int t_getsockname(int fd, struct sockaddr *a, socklen_t *l)
{
	struct sockaddr_qrtr *q = (void *)a;
	(void)fd;
	memset(q, 0, sizeof(*q));
	q->sq_family = 42;
	q->sq_node = 1;
	q->sq_port = 0x4000;
	*l = sizeof(*q);
	return 0;
}
static int t_close(int fd) { (void)fd; return 0; }
#define sendto t_sendto
#define recvfrom t_recvfrom
#define poll t_poll
#define socket t_socket
#define getsockname t_getsockname
#define close t_close
#include "qmiloc.c"
#undef sendto
#undef recvfrom
#undef poll
#undef socket
#undef getsockname
#undef close

/* queue of packets for the client */
struct pkt { uint8_t b[4096]; size_t n; uint32_t node, port; };
static struct pkt queue[256];
static int qh, qt;
static int fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fail++; } } while (0)

static void push(uint32_t node, uint32_t port, const void *b, size_t n)
{
	struct pkt *p = &queue[qt++ % 256];

	memcpy(p->b, b, n);
	p->n = n;
	p->node = node;
	p->port = port;
}

/* fake modem state */
static uint8_t xtra_rx[200000];
static size_t xtra_rx_len;
static int xtra_parts_seen;
static int reject_a7;
static uint32_t expect_total;

static void put_tlv(uint8_t *b, size_t *n, uint8_t t, const void *v, uint16_t l)
{
	b[*n] = t;
	b[*n + 1] = l & 0xff;
	b[*n + 2] = l >> 8;
	memcpy(b + *n + 3, v, l);
	*n += 3 + l;
}

static void reply(uint8_t type, uint16_t txn, uint16_t id, const uint8_t *tlvs, size_t tl)
{
	uint8_t b[4096];
	struct qmi_header h = { type, txn, id, (uint16_t)tl };

	memcpy(b, &h, 7);
	memcpy(b + 7, tlvs, tl);
	push(1, 99, b, 7 + tl);
}

static ssize_t t_sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tl)
{
	const struct sockaddr_qrtr *dst = (const void *)to;
	struct qmi_header h;
	const uint8_t *tlvs = (const uint8_t *)buf + 7;
	uint8_t r[4096], ind[4096];
	size_t rn = 0, in = 0;
	uint16_t res[2] = { 0, 0 };
	uint32_t st = 0;
	int send_ind = 1;
	const uint8_t *v;
	int l;

	(void)fd; (void)flags; (void)tl;
	if (dst->sq_port == QRTR_PORT_CTRL) {
		struct qrtr_ctrl_pkt c;

		memcpy(&c, buf, sizeof(c));
		CHECK(c.cmd == QRTR_TYPE_NEW_LOOKUP && c.server.service == 16);
		memset(&c, 0, sizeof(c));
		c.cmd = QRTR_TYPE_NEW_SERVER;
		c.server.service = 16;
		c.server.instance = 2;
		c.server.node = 1;
		c.server.port = 99;
		push(1, QRTR_PORT_CTRL, &c, sizeof(c));
		memset(&c, 0, sizeof(c));
		c.cmd = QRTR_TYPE_NEW_SERVER;
		push(1, QRTR_PORT_CTRL, &c, sizeof(c));
		return len;
	}
	CHECK(dst->sq_node == 1 && dst->sq_port == 99);
	memcpy(&h, buf, 7);
	CHECK(h.type == 0 && h.len == len - 7);

	switch (h.msg_id) {
	case 0x1E: {	/* supported messages */
		uint8_t bm[2 + 32] = { 32, 0 };

		bm[2 + 0x22 / 8] |= 1 << (0x22 % 8);
		if (!reject_a7)
			bm[2 + 0xA7 / 8] |= 1 << (0xA7 % 8);
		bm[2 + 0xB0 / 8] |= 1 << (0xB0 % 8);
		bm[2 + 0x35 / 8] |= 1 << (0x35 % 8);
		put_tlv(r, &rn, 0x02, res, 4);
		put_tlv(r, &rn, 0x10, bm, sizeof(bm));
		reply(2, h.txn, h.msg_id, r, rn);
		return len;
	}
	case 0xA7:
	case 0x35: {
		uint32_t total;
		uint16_t parts, num, cnt;
		const uint8_t *d;

		if (h.msg_id == 0xA7 && reject_a7) {
			uint16_t e[2] = { 1, 0x39 };

			put_tlv(r, &rn, 0x02, e, 4);
			reply(2, h.txn, h.msg_id, r, rn);
			return len;
		}
		CHECK(qmiloc_tlv_find(tlvs, h.len, 0x01, &v) == 4);
		memcpy(&total, v, 4);
		CHECK(qmiloc_tlv_find(tlvs, h.len, 0x02, &v) == 2);
		memcpy(&parts, v, 2);
		CHECK(qmiloc_tlv_find(tlvs, h.len, 0x03, &v) == 2);
		memcpy(&num, v, 2);
		l = qmiloc_tlv_find(tlvs, h.len, 0x04, &d);
		CHECK(l >= 2);
		memcpy(&cnt, d, 2);
		CHECK(cnt == l - 2);
		CHECK(cnt <= 1024);
		CHECK(qmiloc_tlv_find(tlvs, h.len, 0x10, &v) == 4);
		CHECK(num == xtra_parts_seen + 1);
		CHECK(total == expect_total);
		memcpy(xtra_rx + xtra_rx_len, d + 2, cnt);
		xtra_rx_len += cnt;
		xtra_parts_seen++;
		put_tlv(r, &rn, 0x02, res, 4);
		reply(2, h.txn, h.msg_id, r, rn);
		put_tlv(ind, &in, 0x01, &st, 4);
		put_tlv(ind, &in, 0x10, &num, 2);
		if (num == parts) {
			uint64_t c = 0x3f;

			put_tlv(ind, &in, 0x11, &c, 8);
		}
		/* an unrelated indication first, as a busy modem would */
		{
			uint8_t e[16];
			size_t en = 0;
			uint32_t on = 1;

			put_tlv(e, &en, 0x01, &on, 4);
			reply(4, 0, 0x2B, e, en);
		}
		reply(4, 0, h.msg_id, ind, in);
		return len;
	}
	case 0xB0: {
		uint8_t a[600];
		size_t an = 0;
		uint32_t mask = 0x3f, delay = 100, rem = 7000;
		uint16_t abs = 120, pref = 24;
		uint64_t rel = 1761000000000ULL;
		const char *s1 = "https://path1.xtracloud.net/xtra3Mgrbeji.bin";
		const char *s2 = "https://path2.xtracloud.net/xtra3Mgrbeji.bin";
		const char *n1 = "time.xtracloud.net";

		memcpy(a + an, &mask, 4); an += 4;
		memcpy(a + an, &abs, 2); an += 2;
		memcpy(a + an, &rel, 8); an += 8;
		a[an++] = 2;
		a[an++] = strlen(s1); memcpy(a + an, s1, strlen(s1)); an += strlen(s1);
		a[an++] = strlen(s2); memcpy(a + an, s2, strlen(s2)); an += strlen(s2);
		memcpy(a + an, &delay, 4); an += 4;
		a[an++] = 1;
		a[an++] = strlen(n1); memcpy(a + an, n1, strlen(n1)); an += strlen(n1);
		a[an++] = 1;
		memcpy(a + an, &pref, 2); an += 2;
		put_tlv(r, &rn, 0x02, res, 4);
		reply(2, h.txn, h.msg_id, r, rn);
		put_tlv(ind, &in, 0x01, &st, 4);
		put_tlv(ind, &in, 0x02, a, an);
		put_tlv(ind, &in, 0x19, &rem, 4);
		reply(4, 0, h.msg_id, ind, in);
		return len;
	}
	case 0x3B: {
		uint32_t lock = 4;

		put_tlv(r, &rn, 0x02, res, 4);
		reply(2, h.txn, h.msg_id, r, rn);
		put_tlv(ind, &in, 0x01, &st, 4);
		put_tlv(ind, &in, 0x10, &lock, 4);
		reply(4, 0, h.msg_id, ind, in);
		return len;
	}
	case 0x21: case 0x22: case 0x23: case 0x20:
		send_ind = 0;
		/* fallthrough */
	default:
		put_tlv(r, &rn, 0x02, res, 4);
		reply(2, h.txn, h.msg_id, r, rn);
		if (send_ind) {
			put_tlv(ind, &in, 0x01, &st, 4);
			reply(4, 0, h.msg_id, ind, in);
		}
		return len;
	}
}

static ssize_t t_recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fl)
{
	struct sockaddr_qrtr *f = (void *)from;
	struct pkt *p;

	(void)fd; (void)flags;
	if (qh == qt) {
		errno = EAGAIN;
		return -1;
	}
	p = &queue[qh++ % 256];
	memcpy(buf, p->b, p->n < len ? p->n : len);
	f->sq_family = 42;
	f->sq_node = p->node;
	f->sq_port = p->port;
	*fl = sizeof(*f);
	return p->n;
}

static int t_poll(struct pollfd *p, nfds_t n, int timeout)
{
	(void)n; (void)timeout;
	p->revents = qh != qt ? POLLIN : 0;
	return qh != qt;
}

static int inds, svc_ups;
static void on_ind(void *u, uint16_t id, const uint8_t *t, size_t l) { (void)u; (void)t; (void)l; if (id == 0x2B) inds++; }
static void on_svc(void *u, int up) { (void)u; if (up) svc_ups++; }

int main(int argc, char **argv)
{
	struct qmiloc q;
	uint8_t bm[1024];
	size_t bl = sizeof(bm), n;
	uint32_t lock = 0;
	uint64_t cons = 0;
	static uint8_t data[200000];
	struct qmiloc_xtra_info xi;
	FILE *f;
	int rc;

	CHECK(qmiloc_open(&q, on_ind, on_svc, NULL) == 0);
	CHECK(qmiloc_wait_service(&q, 100) == 0);
	CHECK(q.up && q.node == 1 && q.port == 99 && svc_ups == 1);
	CHECK(qmiloc_inform_client_revision(&q, 2) == 0);
	CHECK(qmiloc_register_master_client(&q, 0xBAABCDEF) == 0);
	CHECK(qmiloc_get_supported_msgs(&q, bm, &bl) == 0);
	CHECK(bl == 32 && qmiloc_msg_supported(bm, bl, 0xA7) && qmiloc_msg_supported(bm, bl, 0x22) &&
	      !qmiloc_msg_supported(bm, bl, 0x23));
	CHECK(qmiloc_get_engine_lock(&q, &lock) == 0 && lock == 4);
	CHECK(qmiloc_set_engine_lock(&q, 1) == 0);
	CHECK(qmiloc_reg_events(&q, 0x1ff, 1) == 0);
	CHECK(qmiloc_start(&q, 1, 1000, 0) == 0);
	CHECK(qmiloc_inject_utc_time(&q, 1761000000000ULL, 100) == 0);
	CHECK(qmiloc_inject_position(&q, 10.5, 20.25, 5000, 68, 2, 6, 1761000000000ULL) == 0);

	/* XTRA: a real file when one is given, else a synthetic blob (not a multiple of the part size) */
	if (argc > 1) {
		f = fopen(argv[1], "rb");
		if (!f) {
			perror("xtra file");
			return 2;
		}
		n = fread(data, 1, sizeof(data), f);
		fclose(f);
	} else {
		uint32_t x = 0x12345678;

		n = 40 * 1024 + 123;
		for (size_t i = 0; i < n; i++) {
			x = x * 1103515245u + 12345u;
			data[i] = x >> 24;
		}
	}
	expect_total = n;
	rc = qmiloc_inject_xtra(&q, data, n, 0, 0, &cons);
	printf("inject xtra: %s, %d parts, %zu bytes, constellations 0x%llx, interleaved inds %d\n",
	       qmiloc_strerror(rc), xtra_parts_seen, xtra_rx_len, (unsigned long long)cons, inds);
	CHECK(rc == 0 && xtra_rx_len == n && !memcmp(xtra_rx, data, n) && cons == 0x3f &&
	      xtra_parts_seen == (int)((n + 1023) / 1024) && inds == xtra_parts_seen);
	/* the newer message rejected: the QMI error comes back */
	reject_a7 = 1;
	xtra_parts_seen = 0;
	xtra_rx_len = 0;
	rc = qmiloc_inject_xtra(&q, data, n, 0, 0, &cons);
	CHECK(QMILOC_IS_QMI_ERR(rc) && (rc & 0xffff) == 0x39);
	printf("rejected: %s\n", qmiloc_strerror(rc));
	rc = qmiloc_inject_xtra(&q, data, n, 512, 1, &cons);
	CHECK(rc == 0 && xtra_parts_seen == (int)((n + 511) / 512) && xtra_rx_len == n &&
	      !memcmp(xtra_rx, data, n));

	CHECK(qmiloc_query_xtra_info(&q, 0, &xi) == 0);
	printf("xtra info: mask 0x%x abs %u rel %llu servers %d [%s] [%s] ntp %d [%s] treq %u pref %u rem %u\n",
	       xi.report_mask, xi.abs_age_hrs, (unsigned long long)xi.rel_age_utc_ms, xi.n_servers,
	       xi.servers[0], xi.servers[1], xi.n_ntp, xi.ntp[0], xi.time_request, xi.pref_valid_age_hrs,
	       xi.rem_valid_min);
	CHECK(xi.abs_age_hrs == 120 && xi.n_servers == 2 && xi.n_ntp == 1 && xi.time_request == 1 &&
	      xi.pref_valid_age_hrs == 24 && xi.has_rem_valid && xi.rem_valid_min == 7000);
	CHECK(!strcmp(xi.servers[1], "https://path2.xtracloud.net/xtra3Mgrbeji.bin") &&
	      !strcmp(xi.ntp[0], "time.xtracloud.net"));

	/* position report */
	{
		uint8_t b[512];
		size_t bn = 0;
		uint32_t st = 0, tech = 1;
		double la = 10.5012, lo = 20.2534;  /* synthetic */
		float unc = 12.5f, alt = 40.f, msl = 20.f, spd = 1.5f;
		uint64_t utc = 1761000001000ULL;
		uint8_t used[1 + 2 * 3] = { 3 };
		uint16_t ids[3] = { 5, 70, 130 };
		struct qmiloc_position p;

		memcpy(used + 1, ids, 6);
		put_tlv(b, &bn, 0x01, &st, 4);
		put_tlv(b, &bn, 0x10, &la, 8);
		put_tlv(b, &bn, 0x11, &lo, 8);
		put_tlv(b, &bn, 0x12, &unc, 4);
		put_tlv(b, &bn, 0x18, &spd, 4);
		put_tlv(b, &bn, 0x1A, &alt, 4);
		put_tlv(b, &bn, 0x1B, &msl, 4);
		put_tlv(b, &bn, 0x23, &tech, 4);
		put_tlv(b, &bn, 0x25, &utc, 8);
		put_tlv(b, &bn, 0x2C, used, sizeof(used));
		CHECK(qmiloc_parse_position(b, bn, &p) == 0);
		CHECK(p.has_latlon && p.latitude == la && p.longitude == lo && p.has_hor_unc &&
		      p.hor_unc == unc && p.has_alt_msl && p.alt_msl == 20.f);
		CHECK(p.n_used == 3 && p.used[1] == 70 && qmiloc_used_prn(p.used[2]) == 43 &&
		      p.tech_mask == 1 && p.utc_ms == utc && p.has_speed_h);
	}
	/* satellites: two entries in 0x10 */
	{
		uint8_t b[256], a[1 + 28 * 2], alt = 0;
		size_t bn = 0;
		struct qmiloc_sv sv[4];

		a[0] = 2;
		for (int i = 0; i < 2; i++) {
			uint8_t *e = a + 1 + 28 * i;
			uint32_t vm = 0xff, sys = i ? 5 : 1, svst = 3;
			uint16_t id = i ? 7 : 12;
			float el = 45, az = 180, snr = 33.5f;

			memcpy(e, &vm, 4);
			memcpy(e + 4, &sys, 4);
			memcpy(e + 8, &id, 2);
			e[10] = 1;
			memcpy(e + 11, &svst, 4);
			e[15] = 3;
			memcpy(e + 16, &el, 4);
			memcpy(e + 20, &az, 4);
			memcpy(e + 24, &snr, 4);
		}
		put_tlv(b, &bn, 0x01, &alt, 1);
		put_tlv(b, &bn, 0x10, a, sizeof(a));
		CHECK(qmiloc_parse_sv_info(b, bn, sv, 4) == 2);
		CHECK(qmiloc_sv_prn(&sv[0]) == 12 && qmiloc_sv_prn(&sv[1]) == 71 && sv[1].snr == 33.5f &&
		      sv[1].status == 3 && sv[0].info == 3);
	}
	/* NMEA with and without a length prefix */
	{
		uint8_t b[128], pre[64];
		size_t bn = 0;
		char out[128];
		const char *s = "$GPGGA,1,2*00\r\n";

		put_tlv(b, &bn, 0x01, s, strlen(s));
		CHECK(qmiloc_parse_nmea(b, bn, out, sizeof(out)) && !strcmp(out, "$GPGGA,1,2*00"));
		pre[0] = strlen(s);
		memcpy(pre + 1, s, strlen(s));
		bn = 0;
		put_tlv(b, &bn, 0x01, pre, strlen(s) + 1);
		CHECK(qmiloc_parse_nmea(b, bn, out, sizeof(out)) && !strcmp(out, "$GPGGA,1,2*00"));
	}
	/* the service going away */
	{
		struct qrtr_ctrl_pkt c;

		memset(&c, 0, sizeof(c));
		c.cmd = QRTR_TYPE_DEL_SERVER;
		c.server.service = 16;
		c.server.node = 1;
		c.server.port = 99;
		push(1, QRTR_PORT_CTRL, &c, sizeof(c));
		qmiloc_dispatch(&q);
		CHECK(!q.up);
		CHECK(qmiloc_start(&q, 1, 1000, 0) == -ENOTCONN);
	}
	printf("%s (%d failures)\n", fail ? "FAILED" : "all tests passed", fail);
	return fail != 0;
}

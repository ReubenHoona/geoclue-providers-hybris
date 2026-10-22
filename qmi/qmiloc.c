/*
 * qmiloc: client for the Qualcomm QMI location service over QRTR.
 * See qmiloc.h.
 *
 * Copyright (c) 2026. This file is part of geoclue-hybris and may be used
 * under the GNU Lesser General Public License version 2.1.
 */
#include "qmiloc.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/qrtr.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

enum { QMI_REQUEST = 0, QMI_RESPONSE = 2, QMI_INDICATION = 4 };

struct qmi_header {
	uint8_t type;
	uint16_t txn;
	uint16_t msg_id;
	uint16_t len;
} __attribute__((packed));

#define HDR_LEN ((size_t)sizeof(struct qmi_header))

static uint8_t rx_buf[65536];

static uint16_t get_u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t get_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t get_u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static float get_float(const uint8_t *p) { float v; memcpy(&v, p, 4); return v; }
static double get_double(const uint8_t *p) { double v; memcpy(&v, p, 8); return v; }

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_service(struct qmiloc *q, int up, uint32_t node, uint32_t port)
{
	if (q->up && (!up || node != q->node || port != q->port)) {
		q->up = 0;
		if (q->on_svc)
			q->on_svc(q->user, 0);
	}
	if (up && !q->up) {
		q->node = node;
		q->port = port;
		q->up = 1;
		if (q->on_svc)
			q->on_svc(q->user, 1);
	}
}

int qmiloc_open(struct qmiloc *q, qmiloc_ind_cb on_ind, qmiloc_svc_cb on_svc, void *user)
{
	struct sockaddr_qrtr local, ctrl;
	struct qrtr_ctrl_pkt pkt;
	socklen_t len = sizeof(local);

	memset(q, 0, sizeof(*q));
	q->fd = -1;
	q->txn = 1;
	q->timeout_ms = 3000;
	q->on_ind = on_ind;
	q->on_svc = on_svc;
	q->user = user;

	q->fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (q->fd < 0)
		return -errno;
	if (getsockname(q->fd, (struct sockaddr *)&local, &len) < 0)
		goto fail;

	/* A lookup stays registered: the name server reports the current
	 * servers, an empty entry, and later NEW/DEL_SERVER changes */
	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
	pkt.server.service = QMILOC_SERVICE;
	pkt.server.instance = 0;	/* any version and instance */
	memset(&ctrl, 0, sizeof(ctrl));
	ctrl.sq_family = AF_QIPCRTR;
	ctrl.sq_node = local.sq_node;
	ctrl.sq_port = QRTR_PORT_CTRL;
	if (sendto(q->fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&ctrl, sizeof(ctrl)) < 0)
		goto fail;
	return 0;

fail:
	{
		int err = -errno;

		close(q->fd);
		q->fd = -1;
		return err;
	}
}

void qmiloc_close(struct qmiloc *q)
{
	if (q->fd >= 0)
		close(q->fd);
	q->fd = -1;
	q->up = 0;
}

static void handle_ctrl(struct qmiloc *q, const uint8_t *buf, size_t n)
{
	struct qrtr_ctrl_pkt pkt;

	if (n < sizeof(pkt))
		return;
	memcpy(&pkt, buf, sizeof(pkt));
	switch (pkt.cmd) {
	case QRTR_TYPE_NEW_SERVER:
		if (pkt.server.service == QMILOC_SERVICE)
			set_service(q, 1, pkt.server.node, pkt.server.port);
		break;
	case QRTR_TYPE_DEL_SERVER:
		if (pkt.server.service == QMILOC_SERVICE && q->up &&
		    pkt.server.node == q->node && pkt.server.port == q->port)
			set_service(q, 0, 0, 0);
		break;
	default:
		break;
	}
}

static void handle_packet(struct qmiloc *q, const uint8_t *buf, size_t n,
			  const struct sockaddr_qrtr *from)
{
	struct qmi_header hdr;
	const uint8_t *body;

	if (from->sq_port == QRTR_PORT_CTRL) {
		handle_ctrl(q, buf, n);
		return;
	}
	if (!q->up || from->sq_node != q->node || from->sq_port != q->port)
		return;
	if (n < HDR_LEN)
		return;
	memcpy(&hdr, buf, HDR_LEN);
	if (HDR_LEN + hdr.len > n)
		return;
	body = buf + HDR_LEN;

	if (hdr.type == QMI_RESPONSE) {
		if (q->want_resp_id && hdr.txn == q->want_resp_txn && hdr.msg_id == q->want_resp_id &&
		    hdr.len <= sizeof(q->resp_buf)) {
			memcpy(q->resp_buf, body, hdr.len);
			q->resp_len = hdr.len;
			q->resp_got = 1;
		}
	} else if (hdr.type == QMI_INDICATION) {
		if (q->want_ind && hdr.msg_id == q->want_ind && hdr.len <= sizeof(q->ind_buf)) {
			memcpy(q->ind_buf, body, hdr.len);
			q->ind_len = hdr.len;
			q->ind_got = 1;
		} else if (q->on_ind) {
			q->on_ind(q->user, hdr.msg_id, body, hdr.len);
		}
	}
}

int qmiloc_dispatch(struct qmiloc *q)
{
	int handled = 0;

	if (q->fd < 0)
		return -EBADF;
	for (;;) {
		struct sockaddr_qrtr from;
		socklen_t flen = sizeof(from);
		ssize_t r = recvfrom(q->fd, rx_buf, sizeof(rx_buf), MSG_DONTWAIT | MSG_TRUNC,
				     (struct sockaddr *)&from, &flen);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			/* e.g. ENETRESET after the remote node went away */
			if (handled)
				break;
			return -errno;
		}
		handled++;
		if ((size_t)r > sizeof(rx_buf))
			continue;	/* truncated: drop */
		handle_packet(q, rx_buf, r, &from);
	}
	return handled;
}

/* Pump the socket until *FLAG is set, the service goes away or the
 * deadline passes */
static int pump_until(struct qmiloc *q, int *flag, int64_t deadline)
{
	while (!*flag) {
		struct pollfd pfd = { .fd = q->fd, .events = POLLIN };
		int64_t left = deadline - now_ms();
		int r;

		if (left <= 0)
			return -ETIMEDOUT;
		r = poll(&pfd, 1, (int)left);
		if (r < 0 && errno != EINTR)
			return -errno;
		if (r > 0 && qmiloc_dispatch(q) < 0 && !*flag)
			return -EIO;
		if (!q->up && !*flag)
			return -ENOTCONN;
	}
	return 0;
}

int qmiloc_wait_service(struct qmiloc *q, int timeout_ms)
{
	int64_t deadline = now_ms() + timeout_ms;

	if (q->fd < 0)
		return -EBADF;
	qmiloc_dispatch(q);
	while (!q->up) {
		struct pollfd pfd = { .fd = q->fd, .events = POLLIN };
		int64_t left = deadline - now_ms();

		if (left <= 0)
			return -ETIMEDOUT;
		if (poll(&pfd, 1, (int)left) > 0)
			qmiloc_dispatch(q);
	}
	return 0;
}

void qmiloc_req_init(struct qmiloc_req *m, uint16_t msg_id)
{
	struct qmi_header hdr = { QMI_REQUEST, 0, msg_id, 0 };

	memcpy(m->buf, &hdr, HDR_LEN);
	m->len = HDR_LEN;
	m->overflow = 0;
}

void qmiloc_req_tlv(struct qmiloc_req *m, uint8_t type, const void *data, size_t len)
{
	if (m->len + 3 + len > sizeof(m->buf) || len > 0xffff) {
		m->overflow = 1;
		return;
	}
	m->buf[m->len] = type;
	m->buf[m->len + 1] = len & 0xff;
	m->buf[m->len + 2] = len >> 8;
	memcpy(m->buf + m->len + 3, data, len);
	m->len += 3 + len;
}

void qmiloc_req_u8(struct qmiloc_req *m, uint8_t type, uint8_t v) { qmiloc_req_tlv(m, type, &v, 1); }
void qmiloc_req_u16(struct qmiloc_req *m, uint8_t type, uint16_t v) { qmiloc_req_tlv(m, type, &v, 2); }
void qmiloc_req_u32(struct qmiloc_req *m, uint8_t type, uint32_t v) { qmiloc_req_tlv(m, type, &v, 4); }
void qmiloc_req_u64(struct qmiloc_req *m, uint8_t type, uint64_t v) { qmiloc_req_tlv(m, type, &v, 8); }
void qmiloc_req_float(struct qmiloc_req *m, uint8_t type, float v) { qmiloc_req_tlv(m, type, &v, 4); }
void qmiloc_req_double(struct qmiloc_req *m, uint8_t type, double v) { qmiloc_req_tlv(m, type, &v, 8); }

int qmiloc_request(struct qmiloc *q, struct qmiloc_req *m, uint8_t *resp, size_t *resp_len)
{
	struct qmi_header hdr;
	struct sockaddr_qrtr to;
	const uint8_t *v;
	int rc;

	if (q->fd < 0)
		return -EBADF;
	if (!q->up)
		return -ENOTCONN;
	if (m->overflow)
		return -EMSGSIZE;

	memcpy(&hdr, m->buf, HDR_LEN);
	hdr.txn = q->txn++;
	if (!q->txn)
		q->txn = 1;
	hdr.len = m->len - HDR_LEN;
	memcpy(m->buf, &hdr, HDR_LEN);

	memset(&to, 0, sizeof(to));
	to.sq_family = AF_QIPCRTR;
	to.sq_node = q->node;
	to.sq_port = q->port;

	q->want_resp_txn = hdr.txn;
	q->want_resp_id = hdr.msg_id;
	q->resp_got = 0;
	if (sendto(q->fd, m->buf, m->len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
		rc = -errno;
		q->want_resp_id = 0;
		return rc;
	}
	rc = pump_until(q, &q->resp_got, now_ms() + q->timeout_ms);
	q->want_resp_id = 0;
	if (rc)
		return rc;

	/* Result TLV: result u16 (0 success), error u16 */
	if (qmiloc_tlv_find(q->resp_buf, q->resp_len, 0x02, &v) == 4 && get_u16(v))
		return QMILOC_RC_QMI(get_u16(v + 2));
	if (resp && resp_len) {
		size_t n = q->resp_len < *resp_len ? q->resp_len : *resp_len;

		memcpy(resp, q->resp_buf, n);
		*resp_len = n;
	}
	return 0;
}

int qmiloc_request_ind(struct qmiloc *q, struct qmiloc_req *m)
{
	struct qmi_header hdr;
	uint32_t status;
	int rc;

	memcpy(&hdr, m->buf, HDR_LEN);
	q->want_ind = hdr.msg_id;
	q->ind_got = 0;
	q->ind_len = 0;
	rc = qmiloc_request(q, m, NULL, NULL);
	if (!rc)
		rc = pump_until(q, &q->ind_got, now_ms() + q->timeout_ms);
	q->want_ind = 0;
	if (rc)
		return rc;
	if (!qmiloc_tlv_u32(q->ind_buf, q->ind_len, 0x01, &status) && status)
		return QMILOC_RC_STATUS(status);
	return 0;
}

const char *qmiloc_strerror(int rc)
{
	static const char *const status_names[] = {
		"success", "general failure", "unsupported", "invalid parameter",
		"engine busy", "phone offline", "timeout", "configuration not supported",
		"insufficient memory", "max geofences programmed", "XTRA version check failure",
		"GNSS disabled",
	};
	static char buf[64];

	if (rc == 0)
		return "success";
	if (QMILOC_IS_QMI_ERR(rc)) {
		int err = rc & 0xffff;

		snprintf(buf, sizeof(buf), "QMI error 0x%04x%s", err,
			 err == QMI_ERR_INVALID_MESSAGE_ID ? " (message not supported)" :
			 err == QMI_ERR_NOT_SUPPORTED ? " (not supported)" :
			 err == QMI_ERR_INVALID_ARG ? " (invalid argument)" : "");
		return buf;
	}
	if (QMILOC_IS_STATUS(rc)) {
		unsigned int st = rc & 0xffff;

		snprintf(buf, sizeof(buf), "status %u (%s)", st,
			 st < sizeof(status_names) / sizeof(status_names[0]) ? status_names[st] : "?");
		return buf;
	}
	if (rc == -ETIMEDOUT)
		return "no answer from the modem";
	if (rc == -ENOTCONN)
		return "location service not available";
	return strerror(-rc);
}

int qmiloc_tlv_find(const uint8_t *tlvs, size_t len, uint8_t type, const uint8_t **val)
{
	const uint8_t *p = tlvs, *end = tlvs + len;

	while (p + 3 <= end) {
		uint16_t l = p[1] | p[2] << 8;

		if (p + 3 + l > end)
			break;
		if (p[0] == type) {
			*val = p + 3;
			return l;
		}
		p += 3 + l;
	}
	return -1;
}

int qmiloc_tlv_u32(const uint8_t *tlvs, size_t len, uint8_t type, uint32_t *v)
{
	const uint8_t *p;

	if (qmiloc_tlv_find(tlvs, len, type, &p) != 4)
		return -1;
	*v = get_u32(p);
	return 0;
}

/* Used-SV list: u8 count, then u16 ids */
static int parse_used(const uint8_t *v, int l, struct qmiloc_position *p)
{
	int n;

	if (l < 1)
		return -1;
	n = v[0];
	if (1 + 2 * n > l)
		n = (l - 1) / 2;
	if (n > QMILOC_MAX_USED)
		n = QMILOC_MAX_USED;
	for (int i = 0; i < n; i++)
		p->used[i] = get_u16(v + 1 + 2 * i);
	p->n_used = n;
	return 0;
}

int qmiloc_parse_position(const uint8_t *tlvs, size_t len, struct qmiloc_position *p)
{
	const uint8_t *t = tlvs, *end = tlvs + len;
	int have_expanded_used = 0;

	memset(p, 0, sizeof(*p));
	p->session_status = 0xffffffff;
	while (t + 3 <= end) {
		uint8_t type = t[0];
		int l = t[1] | t[2] << 8;
		const uint8_t *v = t + 3;

		if (t + 3 + l > end)
			break;
		t += 3 + l;
		switch (type) {
		case 0x01: if (l == 4) p->session_status = get_u32(v); break;
		case 0x02: if (l == 1) p->session_id = v[0]; break;
		case 0x10: if (l == 8) { p->latitude = get_double(v); p->has_latlon |= 1; } break;
		case 0x11: if (l == 8) { p->longitude = get_double(v); p->has_latlon |= 2; } break;
		case 0x12: if (l == 4) { p->hor_unc = get_float(v); p->has_hor_unc = 1; } break;
		case 0x13: case 0x14:	/* ellipse axes, used if there is no circle */
			if (l == 4 && !p->has_hor_unc) {
				float a = get_float(v);

				if (a > p->hor_unc)
					p->hor_unc = a;
			}
			break;
		case 0x16: if (l == 1) p->hor_conf = v[0]; break;
		case 0x18: if (l == 4) { p->speed_h = get_float(v); p->has_speed_h = 1; } break;
		case 0x1A: if (l == 4) { p->alt_ell = get_float(v); p->has_alt_ell = 1; } break;
		case 0x1B: if (l == 4) { p->alt_msl = get_float(v); p->has_alt_msl = 1; } break;
		case 0x1C: if (l == 4) { p->vert_unc = get_float(v); p->has_vert_unc = 1; } break;
		case 0x1F: if (l == 4) { p->speed_v = get_float(v); p->has_speed_v = 1; } break;
		case 0x20: if (l == 4) { p->heading = get_float(v); p->has_heading = 1; } break;
		case 0x22: if (l == 4) { p->mag_dev = get_float(v); p->has_mag_dev = 1; } break;
		case 0x23: if (l == 4) p->tech_mask = get_u32(v); break;
		case 0x24:
			if (l == 12) {
				p->pdop = get_float(v);
				p->hdop = get_float(v + 4);
				p->vdop = get_float(v + 8);
				p->has_dop = 1;
			}
			break;
		case 0x25: if (l == 8) { p->utc_ms = get_u64(v); p->has_utc = 1; } break;
		case 0x2C:
			if (!have_expanded_used)
				parse_used(v, l, p);
			break;
		case 0x2D: if (l == 1) p->alt_assumed = v[0]; break;
		case 0x36:
			if (!parse_used(v, l, p))
				have_expanded_used = 1;
			break;
		default:
			break;
		}
	}
	/* an ellipse-only uncertainty counts as given */
	if (!p->has_hor_unc && p->hor_unc > 0)
		p->has_hor_unc = 1;
	p->has_latlon = p->has_latlon == 3;
	return p->session_status == 0xffffffff ? -1 : 0;
}

/* validMask u32, system u32, svId u16, health u8, status u32, infoMask u8,
 * elevation, azimuth, snr (floats): 28 bytes; the expanded list adds the
 * GLONASS frequency (u8) */
static void parse_sv(const uint8_t *e, struct qmiloc_sv *sv)
{
	sv->valid = get_u32(e);
	sv->system = get_u32(e + 4);
	sv->svid = get_u16(e + 8);
	sv->health = e[10];
	sv->status = get_u32(e + 11);
	sv->info = e[15];
	sv->elevation = get_float(e + 16);
	sv->azimuth = get_float(e + 20);
	sv->snr = get_float(e + 24);
}

int qmiloc_parse_sv_info(const uint8_t *tlvs, size_t len, struct qmiloc_sv *out, int max)
{
	const uint8_t *v;
	int l, n, size = 29;

	l = qmiloc_tlv_find(tlvs, len, 0x11, &v);
	if (l < 1) {
		l = qmiloc_tlv_find(tlvs, len, 0x10, &v);
		size = 28;
	}
	if (l < 1)
		return qmiloc_tlv_find(tlvs, len, 0x01, &v) >= 0 ? 0 : -1;
	n = v[0];
	if (1 + n * size > l)
		n = (l - 1) / size;
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		parse_sv(v + 1 + i * size, &out[i]);
	return n;
}

int qmiloc_sv_prn(const struct qmiloc_sv *sv)
{
	int id = sv->svid;

	if (!(sv->valid & QMILOC_SV_VALID_SYSTEM) || !(sv->valid & QMILOC_SV_VALID_SVID) || !id)
		return 0;
	switch (sv->system) {
	case QMILOC_SV_GPS:
		return id >= 1 && id <= 32 ? id : 0;
	case QMILOC_SV_SBAS:
		return id >= 120 && id <= 158 ? id - 87 : 0;
	case QMILOC_SV_GLONASS:
		/* slots 1-32 here (255 = unknown slot) */
		return id >= 1 && id <= 32 ? id + 64 : 0;
	case QMILOC_SV_QZSS:
		return id >= 193 && id <= 200 ? id : 0;
	case QMILOC_SV_BDS:
	case QMILOC_SV_COMPASS:
		return id >= 201 && id <= 263 ? id : 0;
	case QMILOC_SV_GALILEO:
		return id >= 301 && id <= 336 ? id : 0;
	case QMILOC_SV_NAVIC:
		return id >= 401 && id <= 420 ? id : 0;
	default:
		return 0;
	}
}

int qmiloc_used_prn(uint16_t id)
{
	/* the used list numbers GLONASS 65-96 already; only SBAS differs */
	if (id >= 120 && id <= 158)
		return id - 87;
	return id;
}

const char *qmiloc_system_name(uint32_t system)
{
	static const char *const names[] = {
		"?", "GPS", "GAL", "SBAS", "BDS", "GLO", "BDS", "QZSS", "NAVIC",
	};

	return system < sizeof(names) / sizeof(names[0]) ? names[system] : "?";
}

/* A top-level string TLV is normally the bare characters; tolerate a
 * length prefix */
static const char *tlv_string(const uint8_t *v, int l, char *buf, size_t size)
{
	size_t n;

	if (l <= 0)
		return NULL;
	if (v[0] != '$' && v[0] != '!') {
		if (l >= 2 && v[0] == l - 1 && (v[1] == '$' || v[1] == '!')) {
			v += 1;
			l -= 1;
		} else if (l >= 3 && get_u16(v) == l - 2 && (v[2] == '$' || v[2] == '!')) {
			v += 2;
			l -= 2;
		}
	}
	n = strnlen((const char *)v, l);
	if (n >= size)
		n = size - 1;
	memcpy(buf, v, n);
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\r' || buf[n - 1] == '\n'))
		buf[--n] = '\0';
	return buf;
}

const char *qmiloc_parse_nmea(const uint8_t *tlvs, size_t len, char *buf, size_t size)
{
	const uint8_t *v;
	int l = qmiloc_tlv_find(tlvs, len, 0x10, &v);

	if (l <= 0)
		l = qmiloc_tlv_find(tlvs, len, 0x01, &v);
	return tlv_string(v, l, buf, size);
}

int qmiloc_inform_client_revision(struct qmiloc *q, uint32_t rev)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_INFORM_CLIENT_REVISION);
	qmiloc_req_u32(&m, 0x01, rev);
	return qmiloc_request(q, &m, NULL, NULL);
}

int qmiloc_register_master_client(struct qmiloc *q, uint32_t key)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_REGISTER_MASTER_CLIENT);
	qmiloc_req_u32(&m, 0x01, key);
	return qmiloc_request_ind(q, &m);
}

int qmiloc_get_supported_msgs(struct qmiloc *q, uint8_t *bitmap, size_t *len)
{
	struct qmiloc_req m;
	uint8_t resp[2048];
	size_t resp_len = sizeof(resp);
	const uint8_t *v;
	int rc, l;

	qmiloc_req_init(&m, QMILOC_GET_SUPPORTED_MSGS);
	rc = qmiloc_request(q, &m, resp, &resp_len);
	if (rc)
		return rc;
	/* TLV 0x10: u16 length, then the bit mask */
	l = qmiloc_tlv_find(resp, resp_len, 0x10, &v);
	if (l < 2)
		return -EPROTO;
	{
		size_t n = get_u16(v);

		if (n > (size_t)l - 2)
			n = l - 2;
		if (n > *len)
			n = *len;
		memcpy(bitmap, v + 2, n);
		*len = n;
	}
	return 0;
}

int qmiloc_msg_supported(const uint8_t *bitmap, size_t len, uint16_t msg_id)
{
	if (msg_id / 8 >= len)
		return 0;
	return (bitmap[msg_id / 8] >> (msg_id % 8)) & 1;
}

int qmiloc_get_engine_lock(struct qmiloc *q, uint32_t *lock)
{
	struct qmiloc_req m;
	int rc;

	qmiloc_req_init(&m, QMILOC_GET_ENGINE_LOCK);
	rc = qmiloc_request_ind(q, &m);
	if (!rc && qmiloc_tlv_u32(q->ind_buf, q->ind_len, 0x10, lock))
		rc = -EPROTO;
	return rc;
}

int qmiloc_set_engine_lock(struct qmiloc *q, uint32_t lock)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_SET_ENGINE_LOCK);
	qmiloc_req_u32(&m, 0x01, lock);
	return qmiloc_request_ind(q, &m);
}

int qmiloc_get_operation_mode(struct qmiloc *q, uint32_t *mode)
{
	struct qmiloc_req m;
	int rc;

	qmiloc_req_init(&m, QMILOC_GET_OPERATION_MODE);
	rc = qmiloc_request_ind(q, &m);
	if (!rc && qmiloc_tlv_u32(q->ind_buf, q->ind_len, 0x10, mode))
		rc = -EPROTO;
	return rc;
}

int qmiloc_set_operation_mode(struct qmiloc *q, uint32_t mode)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_SET_OPERATION_MODE);
	qmiloc_req_u32(&m, 0x01, mode);
	return qmiloc_request_ind(q, &m);
}

int qmiloc_set_nmea_types(struct qmiloc *q, uint32_t mask)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_SET_NMEA_TYPES);
	qmiloc_req_u32(&m, 0x01, mask);
	return qmiloc_request_ind(q, &m);
}

int qmiloc_reg_events(struct qmiloc *q, uint64_t mask, uint32_t client_type)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_REG_EVENTS);
	qmiloc_req_u64(&m, 0x01, mask);
	if (client_type)
		qmiloc_req_u32(&m, 0x11, client_type);
	return qmiloc_request(q, &m, NULL, NULL);
}

int qmiloc_start(struct qmiloc *q, uint8_t session, uint32_t interval_ms, int single)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_START);
	qmiloc_req_u8(&m, 0x01, session);
	qmiloc_req_u32(&m, 0x10, single ? 2 : 1);	/* single / periodic fixes */
	qmiloc_req_u32(&m, 0x11, 3);			/* high accuracy */
	qmiloc_req_u32(&m, 0x12, 1);			/* intermediate reports on */
	qmiloc_req_u32(&m, 0x13, interval_ms);
	return qmiloc_request(q, &m, NULL, NULL);
}

int qmiloc_stop(struct qmiloc *q, uint8_t session)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_STOP);
	qmiloc_req_u8(&m, 0x01, session);
	return qmiloc_request(q, &m, NULL, NULL);
}

int qmiloc_inject_utc_time(struct qmiloc *q, uint64_t utc_ms, uint32_t unc_ms)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_INJECT_UTC_TIME);
	qmiloc_req_u64(&m, 0x01, utc_ms);
	qmiloc_req_u32(&m, 0x02, unc_ms);
	return qmiloc_request_ind(q, &m);
}

int qmiloc_inject_position(struct qmiloc *q, double lat, double lon, float unc_m,
			   uint8_t confidence, uint32_t reliability, uint32_t source,
			   uint64_t utc_ms)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, QMILOC_INJECT_POSITION);
	qmiloc_req_double(&m, 0x10, lat);
	qmiloc_req_double(&m, 0x11, lon);
	qmiloc_req_float(&m, 0x12, unc_m);
	if (confidence)
		qmiloc_req_u8(&m, 0x13, confidence);
	if (reliability)
		qmiloc_req_u32(&m, 0x14, reliability);
	if (utc_ms)
		qmiloc_req_u64(&m, 0x1B, utc_ms);
	qmiloc_req_u32(&m, 0x1D, source);
	return qmiloc_request_ind(q, &m);
}

int qmiloc_delete_all_aiding(struct qmiloc *q, int use_new_msg)
{
	struct qmiloc_req m;

	qmiloc_req_init(&m, use_new_msg ? QMILOC_DELETE_GNSS_SERVICE_DATA : QMILOC_DELETE_ASSIST_DATA);
	qmiloc_req_u8(&m, 0x01, 1);	/* delete all */
	return qmiloc_request_ind(q, &m);
}

int qmiloc_inject_xtra(struct qmiloc *q, const uint8_t *data, size_t len, size_t part_size,
		       int legacy, uint64_t *constellations)
{
	uint8_t part[2 + 1024];
	unsigned int total, i;

	if (constellations)
		*constellations = 0;
	if (!part_size || part_size > 1024)
		part_size = 1024;
	if (!len || (len + part_size - 1) / part_size > 0xffff)
		return -EINVAL;
	total = (len + part_size - 1) / part_size;

	for (i = 1; i <= total; i++) {
		size_t off = (size_t)(i - 1) * part_size;
		size_t n = len - off < part_size ? len - off : part_size;
		struct qmiloc_req m;
		const uint8_t *v;
		int rc;

		qmiloc_req_init(&m, legacy ? QMILOC_INJECT_PREDICTED_ORBITS_DATA : QMILOC_INJECT_XTRA_DATA);
		qmiloc_req_u32(&m, 0x01, len);		/* total size */
		qmiloc_req_u16(&m, 0x02, total);	/* total parts */
		qmiloc_req_u16(&m, 0x03, i);		/* part number, from 1 */
		part[0] = n & 0xff;			/* data: u16 count, bytes */
		part[1] = n >> 8;
		memcpy(part + 2, data + off, n);
		qmiloc_req_tlv(&m, 0x04, part, n + 2);
		qmiloc_req_u32(&m, 0x10, 0);		/* format: XTRA */
		rc = qmiloc_request_ind(q, &m);
		if (rc)
			return rc;
		if (qmiloc_tlv_find(q->ind_buf, q->ind_len, 0x10, &v) == 2 && get_u16(v) != i)
			return -EPROTO;
		if (i == total && constellations &&
		    qmiloc_tlv_find(q->ind_buf, q->ind_len, 0x11, &v) == 8)
			*constellations = get_u64(v);
	}
	return 0;
}

/* A server list: u8 count, then u8-length-prefixed strings. Returns the
 * number of bytes used or -1. */
static int parse_server_list(const uint8_t *v, int l, char servers[3][256], int *n_out)
{
	int off = 1, n, i;

	if (l < 1)
		return -1;
	n = v[0];
	for (i = 0; i < n; i++) {
		int sl;

		if (off >= l)
			return -1;
		sl = v[off++];
		if (off + sl > l)
			return -1;
		if (i < 3) {
			memcpy(servers[i], v + off, sl);
			servers[i][sl] = '\0';
		}
		off += sl;
	}
	*n_out = n < 3 ? n : 3;
	return off;
}

static int parse_xtra_info(const uint8_t *tlvs, size_t len, struct qmiloc_xtra_info *info)
{
	const uint8_t *v;
	int l, off, used;

	/* 0x02: mask u32, absAgeHrs u16, relAgeInUTC u64, XTRA servers,
	 * NTP servers (delay u32, list), timeRequest u8, prefValidAge u16 */
	l = qmiloc_tlv_find(tlvs, len, 0x02, &v);
	if (l < 14)
		return -EPROTO;
	info->report_mask = get_u32(v);
	info->abs_age_hrs = get_u16(v + 4);
	info->rel_age_utc_ms = get_u64(v + 6);
	off = 14;
	used = parse_server_list(v + off, l - off, info->servers, &info->n_servers);
	if (used > 0) {
		off += used;
		if (off + 4 <= l) {
			info->ntp_delay_threshold = get_u32(v + off);
			off += 4;
			used = parse_server_list(v + off, l - off, info->ntp, &info->n_ntp);
			if (used > 0) {
				off += used;
				if (off + 1 <= l)
					info->time_request = v[off];
				if (off + 3 <= l)
					info->pref_valid_age_hrs = get_u16(v + off + 1);
			}
		}
	}
	if (qmiloc_tlv_find(tlvs, len, 0x14, &v) == 2) {
		info->has_file_version = 1;
		info->file_ver_major = v[0];
		info->file_ver_minor = v[1];
	}
	if (qmiloc_tlv_find(tlvs, len, 0x18, &v) == 8) {
		info->has_gen_time = 1;
		info->gen_time_ms = get_u64(v);
	}
	if (qmiloc_tlv_find(tlvs, len, 0x19, &v) == 4) {
		info->has_rem_valid = 1;
		info->rem_valid_min = get_u32(v);
	}
	return 0;
}

int qmiloc_query_xtra_info(struct qmiloc *q, uint32_t config, struct qmiloc_xtra_info *info)
{
	struct qmiloc_req m;
	int rc;

	memset(info, 0, sizeof(*info));
	qmiloc_req_init(&m, QMILOC_QUERY_XTRA_INFO);
	qmiloc_req_u32(&m, 0x01, config);
	rc = qmiloc_request_ind(q, &m);
	if (rc)
		return rc;
	return parse_xtra_info(q->ind_buf, q->ind_len, info);
}

int qmiloc_get_xtra_validity(struct qmiloc *q, uint64_t *start_utc_s, uint16_t *hours)
{
	struct qmiloc_req m;
	const uint8_t *v;
	int rc;

	*start_utc_s = 0;
	*hours = 0;
	qmiloc_req_init(&m, QMILOC_GET_PREDICTED_ORBITS_DATA_VALIDITY);
	rc = qmiloc_request_ind(q, &m);
	if (rc)
		return rc;
	if (qmiloc_tlv_find(q->ind_buf, q->ind_len, 0x10, &v) == 10) {
		*start_utc_s = get_u64(v);
		*hours = get_u16(v + 8);
	}
	return 0;
}

int qmiloc_get_xtra_source(struct qmiloc *q, uint32_t *max_file, uint32_t *max_part,
			   char servers[3][256], int *n_servers)
{
	struct qmiloc_req m;
	const uint8_t *v;
	int rc, l;

	*max_file = *max_part = 0;
	*n_servers = 0;
	qmiloc_req_init(&m, QMILOC_GET_PREDICTED_ORBITS_DATA_SOURCE);
	rc = qmiloc_request_ind(q, &m);
	if (rc)
		return rc;
	if (qmiloc_tlv_find(q->ind_buf, q->ind_len, 0x10, &v) == 8) {
		*max_file = get_u32(v);
		*max_part = get_u32(v + 4);
	}
	l = qmiloc_tlv_find(q->ind_buf, q->ind_len, 0x11, &v);
	if (l > 0)
		parse_server_list(v, l, servers, n_servers);
	return 0;
}

int qmiloc_clock_synced(uint32_t *unc_ms)
{
	struct timex tx;
	int state;

	memset(&tx, 0, sizeof(tx));
	state = adjtimex(&tx);
	if (unc_ms)
		*unc_ms = tx.maxerror > 0 ? (uint32_t)(tx.maxerror / 1000) : 0;
	return state >= 0 && state != TIME_ERROR && !(tx.status & STA_UNSYNC);
}

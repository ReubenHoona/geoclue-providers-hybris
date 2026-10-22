/*
 * qmiloc: a small client for the Qualcomm QMI location service (LOC,
 * service 16) over QRTR, for Linux systems without the Android location
 * stack. Message and TLV numbers follow Qualcomm's location_service_v02 IDL
 * (vendor/qcom/opensource/location, loc_api/loc_api_v02).
 *
 * The socket is non-blocking. Indications and service arrival/removal are
 * reported through callbacks from qmiloc_dispatch() and from the receive
 * loop of the synchronous requests, so a callback must not issue requests
 * itself; it should queue the work instead.
 *
 * Copyright (c) 2026. This file is part of geoclue-hybris and may be used
 * under the GNU Lesser General Public License version 2.1.
 */
#ifndef QMILOC_H
#define QMILOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QMILOC_SERVICE 16

/* Messages (requests, their responses and indications share the id) */
enum {
	QMILOC_GET_SUPPORTED_MSGS = 0x001E,
	QMILOC_INFORM_CLIENT_REVISION = 0x0020,
	QMILOC_REG_EVENTS = 0x0021,
	QMILOC_START = 0x0022,
	QMILOC_STOP = 0x0023,
	QMILOC_EVENT_POSITION_REPORT = 0x0024,
	QMILOC_EVENT_GNSS_SV_INFO = 0x0025,
	QMILOC_EVENT_NMEA = 0x0026,
	QMILOC_EVENT_INJECT_TIME_REQ = 0x0028,
	QMILOC_EVENT_INJECT_PREDICTED_ORBITS_REQ = 0x0029,
	QMILOC_EVENT_INJECT_POSITION_REQ = 0x002A,
	QMILOC_EVENT_ENGINE_STATE = 0x002B,
	QMILOC_EVENT_FIX_SESSION_STATE = 0x002C,
	QMILOC_GET_SERVICE_REVISION = 0x0032,
	QMILOC_INJECT_PREDICTED_ORBITS_DATA = 0x0035,
	QMILOC_GET_PREDICTED_ORBITS_DATA_SOURCE = 0x0036,
	QMILOC_GET_PREDICTED_ORBITS_DATA_VALIDITY = 0x0037,
	QMILOC_INJECT_UTC_TIME = 0x0038,
	QMILOC_INJECT_POSITION = 0x0039,
	QMILOC_SET_ENGINE_LOCK = 0x003A,
	QMILOC_GET_ENGINE_LOCK = 0x003B,
	QMILOC_SET_NMEA_TYPES = 0x003E,
	QMILOC_DELETE_ASSIST_DATA = 0x0044,
	QMILOC_SET_OPERATION_MODE = 0x004A,
	QMILOC_GET_OPERATION_MODE = 0x004B,
	QMILOC_DELETE_GNSS_SERVICE_DATA = 0x00A6,
	QMILOC_INJECT_XTRA_DATA = 0x00A7,
	QMILOC_QUERY_XTRA_INFO = 0x00B0,
	QMILOC_REGISTER_MASTER_CLIENT = 0x00BA,
	QMILOC_EVENT_ENGINE_LOCK_STATE = 0x00BB,
	QMILOC_EVENT_QUERY_XTRA_INFO_REQ = 0x00D6,
};

/* REG_EVENTS mask bits */
#define QMILOC_EV_POSITION_REPORT		0x00000001ull
#define QMILOC_EV_GNSS_SV_INFO			0x00000002ull
#define QMILOC_EV_NMEA				0x00000004ull
#define QMILOC_EV_INJECT_TIME_REQ		0x00000010ull
#define QMILOC_EV_INJECT_PREDICTED_ORBITS_REQ	0x00000020ull
#define QMILOC_EV_INJECT_POSITION_REQ		0x00000040ull
#define QMILOC_EV_ENGINE_STATE			0x00000080ull
#define QMILOC_EV_FIX_SESSION_STATE		0x00000100ull
#define QMILOC_EV_ENGINE_LOCK_STATE		0x2000000000ull
#define QMILOC_EV_QUERY_XTRA_INFO		0x200000000000ull

/* Engine lock (GET/SET_ENGINE_LOCK) */
enum { QMILOC_LOCK_NONE = 1, QMILOC_LOCK_MI = 2, QMILOC_LOCK_MT = 3, QMILOC_LOCK_ALL = 4 };

/* SET_OPERATION_MODE */
enum {
	QMILOC_MODE_DEFAULT = 1, QMILOC_MODE_MSB = 2, QMILOC_MODE_MSA = 3,
	QMILOC_MODE_STANDALONE = 4,
};

/* Position report session status */
enum { QMILOC_SESS_SUCCESS = 0, QMILOC_SESS_IN_PROGRESS = 1 };

/* SV system and status */
enum {
	QMILOC_SV_GPS = 1, QMILOC_SV_GALILEO = 2, QMILOC_SV_SBAS = 3, QMILOC_SV_COMPASS = 4,
	QMILOC_SV_GLONASS = 5, QMILOC_SV_BDS = 6, QMILOC_SV_QZSS = 7, QMILOC_SV_NAVIC = 8,
};
enum { QMILOC_SV_IDLE = 1, QMILOC_SV_SEARCH = 2, QMILOC_SV_TRACK = 3 };

#define QMILOC_SV_VALID_SYSTEM	0x01
#define QMILOC_SV_VALID_SVID	0x02
#define QMILOC_SV_VALID_STATUS	0x08
#define QMILOC_SV_VALID_INFO	0x10
#define QMILOC_SV_VALID_ELEV	0x20
#define QMILOC_SV_VALID_AZIM	0x40
#define QMILOC_SV_VALID_SNR	0x80

/* Technology mask of a position report */
#define QMILOC_TECH_SATELLITE	0x0001
#define QMILOC_TECH_CELLID	0x0002
#define QMILOC_TECH_WIFI	0x0004
#define QMILOC_TECH_SENSORS	0x0008
#define QMILOC_TECH_REFERENCE	0x0010
#define QMILOC_TECH_INJECTED	0x0020
#define QMILOC_TECH_HYBRID	0x0080

/*
 * Return codes: 0 success, negative errno for transport failures (-ETIMEDOUT
 * when the modem did not answer, -ENOTCONN when the service is not known),
 * QMILOC_RC_QMI(err) when the response carried a QMI error and
 * QMILOC_RC_STATUS(st) when the indication reported a failure status.
 */
#define QMILOC_RC_QMI(err)	(0x10000 | (err))
#define QMILOC_RC_STATUS(st)	(0x20000 | (st))
#define QMILOC_IS_QMI_ERR(rc)	(((rc) & 0xF0000) == 0x10000)
#define QMILOC_IS_STATUS(rc)	(((rc) & 0xF0000) == 0x20000)

/* QMI errors worth recognising */
#define QMI_ERR_INVALID_ARG		0x0030
#define QMI_ERR_INVALID_MESSAGE_ID	0x0039
#define QMI_ERR_NOT_SUPPORTED		0x005E

typedef void (*qmiloc_ind_cb)(void *user, uint16_t msg_id, const uint8_t *tlvs, size_t len);
typedef void (*qmiloc_svc_cb)(void *user, int up);

struct qmiloc {
	int fd;
	int up;				/* the LOC service is known */
	uint32_t node, port;		/* where it is */
	uint16_t txn;
	int timeout_ms;			/* per synchronous step, default 3000 */
	qmiloc_ind_cb on_ind;
	qmiloc_svc_cb on_svc;
	void *user;

	/* synchronous request state */
	uint16_t want_resp_txn, want_resp_id, want_ind;
	int resp_got, ind_got;
	uint8_t resp_buf[2048];
	size_t resp_len;
	uint8_t ind_buf[8192];
	size_t ind_len;
};

/* A request being built */
struct qmiloc_req {
	uint8_t buf[1200];
	size_t len;
	int overflow;
};

/* Open the socket and look the service up (the result arrives through
 * on_svc and q->up). Returns 0 or -errno. */
int qmiloc_open(struct qmiloc *q, qmiloc_ind_cb on_ind, qmiloc_svc_cb on_svc, void *user);
void qmiloc_close(struct qmiloc *q);

/* Handle everything that is waiting on the socket without blocking.
 * Returns the number of packets handled or -errno. */
int qmiloc_dispatch(struct qmiloc *q);

/* Pump the socket until the service is known or TIMEOUT_MS passes.
 * Returns 0 if it is known, -ETIMEDOUT otherwise. */
int qmiloc_wait_service(struct qmiloc *q, int timeout_ms);

/* Request building */
void qmiloc_req_init(struct qmiloc_req *m, uint16_t msg_id);
void qmiloc_req_tlv(struct qmiloc_req *m, uint8_t type, const void *data, size_t len);
void qmiloc_req_u8(struct qmiloc_req *m, uint8_t type, uint8_t v);
void qmiloc_req_u16(struct qmiloc_req *m, uint8_t type, uint16_t v);
void qmiloc_req_u32(struct qmiloc_req *m, uint8_t type, uint32_t v);
void qmiloc_req_u64(struct qmiloc_req *m, uint8_t type, uint64_t v);
void qmiloc_req_float(struct qmiloc_req *m, uint8_t type, float v);
void qmiloc_req_double(struct qmiloc_req *m, uint8_t type, double v);

/* Send a request and wait for its response; RESP gets the response TLVs if
 * not NULL (up to *RESP_LEN bytes). */
int qmiloc_request(struct qmiloc *q, struct qmiloc_req *m, uint8_t *resp, size_t *resp_len);
/* Send a request whose result comes in an indication with the same id; on
 * success the indication's TLVs are in q->ind_buf / q->ind_len. The
 * indication's status TLV (0x01) is checked. */
int qmiloc_request_ind(struct qmiloc *q, struct qmiloc_req *m);

const char *qmiloc_strerror(int rc);

/* TLV access */
int qmiloc_tlv_find(const uint8_t *tlvs, size_t len, uint8_t type, const uint8_t **val);
int qmiloc_tlv_u32(const uint8_t *tlvs, size_t len, uint8_t type, uint32_t *v);

/* Decoded position report (QMILOC_EVENT_POSITION_REPORT) */
#define QMILOC_MAX_USED 176
struct qmiloc_position {
	uint32_t session_status;
	uint8_t session_id;
	int has_latlon;
	double latitude, longitude;
	int has_hor_unc;
	float hor_unc;			/* meters, circular */
	uint8_t hor_conf;		/* percent, 0 if not given */
	int has_alt_ell, has_alt_msl, alt_assumed;
	float alt_ell, alt_msl;
	int has_vert_unc;
	float vert_unc;
	int has_speed_h, has_speed_v, has_heading, has_mag_dev;
	float speed_h, speed_v, heading, mag_dev;
	uint32_t tech_mask;
	int has_utc;
	uint64_t utc_ms;
	int has_dop;
	float pdop, hdop, vdop;
	int n_used;
	uint16_t used[QMILOC_MAX_USED];	/* SV ids, GLONASS as 65-96 */
};
int qmiloc_parse_position(const uint8_t *tlvs, size_t len, struct qmiloc_position *p);

/* One SV of a QMILOC_EVENT_GNSS_SV_INFO */
struct qmiloc_sv {
	uint32_t valid;
	uint32_t system;
	uint16_t svid;
	uint8_t health;
	uint32_t status;
	uint8_t info;			/* 1 ephemeris, 2 almanac */
	float elevation, azimuth, snr;
};
#define QMILOC_MAX_SV 176
/* Returns the number of SVs stored in OUT (at most MAX) or -1 */
int qmiloc_parse_sv_info(const uint8_t *tlvs, size_t len, struct qmiloc_sv *out, int max);

/* NMEA-style satellite numbers as Android/geoclue-hybris use them: GPS 1-32,
 * SBAS 33-71, GLONASS 65-96, QZSS 193-200, BeiDou 201-263, Galileo 301-336,
 * NavIC 401-. 0 if unknown. */
int qmiloc_sv_prn(const struct qmiloc_sv *sv);
int qmiloc_used_prn(uint16_t svid);
const char *qmiloc_system_name(uint32_t system);

/* NMEA sentence of a QMILOC_EVENT_NMEA (NUL-terminated into BUF) or NULL */
const char *qmiloc_parse_nmea(const uint8_t *tlvs, size_t len, char *buf, size_t size);

/* Higher-level operations; all synchronous */
int qmiloc_inform_client_revision(struct qmiloc *q, uint32_t rev);
int qmiloc_register_master_client(struct qmiloc *q, uint32_t key);
/* BITMAP gets the supported-messages bit mask (bit N = message N) */
int qmiloc_get_supported_msgs(struct qmiloc *q, uint8_t *bitmap, size_t *len);
int qmiloc_msg_supported(const uint8_t *bitmap, size_t len, uint16_t msg_id);
int qmiloc_get_engine_lock(struct qmiloc *q, uint32_t *lock);
int qmiloc_set_engine_lock(struct qmiloc *q, uint32_t lock);
int qmiloc_get_operation_mode(struct qmiloc *q, uint32_t *mode);
int qmiloc_set_operation_mode(struct qmiloc *q, uint32_t mode);
int qmiloc_set_nmea_types(struct qmiloc *q, uint32_t mask);
/* CLIENT_TYPE 1 = application framework (AFW); 0 leaves it out */
int qmiloc_reg_events(struct qmiloc *q, uint64_t mask, uint32_t client_type);
int qmiloc_start(struct qmiloc *q, uint8_t session, uint32_t interval_ms, int single);
int qmiloc_stop(struct qmiloc *q, uint8_t session);
int qmiloc_inject_utc_time(struct qmiloc *q, uint64_t utc_ms, uint32_t unc_ms);
/* SOURCE: 0 GNSS, 6 other; RELIABILITY 1 very low .. 4 high (0 not set);
 * UTC_MS 0 leaves the timestamp out */
int qmiloc_inject_position(struct qmiloc *q, double lat, double lon, float unc_m,
			   uint8_t confidence, uint32_t reliability, uint32_t source,
			   uint64_t utc_ms);
int qmiloc_delete_all_aiding(struct qmiloc *q, int use_new_msg);

/*
 * Inject XTRA (predicted orbits) data in parts of at most PART_SIZE bytes
 * (0 means 1024) with INJECT_XTRA_DATA, or with the older
 * INJECT_PREDICTED_ORBITS_DATA when LEGACY is set. CONSTELLATIONS, if not
 * NULL, gets the accepted-constellation mask of the last indication (0 if
 * the modem did not report one).
 */
int qmiloc_inject_xtra(struct qmiloc *q, const uint8_t *data, size_t len, size_t part_size,
		       int legacy, uint64_t *constellations);

struct qmiloc_xtra_info {
	uint32_t report_mask;
	uint16_t abs_age_hrs;		/* hours the current data is valid for */
	uint64_t rel_age_utc_ms;	/* when the data was downloaded */
	int n_servers;
	char servers[3][256];
	uint32_t ntp_delay_threshold;
	int n_ntp;
	char ntp[3][256];
	uint8_t time_request;		/* the engine wants UTC time */
	uint16_t pref_valid_age_hrs;
	int has_gen_time;
	uint64_t gen_time_ms;		/* file generation time */
	int has_rem_valid;
	uint32_t rem_valid_min;		/* remaining validity */
	int has_file_version;
	uint8_t file_ver_major, file_ver_minor;
};
#define QMILOC_XTRA_INFO_ABS_AGE	0x01
#define QMILOC_XTRA_INFO_REL_AGE	0x02
#define QMILOC_XTRA_INFO_XTRA_SERVER	0x04
#define QMILOC_XTRA_INFO_NTP_SERVER	0x08
#define QMILOC_XTRA_INFO_TIME_REQUEST	0x10
#define QMILOC_XTRA_INFO_PREF_VALID_AGE	0x20
#define QMILOC_XTRA_INFO_XTRA_DISABLED	0x80
int qmiloc_query_xtra_info(struct qmiloc *q, uint32_t config, struct qmiloc_xtra_info *info);
/* START_UTC_S 0 and HOURS 0 when the engine has no data */
int qmiloc_get_xtra_validity(struct qmiloc *q, uint64_t *start_utc_s, uint16_t *hours);
int qmiloc_get_xtra_source(struct qmiloc *q, uint32_t *max_file, uint32_t *max_part,
			   char servers[3][256], int *n_servers);

/*
 * Whether the system clock is NTP-synchronised (adjtimex reports no
 * TIME_ERROR); *UNC_MS gets the kernel's maximum error estimate.
 */
int qmiloc_clock_synced(uint32_t *unc_ms);

#ifdef __cplusplus
}
#endif

#endif /* QMILOC_H */

/*
    Copyright (c) 2026

    This file is part of geoclue-hybris.

    Geoclue-hybris is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License.
*/

#include "qmilocationbackend.h"

#include "hybrisprovider.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QSettings>
#include <QtCore/QSocketNotifier>
#include <QtCore/QStandardPaths>
#include <QtCore/QTimerEvent>
#include <QtCore/QUrl>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

#include <errno.h>
#include <math.h>
#include <time.h>

Q_LOGGING_CATEGORY(lcGeoclueQmiLoc, "geoclue.provider.qmiloc", QtInfoMsg)

HybrisLocationBackend *getLocationBackend()
{
    return qobject_cast<HybrisLocationBackend *>(new QmiLocationBackend());
}

namespace {

const char *const ConfigFile = "/etc/geoclue-qmiloc.conf";

// Key the Android location HAL uses to become the master client
const uint32_t MasterClientKey = 0xBAABCDEF;
const uint8_t SessionId = 1;
const double MpsToKnots = 1.943844;

const int SetupRetryMs = 10000;
const qint64 TimeInjectionIntervalMs = 5 * 60 * 1000;
const qint64 PositionSaveIntervalMs = 60 * 1000;
const qint64 UsedPrnsMaxAgeMs = 3000;
const qint64 UnlockRetryMs = 30000;
// do not hammer the XTRA servers: after a failed round wait this long
const int XtraRetryMs = 10 * 60 * 1000;
const qint64 XtraMinDownloadIntervalMs = 60 * 60 * 1000;
const int XtraMinSize = 1000;
const int XtraMaxSize = 1024 * 1024;
// a cached position grows this uncertain per second of age
const double PositionUncertaintyGrowth = 10.0;
const double PositionUncertaintyMax = 1500e3;
const qint64 PositionMaxAgeMs = 30LL * 24 * 3600 * 1000;

qint64 monotonicMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

QString lockName(uint32_t lock)
{
    switch (lock) {
    case QMILOC_LOCK_NONE: return QStringLiteral("none");
    case QMILOC_LOCK_MI: return QStringLiteral("AFW sessions");
    case QMILOC_LOCK_MT: return QStringLiteral("non-AFW sessions");
    case QMILOC_LOCK_ALL: return QStringLiteral("all sessions");
    default: return QString::number(lock);
    }
}

quint32 modeFromString(const QString &mode)
{
    if (mode == QLatin1String("standalone"))
        return QMILOC_MODE_STANDALONE;
    if (mode == QLatin1String("default"))
        return QMILOC_MODE_DEFAULT;
    if (mode == QLatin1String("msb"))
        return QMILOC_MODE_MSB;
    return 0;   // follow the provider
}

}

QmiLocationBackend::QmiLocationBackend(QObject *parent)
:   HybrisLocationBackend(parent), m_open(false), m_notifier(Q_NULLPTR),
    m_engineReady(false), m_wantRunning(false), m_sessionActive(false),
    m_intervalMs(1000), m_providerMode(QMILOC_MODE_STANDALONE), m_currentMode(0),
    m_xtraMaxPart(0), m_xtraMaxFile(0), m_lastUnlockAttempt(-UnlockRetryMs),
    m_usedPrnsTime(0), m_lastPositionSave(0), m_lastTimeInjection(-TimeInjectionIntervalMs),
    m_positionInjected(false), m_xtraInjected(false),
    m_network(Q_NULLPTR), m_xtraReply(Q_NULLPTR), m_xtraServerIndex(0), m_xtraServersTried(0),
    m_lastXtraAttempt(-XtraMinDownloadIntervalMs), m_lastXtraSuccess(-XtraMinDownloadIntervalMs),
    m_xtraPending(false), m_networkKnown(false), m_networkConnected(false),
    m_networkUnmetered(false)
{
    memset(&m_qmi, 0, sizeof(m_qmi));
    m_qmi.fd = -1;
    loadConfig();
}

QmiLocationBackend::~QmiLocationBackend()
{
    gnssCleanup();
}

void QmiLocationBackend::loadConfig()
{
    const QByteArray path = qgetenv("GEOCLUE_QMILOC_CONF");
    QSettings s(path.isEmpty() ? QString::fromLatin1(ConfigFile) : QString::fromLocal8Bit(path),
                QSettings::IniFormat);

    m_cfgUnlock = s.value(QStringLiteral("engine/unlock"), true).toBool();
    m_cfgMasterClient = s.value(QStringLiteral("engine/master_client"), true).toBool();
    m_cfgMode = modeFromString(s.value(QStringLiteral("engine/mode"), QStringLiteral("standalone")).toString());
    m_cfgIntermediate = s.value(QStringLiteral("position/intermediate"), false).toBool();
    m_cfgAccuracyThreshold = s.value(QStringLiteral("position/accuracy_threshold"), 0).toFloat();
    m_cfgUntrackedSatellites = s.value(QStringLiteral("satellites/untracked"), true).toBool();
    m_cfgNmeaLog = s.value(QStringLiteral("debug/nmea"), false).toBool();
    m_cfgInjectTime = s.value(QStringLiteral("assistance/time"), true).toBool();
    m_cfgInjectPosition = s.value(QStringLiteral("assistance/position"), true).toBool();
    m_cfgXtra = s.value(QStringLiteral("xtra/enabled"), true).toBool();
    m_cfgXtraMetered = s.value(QStringLiteral("xtra/metered"), true).toBool();
    m_cfgXtraNeedsNetwork = s.value(QStringLiteral("xtra/wait_for_network"), true).toBool();
    m_cfgXtraServers = s.value(QStringLiteral("xtra/servers"), QStringList()
                               << QStringLiteral("https://path1.xtracloud.net/xtra3Mgrbeji.bin")
                               << QStringLiteral("https://path2.xtracloud.net/xtra3Mgrbeji.bin")
                               << QStringLiteral("https://path3.xtracloud.net/xtra3Mgrbeji.bin")).toStringList();
    m_cfgXtraRefreshHours = s.value(QStringLiteral("xtra/refresh_hours"), 24).toInt();
    m_cfgXtraMaxAgeHours = s.value(QStringLiteral("xtra/max_age_hours"), 168).toInt();
    m_cfgUserAgent = s.value(QStringLiteral("xtra/user_agent")).toString();
    m_cfgCacheDir = s.value(QStringLiteral("cache/dir")).toString();
    if (m_cfgCacheDir.isEmpty())
        m_cfgCacheDir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation)
                        + QStringLiteral("/geoclue-qmiloc");
    m_cfgXtraServers.removeAll(QString());
}

QString QmiLocationBackend::cacheFile(const QString &name) const
{
    return m_cfgCacheDir + QLatin1Char('/') + name;
}

/*==========================================================================*
 * QMI plumbing
 *==========================================================================*/

void QmiLocationBackend::indicationCallback(void *user, uint16_t msgId, const uint8_t *tlvs, size_t len)
{
    static_cast<QmiLocationBackend *>(user)->handleIndication(msgId, tlvs, len);
}

void QmiLocationBackend::serviceCallback(void *user, int up)
{
    QmiLocationBackend *self = static_cast<QmiLocationBackend *>(user);

    // the service went away (modem restart) or moved: everything set up in
    // it is gone, so set it up again when it is back
    if (!up) {
        self->m_engineReady = false;
        if (self->m_sessionActive) {
            self->m_sessionActive = false;
            QMetaObject::invokeMethod(staticProvider, "engineOff", Qt::QueuedConnection);
        }
        self->m_positionInjected = false;
        self->m_xtraInjected = false;
        self->m_lastTimeInjection = -TimeInjectionIntervalMs;
    }
    QMetaObject::invokeMethod(self, "serviceChanged", Qt::QueuedConnection);
}

void QmiLocationBackend::socketActivated()
{
    const int rc = qmiloc_dispatch(&m_qmi);
    if (rc < 0)
        qCDebug(lcGeoclueQmiLoc) << "receive failed:" << strerror(-rc);
}

bool QmiLocationBackend::msgSupported(uint16_t msgId) const
{
    // unknown: assume it is and fall back on errors
    if (m_supportedMsgs.isEmpty())
        return true;
    return qmiloc_msg_supported(reinterpret_cast<const uint8_t *>(m_supportedMsgs.constData()),
                                m_supportedMsgs.size(), msgId);
}

void QmiLocationBackend::serviceChanged()
{
    if (!m_open)
        return;

    if (!m_qmi.up) {
        qCInfo(lcGeoclueQmiLoc) << "location service not available";
        return;
    }
    if (!m_engineReady) {
        qCInfo(lcGeoclueQmiLoc) << "location service at" << m_qmi.node << m_qmi.port;
        m_engineReady = setupEngine();
        if (!m_engineReady) {
            m_retryTimer.start(SetupRetryMs, this);
            return;
        }
        maintainAssistance();
    }
    if (m_wantRunning && !m_sessionActive && !startSession())
        m_retryTimer.start(SetupRetryMs, this);
}

bool QmiLocationBackend::setupEngine()
{
    uint8_t bitmap[1024];
    size_t len = sizeof(bitmap);
    uint32_t maxFile = 0, maxPart = 0;
    char servers[3][256];
    int rc, n = 0;

    rc = qmiloc_inform_client_revision(&m_qmi, 2);
    if (rc == -ETIMEDOUT || rc == -ENOTCONN) {
        qCWarning(lcGeoclueQmiLoc) << "location service does not answer:" << qmiloc_strerror(rc);
        return false;
    }

    if (m_cfgMasterClient) {
        rc = qmiloc_register_master_client(&m_qmi, MasterClientKey);
        if (rc)
            qCInfo(lcGeoclueQmiLoc) << "register master client:" << qmiloc_strerror(rc);
    }

    rc = qmiloc_get_supported_msgs(&m_qmi, bitmap, &len);
    if (!rc)
        m_supportedMsgs = QByteArray(reinterpret_cast<const char *>(bitmap), len);
    else
        m_supportedMsgs.clear();

    if (m_cfgUnlock)
        unlockEngine();

    if (msgSupported(QMILOC_GET_PREDICTED_ORBITS_DATA_SOURCE) &&
        !qmiloc_get_xtra_source(&m_qmi, &maxFile, &maxPart, servers, &n)) {
        m_xtraMaxFile = maxFile;
        m_xtraMaxPart = maxPart;
        for (int i = 0; i < n; i++)
            qCDebug(lcGeoclueQmiLoc) << "modem XTRA server:" << servers[i];
    }

    if (m_cfgNmeaLog)
        qmiloc_set_nmea_types(&m_qmi, 0x1f);

    const uint64_t base = QMILOC_EV_POSITION_REPORT | QMILOC_EV_GNSS_SV_INFO |
                          QMILOC_EV_ENGINE_STATE | QMILOC_EV_FIX_SESSION_STATE |
                          (m_cfgNmeaLog ? QMILOC_EV_NMEA : 0);
    const uint64_t requests = QMILOC_EV_INJECT_TIME_REQ | QMILOC_EV_INJECT_PREDICTED_ORBITS_REQ |
                              QMILOC_EV_INJECT_POSITION_REQ | QMILOC_EV_QUERY_XTRA_INFO |
                              QMILOC_EV_ENGINE_LOCK_STATE;

    // The modem rejects the registration unless the client type (AFW) is
    // given. Should it not know the newer event bits, register fewer.
    rc = qmiloc_reg_events(&m_qmi, base | requests, 1);
    if (rc) {
        qCInfo(lcGeoclueQmiLoc) << "register events with assistance requests:" << qmiloc_strerror(rc);
        rc = qmiloc_reg_events(&m_qmi, base | (requests & 0xff), 1);
        if (rc)
            rc = qmiloc_reg_events(&m_qmi, base, 1);
    }
    if (rc) {
        qCWarning(lcGeoclueQmiLoc) << "register events:" << qmiloc_strerror(rc);
        return false;
    }
    m_currentMode = 0;
    return true;
}

bool QmiLocationBackend::unlockEngine()
{
    uint32_t lock = 0;
    int rc;

    m_lastUnlockAttempt = monotonicMs();
    rc = qmiloc_get_engine_lock(&m_qmi, &lock);
    if (rc) {
        qCInfo(lcGeoclueQmiLoc) << "get engine lock:" << qmiloc_strerror(rc);
        return false;
    }
    if (lock == QMILOC_LOCK_NONE)
        return true;
    // The modem's default locks all sessions. The setting lives in its NV,
    // which here is the RAM copy rmtfs serves, so it does not persist.
    rc = qmiloc_set_engine_lock(&m_qmi, QMILOC_LOCK_NONE);
    if (rc) {
        qCWarning(lcGeoclueQmiLoc).noquote() << "engine lock" << lockName(lock) << "could not be lifted:"
                                   << qmiloc_strerror(rc);
        return false;
    }
    qCInfo(lcGeoclueQmiLoc).noquote().nospace() << "engine lock lifted (was " << lockName(lock) << ")";
    return true;
}

bool QmiLocationBackend::startSession()
{
    const quint32 mode = m_cfgMode ? m_cfgMode : m_providerMode;
    int rc;

    if (!m_engineReady)
        return false;

    if (mode != m_currentMode && msgSupported(QMILOC_SET_OPERATION_MODE)) {
        rc = qmiloc_set_operation_mode(&m_qmi, mode);
        if (rc)
            qCInfo(lcGeoclueQmiLoc) << "set operation mode" << mode << ":" << qmiloc_strerror(rc);
        else
            m_currentMode = mode;
    }

    rc = qmiloc_start(&m_qmi, SessionId, m_intervalMs, 0);
    if (rc) {
        qCWarning(lcGeoclueQmiLoc) << "start:" << qmiloc_strerror(rc);
        return false;
    }
    m_sessionActive = true;
    qCInfo(lcGeoclueQmiLoc) << "session started, interval" << m_intervalMs << "ms";
    return true;
}

void QmiLocationBackend::stopSession()
{
    if (!m_sessionActive)
        return;
    m_sessionActive = false;
    int rc = qmiloc_stop(&m_qmi, SessionId);
    if (rc)
        qCInfo(lcGeoclueQmiLoc) << "stop:" << qmiloc_strerror(rc);
    else
        qCInfo(lcGeoclueQmiLoc) << "session stopped";
}

void QmiLocationBackend::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_retryTimer.timerId()) {
        m_retryTimer.stop();
        serviceChanged();
    } else if (event->timerId() == m_xtraRetryTimer.timerId()) {
        m_xtraRetryTimer.stop();
        if (m_engineReady)
            checkXtra(false);
    } else {
        HybrisLocationBackend::timerEvent(event);
    }
}

/*==========================================================================*
 * Indications
 *==========================================================================*/

// Called from the socket's receive loop, possibly while a synchronous
// request waits: never issue requests here, queue the work instead.
void QmiLocationBackend::handleIndication(uint16_t msgId, const uint8_t *tlvs, size_t len)
{
    uint32_t v;

    switch (msgId) {
    case QMILOC_EVENT_POSITION_REPORT: {
        struct qmiloc_position p;
        if (!qmiloc_parse_position(tlvs, len, &p))
            handlePosition(p);
        break;
    }
    case QMILOC_EVENT_GNSS_SV_INFO:
        handleSatellites(tlvs, len);
        break;
    case QMILOC_EVENT_NMEA: {
        char buf[4096];
        if (qmiloc_parse_nmea(tlvs, len, buf, sizeof(buf)))
            qCInfo(lcGeoclueQmiLoc) << "nmea" << buf;
        break;
    }
    case QMILOC_EVENT_ENGINE_STATE:
        if (!qmiloc_tlv_u32(tlvs, len, 0x01, &v)) {
            qCDebug(lcGeoclueQmiLoc) << "engine" << (v == 1 ? "on" : "off");
            QMetaObject::invokeMethod(staticProvider, v == 1 ? "engineOn" : "engineOff",
                                      Qt::QueuedConnection);
        }
        break;
    case QMILOC_EVENT_FIX_SESSION_STATE:
        if (!qmiloc_tlv_u32(tlvs, len, 0x01, &v))
            qCDebug(lcGeoclueQmiLoc) << "fix session" << (v == 1 ? "started" : "finished");
        break;
    case QMILOC_EVENT_INJECT_TIME_REQ:
    case QMILOC_EVENT_INJECT_POSITION_REQ:
    case QMILOC_EVENT_INJECT_PREDICTED_ORBITS_REQ:
    case QMILOC_EVENT_QUERY_XTRA_INFO_REQ:
        QMetaObject::invokeMethod(this, "engineRequest", Qt::QueuedConnection, Q_ARG(int, msgId));
        break;
    case QMILOC_EVENT_ENGINE_LOCK_STATE:
        if (!qmiloc_tlv_u32(tlvs, len, 0x01, &v)) {
            qCInfo(lcGeoclueQmiLoc) << "engine lock state" << v;
            if (v != 1 && m_cfgUnlock)  // 1: unlocked
                QMetaObject::invokeMethod(this, "engineRequest", Qt::QueuedConnection,
                                          Q_ARG(int, QMILOC_EVENT_ENGINE_LOCK_STATE));
        }
        break;
    default:
        qCDebug(lcGeoclueQmiLoc) << "indication" << Qt::hex << msgId;
        break;
    }
}

void QmiLocationBackend::handlePosition(const struct qmiloc_position &p)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (p.session_status == 7) {    // engine locked
        qCWarning(lcGeoclueQmiLoc) << "fix refused: engine locked";
        if (m_cfgUnlock)
            QMetaObject::invokeMethod(this, "engineRequest", Qt::QueuedConnection,
                                      Q_ARG(int, QMILOC_EVENT_ENGINE_LOCK_STATE));
        return;
    }

    bool report = false;
    if (p.has_latlon) {
        if (p.session_status == QMILOC_SESS_SUCCESS) {
            // as the Android HAL: a final fix from satellites (or sensors)
            report = p.tech_mask & (QMILOC_TECH_SATELLITE | QMILOC_TECH_SENSORS | QMILOC_TECH_HYBRID);
        } else if (p.session_status == QMILOC_SESS_IN_PROGRESS && m_cfgIntermediate) {
            report = !(p.has_hor_unc && m_cfgAccuracyThreshold > 0 &&
                       p.hor_unc > m_cfgAccuracyThreshold);
        }
    }

    qCDebug(lcGeoclueQmiLoc) << "position status" << p.session_status << "tech" << Qt::hex
                             << p.tech_mask << Qt::dec << p.latitude << p.longitude << "+-"
                             << p.hor_unc << "used" << p.n_used << (report ? "reported" : "");

    if (p.session_status == QMILOC_SESS_SUCCESS || p.session_status == QMILOC_SESS_IN_PROGRESS) {
        m_usedPrns.clear();
        for (int i = 0; i < p.n_used; i++)
            m_usedPrns.append(qmiloc_used_prn(p.used[i]));
        m_usedPrnsTime = monotonicMs();
    }

    if (!report)
        return;

    Location loc;
    loc.setTimestamp(p.has_utc ? qint64(p.utc_ms) : now);
    loc.setLatitude(p.latitude);
    loc.setLongitude(p.longitude);
    if (!p.alt_assumed) {
        if (p.has_alt_msl)
            loc.setAltitude(p.alt_msl);
        else if (p.has_alt_ell)
            loc.setAltitude(p.alt_ell);
    }
    if (p.has_speed_h)
        loc.setSpeed(p.speed_h * MpsToKnots);
    if (p.has_heading)
        loc.setDirection(p.heading);
    if (p.has_speed_v)
        loc.setClimb(p.speed_v);
    Accuracy accuracy;
    if (p.has_hor_unc)
        accuracy.setHorizontal(p.hor_unc);
    if (p.has_vert_unc)
        accuracy.setVertical(p.vert_unc);
    loc.setAccuracy(accuracy);

    QMetaObject::invokeMethod(staticProvider, "setLocation", Qt::QueuedConnection,
                              Q_ARG(Location, loc));
    if (p.has_mag_dev)
        QMetaObject::invokeMethod(staticProvider, "setMagneticVariation", Qt::QueuedConnection,
                                  Q_ARG(double, p.mag_dev));

    if (p.session_status == QMILOC_SESS_SUCCESS &&
        monotonicMs() - m_lastPositionSave >= PositionSaveIntervalMs)
        saveLastPosition(loc.timestamp(), p.latitude, p.longitude,
                         p.has_hor_unc ? p.hor_unc : 100.0);
}

void QmiLocationBackend::handleSatellites(const uint8_t *tlvs, size_t len)
{
    static struct qmiloc_sv svs[QMILOC_MAX_SV];
    const int n = qmiloc_parse_sv_info(tlvs, len, svs, QMILOC_MAX_SV);
    QList<SatelliteInfo> satellites;
    QList<int> used;

    if (n < 0)
        return;
    for (int i = 0; i < n; i++) {
        const struct qmiloc_sv &sv = svs[i];
        const int prn = qmiloc_sv_prn(&sv);
        const bool hasSnr = (sv.valid & QMILOC_SV_VALID_SNR) && sv.snr > 0;

        if (!prn)
            continue;
        if (!m_cfgUntrackedSatellites && sv.status != QMILOC_SV_TRACK && !hasSnr)
            continue;
        SatelliteInfo si;
        si.setPrn(prn);
        if (sv.valid & QMILOC_SV_VALID_ELEV)
            si.setElevation(qRound(sv.elevation));
        if (sv.valid & QMILOC_SV_VALID_AZIM)
            si.setAzimuth(qRound(sv.azimuth));
        si.setSnr(hasSnr ? qRound(sv.snr) : 0);
        satellites.append(si);
    }
    if (monotonicMs() - m_usedPrnsTime <= UsedPrnsMaxAgeMs)
        used = m_usedPrns;

    QMetaObject::invokeMethod(staticProvider, "setSatellite", Qt::QueuedConnection,
                              Q_ARG(QList<SatelliteInfo>, satellites),
                              Q_ARG(QList<int>, used));
}

void QmiLocationBackend::engineRequest(int msgId)
{
    if (!m_engineReady)
        return;

    switch (msgId) {
    case QMILOC_EVENT_INJECT_TIME_REQ:
        qCInfo(lcGeoclueQmiLoc) << "engine requests the time";
        injectTime(true);
        break;
    case QMILOC_EVENT_INJECT_POSITION_REQ:
        qCInfo(lcGeoclueQmiLoc) << "engine requests a position";
        injectLastPosition(true);
        break;
    case QMILOC_EVENT_INJECT_PREDICTED_ORBITS_REQ:
    case QMILOC_EVENT_QUERY_XTRA_INFO_REQ:
        qCInfo(lcGeoclueQmiLoc) << "engine requests XTRA data";
        checkXtra(true);
        break;
    case QMILOC_EVENT_ENGINE_LOCK_STATE:
        if (monotonicMs() - m_lastUnlockAttempt >= UnlockRetryMs && unlockEngine() &&
            m_sessionActive) {
            m_sessionActive = false;
            if (!startSession())
                m_retryTimer.start(SetupRetryMs, this);
        }
        break;
    default:
        break;
    }
}

/*==========================================================================*
 * Assistance: time, position, XTRA
 *==========================================================================*/

void QmiLocationBackend::maintainAssistance()
{
    if (!m_engineReady)
        return;
    injectTime(false);
    injectLastPosition(false);
    checkXtra(false);
}

void QmiLocationBackend::injectTime(bool requested)
{
    uint32_t unc = 0;
    int rc;

    if (!m_cfgInjectTime)
        return;
    if (!requested && monotonicMs() - m_lastTimeInjection < TimeInjectionIntervalMs)
        return;
    if (!qmiloc_clock_synced(&unc)) {
        qCInfo(lcGeoclueQmiLoc) << "system clock not synchronised, time not injected";
        return;
    }
    // the kernel's maximum error grows by 0.5 ms/s from the last NTP update
    if (unc < 50)
        unc = 50;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    rc = qmiloc_inject_utc_time(&m_qmi, uint64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000, unc);
    if (rc) {
        qCInfo(lcGeoclueQmiLoc) << "inject time:" << qmiloc_strerror(rc);
        return;
    }
    m_lastTimeInjection = monotonicMs();
    qCInfo(lcGeoclueQmiLoc) << "time injected, uncertainty" << unc << "ms";
}

void QmiLocationBackend::saveLastPosition(qint64 utcMs, double latitude, double longitude, double accuracy)
{
    QDir().mkpath(m_cfgCacheDir);
    QSaveFile f(cacheFile(QStringLiteral("last-position")));
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QStringLiteral("%1 %2 %3 %4\n").arg(utcMs).arg(latitude, 0, 'f', 7)
            .arg(longitude, 0, 'f', 7).arg(accuracy, 0, 'f', 1).toLatin1());
    if (f.commit())
        m_lastPositionSave = monotonicMs();
}

void QmiLocationBackend::injectLastPosition(bool requested)
{
    if (!m_cfgInjectPosition || (m_positionInjected && !requested))
        return;

    QFile f(cacheFile(QStringLiteral("last-position")));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QList<QByteArray> fields = f.readLine().trimmed().split(' ');
    if (fields.size() < 4)
        return;
    const qint64 utc = fields.at(0).toLongLong();
    const double lat = fields.at(1).toDouble(), lon = fields.at(2).toDouble();
    const double acc = fields.at(3).toDouble();
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - utc;

    if (age < 0 || age > PositionMaxAgeMs || qAbs(lat) > 90 || qAbs(lon) > 180)
        return;
    // the watch may have moved since: grow the uncertainty with the age
    double unc = qMax(acc, 100.0) + PositionUncertaintyGrowth * age / 1000.0;
    if (unc > PositionUncertaintyMax)
        unc = PositionUncertaintyMax;
    const uint32_t reliability = age < 24 * 3600 * 1000LL ? 2 : 1;  // low, very low

    int rc = qmiloc_inject_position(&m_qmi, lat, lon, unc, 68, reliability, 6 /* other */, utc);
    if (rc) {
        qCInfo(lcGeoclueQmiLoc) << "inject position:" << qmiloc_strerror(rc);
        return;
    }
    m_positionInjected = true;
    qCInfo(lcGeoclueQmiLoc).nospace().noquote() << "last position injected (" << age / 60000
                                                << " min old, +-" << qRound(unc) << " m)";
}

void QmiLocationBackend::checkXtra(bool requested)
{
    if (!m_cfgXtra || !m_engineReady)
        return;

    // What the engine has: the hours its data stays valid
    double remainingHours = -1;
    double engineDataAgeHours = -1;
    bool timeRequest = false;
    struct qmiloc_xtra_info info;
    int rc = -1;

    if (msgSupported(QMILOC_QUERY_XTRA_INFO)) {
        rc = qmiloc_query_xtra_info(&m_qmi, 0, &info);
        if (!rc) {
            if (info.report_mask & QMILOC_XTRA_INFO_XTRA_DISABLED) {
                qCInfo(lcGeoclueQmiLoc) << "XTRA is disabled in the modem";
                return;
            }
            if (info.has_rem_valid)
                remainingHours = info.rem_valid_min / 60.0;
            else if (info.report_mask & QMILOC_XTRA_INFO_ABS_AGE)
                remainingHours = info.abs_age_hrs;
            if ((info.report_mask & QMILOC_XTRA_INFO_REL_AGE) && info.rel_age_utc_ms)
                engineDataAgeHours = (QDateTime::currentMSecsSinceEpoch() -
                                      qint64(info.rel_age_utc_ms)) / 3600e3;
            timeRequest = (info.report_mask & QMILOC_XTRA_INFO_TIME_REQUEST) && info.time_request;
        } else {
            qCDebug(lcGeoclueQmiLoc) << "query XTRA info:" << qmiloc_strerror(rc);
        }
    }
    if (rc && msgSupported(QMILOC_GET_PREDICTED_ORBITS_DATA_VALIDITY)) {
        uint64_t start = 0;
        uint16_t hours = 0;
        rc = qmiloc_get_xtra_validity(&m_qmi, &start, &hours);
        if (!rc)
            remainingHours = start ? (start * 1000.0 + hours * 3600e3 -
                                      QDateTime::currentMSecsSinceEpoch()) / 3600e3 : 0;
    }
    if (timeRequest)
        injectTime(true);

    const bool engineHasData = remainingHours > 6 &&
        (engineDataAgeHours < 0 || engineDataAgeHours < m_cfgXtraRefreshHours);
    qCInfo(lcGeoclueQmiLoc).nospace().noquote() << "XTRA in the engine: "
        << (remainingHours < 0 ? QStringLiteral("unknown") :
            QStringLiteral("valid %1 h more").arg(remainingHours, 0, 'f', 1))
        << (engineDataAgeHours >= 0 ? QStringLiteral(", %1 h old").arg(engineDataAgeHours, 0, 'f', 1)
                                    : QString())
        << (requested ? ", engine asks for new data" : "");
    if (engineHasData && !requested)
        return;

    // A cache younger than the refresh interval is good enough, unless the
    // engine asks again after getting it
    const QFileInfo cache(cacheFile(QStringLiteral("xtra.bin")));
    const qint64 cacheAgeMs = cache.exists()
        ? cache.lastModified().msecsTo(QDateTime::currentDateTime()) : -1;
    if (!m_xtraInjected && cacheAgeMs >= 0 && cacheAgeMs < m_cfgXtraRefreshHours * 3600e3 &&
        injectCachedXtra())
        return;

    if (monotonicMs() - m_lastXtraSuccess < XtraMinDownloadIntervalMs) {
        qCDebug(lcGeoclueQmiLoc) << "XTRA downloaded recently, not again";
        return;
    }
    m_xtraServersTried = 0;
    startXtraDownload();
}

bool QmiLocationBackend::injectCachedXtra()
{
    QFile f(cacheFile(QStringLiteral("xtra.bin")));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    qCInfo(lcGeoclueQmiLoc).noquote() << "injecting cached XTRA data from"
                            << QFileInfo(f).lastModified().toString(Qt::ISODate);
    return injectXtra(data);
}

bool QmiLocationBackend::injectXtra(const QByteArray &data)
{
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data.constData());
    uint64_t constellations = 0;
    bool legacy = !msgSupported(QMILOC_INJECT_XTRA_DATA);
    const qint64 started = monotonicMs();
    int rc;

    if (data.size() < XtraMinSize) {
        qCWarning(lcGeoclueQmiLoc) << "XTRA data of" << data.size() << "bytes not injected";
        return false;
    }
    if (m_xtraMaxFile && uint32_t(data.size()) > m_xtraMaxFile)
        qCWarning(lcGeoclueQmiLoc) << "XTRA data of" << data.size() << "bytes is more than the"
                                   << m_xtraMaxFile << "the engine takes; trying anyway";

    rc = qmiloc_inject_xtra(&m_qmi, bytes, data.size(), m_xtraMaxPart, legacy, &constellations);
    if (rc && !legacy && QMILOC_IS_QMI_ERR(rc)) {
        qCInfo(lcGeoclueQmiLoc) << "INJECT_XTRA_DATA:" << qmiloc_strerror(rc)
                                << ", trying INJECT_PREDICTED_ORBITS_DATA";
        legacy = true;
        rc = qmiloc_inject_xtra(&m_qmi, bytes, data.size(), m_xtraMaxPart, 1, &constellations);
    }
    if (rc) {
        qCWarning(lcGeoclueQmiLoc) << "XTRA injection failed:" << qmiloc_strerror(rc);
        return false;
    }
    m_xtraInjected = true;
    qCInfo(lcGeoclueQmiLoc) << "XTRA injected:" << data.size() << "bytes in"
                            << (monotonicMs() - started) << "ms, constellations" << Qt::hex
                            << constellations;
    return true;
}

void QmiLocationBackend::startXtraDownload()
{
    if (m_xtraReply)
        return;
    if (m_cfgXtraServers.isEmpty()) {
        qCInfo(lcGeoclueQmiLoc) << "no XTRA servers configured";
        return;
    }
    if (m_cfgXtraNeedsNetwork && m_networkKnown &&
        (!m_networkConnected || (!m_networkUnmetered && !m_cfgXtraMetered))) {
        qCInfo(lcGeoclueQmiLoc) << "XTRA download waits for a network";
        m_xtraPending = true;
        return;
    }
    m_xtraPending = false;

    if (!m_network)
        m_network = new QNetworkAccessManager(this);
    if (m_xtraServerIndex >= m_cfgXtraServers.size())
        m_xtraServerIndex = 0;

    const QUrl url(m_cfgXtraServers.at(m_xtraServerIndex));
    QNetworkRequest request(url);
    if (!m_cfgUserAgent.isEmpty())
        request.setHeader(QNetworkRequest::UserAgentHeader, m_cfgUserAgent);
    request.setTransferTimeout(30000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    qCInfo(lcGeoclueQmiLoc).noquote() << "downloading XTRA data from" << url.toString();
    m_lastXtraAttempt = monotonicMs();
    m_xtraServersTried++;
    m_xtraReply = m_network->get(request);
    connect(m_xtraReply, &QNetworkReply::finished, this, &QmiLocationBackend::xtraDownloadFinished);
}

void QmiLocationBackend::xtraDownloadFinished()
{
    QNetworkReply *reply = m_xtraReply;
    m_xtraReply = Q_NULLPTR;
    if (!reply)
        return;
    reply->deleteLater();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray data = reply->error() == QNetworkReply::NoError ? reply->read(XtraMaxSize + 1)
                                                                      : QByteArray();
    if (reply->error() != QNetworkReply::NoError || status != 200 ||
        data.size() < XtraMinSize || data.size() > XtraMaxSize) {
        qCWarning(lcGeoclueQmiLoc).noquote() << "XTRA download from" << reply->url().toString() << "failed:"
                                   << (reply->error() != QNetworkReply::NoError ? reply->errorString()
                                       : QStringLiteral("HTTP %1, %2 bytes").arg(status).arg(data.size()));
        m_xtraServerIndex++;
        if (m_xtraServersTried < m_cfgXtraServers.size())
            startXtraDownload();
        else
            xtraDownloadFailed();
        return;
    }

    if (!m_engineReady || !injectXtra(data)) {
        // keep the data only once the engine accepted it
        xtraDownloadFailed();
        return;
    }
    m_lastXtraSuccess = monotonicMs();

    QDir().mkpath(m_cfgCacheDir);
    QSaveFile f(cacheFile(QStringLiteral("xtra.bin")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(data);
        if (!f.commit())
            qCWarning(lcGeoclueQmiLoc) << "could not save" << f.fileName();
    }
}

void QmiLocationBackend::xtraDownloadFailed()
{
    // stale but maybe still valid data is better than none
    const QFileInfo cache(cacheFile(QStringLiteral("xtra.bin")));
    if (m_engineReady && !m_xtraInjected && cache.exists() &&
        cache.lastModified().msecsTo(QDateTime::currentDateTime()) < m_cfgXtraMaxAgeHours * 3600e3)
        injectCachedXtra();
    m_xtraRetryTimer.start(XtraRetryMs, this);
}

/*==========================================================================*
 * HybrisLocationBackend
 *==========================================================================*/

bool QmiLocationBackend::gnssInit()
{
    int rc;

    if (m_open)
        return true;
    rc = qmiloc_open(&m_qmi, indicationCallback, serviceCallback, this);
    if (rc) {
        qCWarning(lcGeoclueQmiLoc) << "no QRTR socket:" << strerror(-rc);
        return false;
    }
    m_open = true;
    m_notifier = new QSocketNotifier(m_qmi.fd, QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &QmiLocationBackend::socketActivated);

    // The modem may still be starting; the service is then set up when it
    // appears
    if (qmiloc_wait_service(&m_qmi, 2000))
        qCInfo(lcGeoclueQmiLoc) << "waiting for the location service";
    return true;
}

bool QmiLocationBackend::gnssStart()
{
    m_wantRunning = true;
    if (!m_qmi.up) {
        qCInfo(lcGeoclueQmiLoc) << "positioning starts when the location service appears";
        return true;
    }
    if (!m_engineReady) {
        m_engineReady = setupEngine();
        if (!m_engineReady) {
            m_wantRunning = false;
            m_retryTimer.start(SetupRetryMs, this);
            return false;
        }
    }
    maintainAssistance();
    if (m_sessionActive)
        return true;
    if (!startSession()) {
        m_wantRunning = false;
        return false;
    }
    return true;
}

bool QmiLocationBackend::gnssStop()
{
    m_wantRunning = false;
    m_retryTimer.stop();
    stopSession();
    return true;
}

void QmiLocationBackend::gnssCleanup()
{
    if (!m_open)
        return;
    gnssStop();
    if (m_xtraReply) {
        m_xtraReply->abort();
        m_xtraReply = Q_NULLPTR;
    }
    delete m_notifier;
    m_notifier = Q_NULLPTR;
    qmiloc_close(&m_qmi);
    m_open = false;
    m_engineReady = false;
}

bool QmiLocationBackend::gnssInjectTime(HybrisGnssUtcTime timeMs, int64_t timeReferenceMs,
                                        int32_t uncertaintyMs)
{
    if (!m_engineReady)
        return false;
    // timeReferenceMs is the CLOCK_MONOTONIC time of the measurement
    const int64_t utc = timeMs + (monotonicMs() - timeReferenceMs);
    int rc = qmiloc_inject_utc_time(&m_qmi, utc, uncertaintyMs > 0 ? uncertaintyMs : 1000);
    if (rc)
        qCInfo(lcGeoclueQmiLoc) << "inject time:" << qmiloc_strerror(rc);
    return !rc;
}

bool QmiLocationBackend::gnssInjectLocation(int timestamp, double latitudeDegrees,
                                            double longitudeDegrees, float accuracyMeters)
{
    if (!m_engineReady)
        return false;
    int rc = qmiloc_inject_position(&m_qmi, latitudeDegrees, longitudeDegrees,
                                    accuracyMeters > 0 ? accuracyMeters : 1000, 68, 2, 6,
                                    uint64_t(timestamp) * 1000);
    if (rc)
        qCInfo(lcGeoclueQmiLoc) << "inject position:" << qmiloc_strerror(rc);
    return !rc;
}

void QmiLocationBackend::gnssDeleteAidingData(HybrisGnssAidingData aidingDataFlags)
{
    if (!m_engineReady || aidingDataFlags != 0xFFFF)
        return;
    int rc = qmiloc_delete_all_aiding(&m_qmi, msgSupported(QMILOC_DELETE_GNSS_SERVICE_DATA));
    if (rc)
        rc = qmiloc_delete_all_aiding(&m_qmi, 0);
    qCInfo(lcGeoclueQmiLoc) << "delete aiding data:" << qmiloc_strerror(rc);
    m_positionInjected = m_xtraInjected = false;
}

bool QmiLocationBackend::gnssSetPositionMode(HybrisGnssPositionMode mode,
                                             HybrisGnssPositionRecurrence recurrence,
                                             uint32_t minIntervalMs, uint32_t preferredAccuracyMeters,
                                             uint32_t preferredTimeMs)
{
    Q_UNUSED(recurrence)
    Q_UNUSED(preferredAccuracyMeters)
    Q_UNUSED(preferredTimeMs)

    switch (mode) {
    case HYBRIS_GNSS_POSITION_MODE_MS_BASED: m_providerMode = QMILOC_MODE_MSB; break;
    case HYBRIS_GNSS_POSITION_MODE_MS_ASSISTED: m_providerMode = QMILOC_MODE_MSA; break;
    default: m_providerMode = QMILOC_MODE_STANDALONE; break;
    }

    const quint32 interval = qMax<quint32>(minIntervalMs, 1000);
    if (interval != m_intervalMs) {
        m_intervalMs = interval;
        // a START for the running session updates it
        if (m_sessionActive) {
            int rc = qmiloc_start(&m_qmi, SessionId, m_intervalMs, 0);
            qCInfo(lcGeoclueQmiLoc) << "interval" << m_intervalMs << "ms:" << qmiloc_strerror(rc);
        }
    }
    return true;
}

void QmiLocationBackend::gnssNiRespond(int32_t notifId, HybrisGnssUserResponseType userResponse)
{
    Q_UNUSED(notifId)
    Q_UNUSED(userResponse)
}

bool QmiLocationBackend::gnssXtraInjectXtraData(QByteArray &xtraData)
{
    return m_engineReady && injectXtra(xtraData);
}

bool QmiLocationBackend::aGnssDataConnOpen(const QByteArray &apn, const QString &protocol)
{
    Q_UNUSED(apn)
    Q_UNUSED(protocol)
    return false;
}

int QmiLocationBackend::aGnssSetServer(HybrisAGnssType type, const char *hostname, int port)
{
    Q_UNUSED(type)
    qCInfo(lcGeoclueQmiLoc) << "SUPL server" << hostname << port << "ignored: no SUPL without the HAL";
    return 0;
}

bool QmiLocationBackend::aGnssRilUpdateNetworkState(bool connected, bool unmetered)
{
    m_networkKnown = true;
    m_networkConnected = connected;
    m_networkUnmetered = unmetered;
    qCDebug(lcGeoclueQmiLoc) << "network" << connected << "unmetered" << unmetered;
    if (connected && m_xtraPending && m_engineReady && (unmetered || m_cfgXtraMetered)) {
        m_xtraServersTried = 0;
        startXtraDownload();
    }
    return true;
}

/*
    Copyright (c) 2026

    This file is part of geoclue-hybris.

    Geoclue-hybris is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License.
*/

/*
 * A location backend for Qualcomm modems that speaks the modem's QMI
 * location service (LOC) directly over QRTR, for systems without the
 * Android GNSS HAL (e.g. a mainline kernel). Besides positions and
 * satellites it keeps the engine assisted: it lifts the engine lock,
 * injects the system time when the clock is NTP-synchronised, the last
 * known position, and XTRA predicted orbits, which it downloads, caches and
 * refreshes on its own.
 */

#ifndef QMILOCATIONBACKEND_H
#define QMILOCATIONBACKEND_H

#include "hybrislocationbackend.h"
#include "locationtypes.h"
#include "qmiloc.h"

#include <QtCore/QBasicTimer>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QLoggingCategory>
#include <QtCore/QStringList>

Q_DECLARE_LOGGING_CATEGORY(lcGeoclueQmiLoc)

QT_FORWARD_DECLARE_CLASS(QSocketNotifier)
QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QNetworkReply)

class QmiLocationBackend : public HybrisLocationBackend
{
    Q_OBJECT
public:
    explicit QmiLocationBackend(QObject *parent = 0);
    ~QmiLocationBackend();

    // Gnss
    bool gnssInit() override;
    bool gnssStart() override;
    bool gnssStop() override;
    void gnssCleanup() override;
    bool gnssInjectTime(HybrisGnssUtcTime timeMs, int64_t timeReferenceMs, int32_t uncertaintyMs) override;
    bool gnssInjectLocation(int timestamp, double latitudeDegrees, double longitudeDegrees, float accuracyMeters) override;
    void gnssDeleteAidingData(HybrisGnssAidingData aidingDataFlags) override;
    bool gnssSetPositionMode(HybrisGnssPositionMode mode, HybrisGnssPositionRecurrence recurrence,
                             uint32_t minIntervalMs, uint32_t preferredAccuracyMeters,
                             uint32_t preferredTimeMs) override;

    // GnssDebug
    void gnssDebugInit() override {}

    // GnnNi
    void gnssNiInit() override {}
    void gnssNiRespond(int32_t notifId, HybrisGnssUserResponseType userResponse) override;

    // GnssXtra
    void gnssXtraInit() override {}
    bool gnssXtraInjectXtraData(QByteArray &xtraData) override;

    // AGnss: SUPL needs a data connection the modem cannot open here
    void aGnssInit() override {}
    bool aGnssDataConnClosed() override { return true; }
    bool aGnssDataConnFailed() override { return true; }
    bool aGnssDataConnOpen(const QByteArray &apn, const QString &protocol) override;
    int aGnssSetServer(HybrisAGnssType type, const char *hostname, int port) override;

    // AGnssRil
    void aGnssRilInit() override {}
    // Network state from connman (newer geoclue-hybris trees call it; no
    // override so that the backend also builds on trees without it)
    bool aGnssRilUpdateNetworkState(bool connected, bool unmetered);

protected:
    void timerEvent(QTimerEvent *event) override;

private slots:
    void socketActivated();
    void serviceChanged();
    void engineRequest(int msgId);
    void xtraDownloadFinished();

private:
    static void indicationCallback(void *user, uint16_t msgId, const uint8_t *tlvs, size_t len);
    static void serviceCallback(void *user, int up);
    void handleIndication(uint16_t msgId, const uint8_t *tlvs, size_t len);
    void handlePosition(const struct qmiloc_position &p);
    void handleSatellites(const uint8_t *tlvs, size_t len);

    void loadConfig();
    bool setupEngine();
    bool unlockEngine();
    bool startSession();
    void stopSession();
    bool msgSupported(uint16_t msgId) const;

    void maintainAssistance();
    void injectTime(bool requested);
    void injectLastPosition(bool requested);
    void saveLastPosition(qint64 utcMs, double latitude, double longitude, double accuracy);
    void checkXtra(bool requested);
    bool injectXtra(const QByteArray &data);
    bool injectCachedXtra();
    void startXtraDownload();
    void xtraDownloadFailed();

    QString cacheFile(const QString &name) const;

    struct qmiloc m_qmi;
    bool m_open;
    QSocketNotifier *m_notifier;

    bool m_engineReady;
    bool m_wantRunning;
    bool m_sessionActive;
    quint32 m_intervalMs;
    quint32 m_providerMode;
    quint32 m_currentMode;
    QByteArray m_supportedMsgs;
    quint32 m_xtraMaxPart;
    quint32 m_xtraMaxFile;
    qint64 m_lastUnlockAttempt;

    QList<int> m_usedPrns;
    qint64 m_usedPrnsTime;
    qint64 m_lastPositionSave;
    qint64 m_lastTimeInjection;
    bool m_positionInjected;
    bool m_xtraInjected;

    QNetworkAccessManager *m_network;
    QNetworkReply *m_xtraReply;
    int m_xtraServerIndex;
    int m_xtraServersTried;
    qint64 m_lastXtraAttempt;
    int m_xtraFailedRounds = 0;
    qint64 m_lastXtraSuccess;
    bool m_xtraPending;
    bool m_networkKnown;
    bool m_networkConnected;
    bool m_networkUnmetered;
    QBasicTimer m_retryTimer;
    QBasicTimer m_xtraRetryTimer;

    // configuration (/etc/geoclue-qmiloc.conf)
    bool m_cfgUnlock;
    bool m_cfgMasterClient;
    quint32 m_cfgMode;
    bool m_cfgIntermediate;
    float m_cfgAccuracyThreshold;
    bool m_cfgUntrackedSatellites;
    bool m_cfgNmeaLog;
    bool m_cfgInjectTime;
    bool m_cfgInjectPosition;
    bool m_cfgXtra;
    bool m_cfgXtraMetered;
    bool m_cfgXtraNeedsNetwork;
    QStringList m_cfgXtraServers;
    int m_cfgXtraRefreshHours;
    int m_cfgXtraMaxAgeHours;
    QString m_cfgUserAgent;
    QString m_cfgCacheDir;
};

#endif // QMILOCATIONBACKEND_H

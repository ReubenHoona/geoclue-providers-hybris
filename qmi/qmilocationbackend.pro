# geoclue-hybris with a backend that talks to the modem's QMI location
# service over QRTR (no Android GNSS HAL needed)
include($$PWD/../geoclue-providers-hybris.pri)

INCLUDEPATH += $$PWD

HEADERS += \
    qmilocationbackend.h \
    qmiloc.h

SOURCES += \
    qmilocationbackend.cpp \
    qmiloc.c

QMAKE_CFLAGS += -std=gnu11

qmiloc_conf.files = $$PWD/geoclue-qmiloc.conf
qmiloc_conf.path = /etc
INSTALLS += qmiloc_conf

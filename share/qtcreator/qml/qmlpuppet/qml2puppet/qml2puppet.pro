TARGET = qml2puppet

TEMPLATE = app
CONFIG += console

DESTDIR = $$[QT_INSTALL_BINS]

build_all:!build_pass {
    CONFIG -= build_all
    CONFIG += release
}

include(qml2puppet.pri)

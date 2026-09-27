TEMPLATE = app
TARGET = tst_punktfunkdecodeunit

QT -= gui
CONFIG += console testcase c++17
CONFIG -= app_bundle

INCLUDEPATH += $$PWD/../../moonlight-common-c/moonlight-common-c/src

SOURCES += \
    $$PWD/tst_punktfunkdecodeunit.cpp \
    $$PWD/../../app/streaming/punktfunk/punktfunkdecodeunit.cpp

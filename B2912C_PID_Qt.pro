QT += core gui widgets printsupport
!equals(QT_MAJOR_VERSION, 5): error("Use Qt 5.15.x MinGW 64-bit kit for this project")
lessThan(QT_MINOR_VERSION, 15): error("Qt 5.15.x is required. Select Qt 5.15.2 (qt5.15.2-mingw64), not the Qt 5.9.9 32-bit kit")
!equals(QT_ARCH, x86_64): error("64-bit Qt is required for KtB2900_64.dll. Select the Qt 5.15.2 MinGW 64-bit kit")
CONFIG += c++17
# Respect the Debug/Release configuration selected by Qt Creator or qmake.
CONFIG -= debug_and_release app_bundle
TEMPLATE = app
TARGET = B2912C_PID_Qt
VERSION = 1.0.0
RC_ICONS = assets/probe-control.ico
RESOURCES += assets/application.qrc
IVI_ROOT = $$(IVI_ROOT)
isEmpty(IVI_ROOT): IVI_ROOT = "C:/Program Files/IVI Foundation/IVI"
INCLUDEPATH += "$$IVI_ROOT/Include" src
!exists("$$IVI_ROOT/Include/KtB2900.h"): error("Install KtB2900 IVI-C x64 or set IVI_ROOT")
SOURCES += src/main.cpp src/instrument.cpp src/controller.cpp src/window.cpp src/PIDFunction.cpp src/protection.cpp tests/selftest.cpp tests/protectiontest.cpp
HEADERS += src/instrument.h src/controller.h src/window.h src/PIDFunction.h src/protection.h
SOURCES += src/qcustomplot.cpp src/traceplot.cpp tests/plottest.cpp tests/targettest.cpp
HEADERS += src/qcustomplot.h src/traceplot.h
SOURCES += src/datarecorder.cpp tests/meetingtest.cpp
SOURCES += src/windowui.cpp
SOURCES += tests/monitortest.cpp
SOURCES += tests/grouptest.cpp
SOURCES += tests/measurementtest.cpp
HEADERS += src/datarecorder.h
HEADERS += src/secondaverage.h
CONFIG(debug, debug|release) {
    DESTDIR = $$PWD/release/debug
} else {
    DESTDIR = $$PWD/release
}
OBJECTS_DIR = obj
MOC_DIR = moc
win32-g++: QMAKE_CXXFLAGS += -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8
win32-msvc: QMAKE_CXXFLAGS += /utf-8

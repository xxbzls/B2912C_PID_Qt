QT += core gui widgets printsupport
CONFIG += c++17 release
CONFIG -= debug debug_and_release app_bundle
TEMPLATE = app
TARGET = B2912C_StabilityTest
VERSION = 1.0.0
RC_ICONS = ../assets/probe-control.ico
RESOURCES += ../assets/application.qrc
INCLUDEPATH += ../src
# Deliberately excludes instrument.cpp, controller.cpp, PID and all VISA/IVI SDK headers.
SOURCES += main.cpp stability.cpp offlineui.cpp selftest.cpp traceplot_offline.cpp ../src/qcustomplot.cpp ../src/datarecorder.cpp
HEADERS += stability.h simulatedsignal.h ../src/traceplot.h ../src/qcustomplot.h ../src/datarecorder.h ../src/secondaverage.h
DESTDIR = bin
OBJECTS_DIR = obj
MOC_DIR = moc
win32: LIBS += -lpsapi
win32-g++: QMAKE_CXXFLAGS += -Wall -Wextra -finput-charset=UTF-8 -fexec-charset=UTF-8

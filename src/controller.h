#pragma once
#include <QThread>
#include <QString>
#include <QMutex>
#include <QWaitCondition>
#include <QElapsedTimer>
#include <atomic>
#include <memory>
#include "PIDFunction.h"
#include "datarecorder.h"

struct Settings {
    int mode = 0; // 0: plant, 1: IVI simulation, 2: real instrument
    QString resource;
    double targetV = 0, kp = 0.8, ti = 5.2, td = 0; // target: V; Kp: mA/V; Ti/Td: s
    double limitMA = 10, voltageLimit = 5, nplc = 1.0, manualMA = 0;
    bool overrideVoltageLimit = false; // Default: preserve CH2 compliance set on the instrument.
    int direction = 1, periodMs = 100;
    bool manual = false;
    bool currentTest = false; // CH2 only; no CH1 voltage feedback or PID.
    void validate() const;
};
class VoltagePid {
public:
    explicit VoltagePid(const Settings& settings);
    double step(double feedbackV, double seconds); // voltage error (V) -> final signed current (mA)
    void setTargetVoltage(double volts); // Worker thread only; preserve PID history.
private:
    Settings s;
    PID_TypeDef pid{};
    bool first = true;
};
class ConnectionProbe : public QThread {
    Q_OBJECT
public:
    ConnectionProbe(QString resource, bool simulation, QObject* parent = nullptr)
        : QThread(parent), resource(resource), simulation(simulation) {}
signals:
    void result(bool success, QString details);
protected:
    void run() override;
private:
    QString resource;
    bool simulation;
};
class ControlThread : public QThread {
    Q_OBJECT
public:
    explicit ControlThread(Settings s, QObject* parent = nullptr) : QThread(parent), settings(s) {}
    void requestStop();
    bool requestTargetVoltage(double volts); // Thread-safe mailbox, no instrument I/O.
signals:
    void connected(QString identity);
    void voltageLimitRead(double volts);
    // Voltage is CH1 feedback in PID mode, CH2 terminal voltage in current-test mode.
    void sample(double seconds, double voltage, double commandMA, double measuredMA, double periodMs, double targetV);
    void monitorSample(double seconds, double voltage, double periodMs); // CH2 OFF, no load-current sample.
    void targetApplied(double volts);
    void recordData(const RecordSample& sample); // Direct connection to recorder's bounded queue.
    void message(QString text);
    void failed(QString text);
    void runStopped();
protected:
    void run() override;
    void runControl(class Instrument& instrument);
    virtual std::unique_ptr<class Instrument> createInstrument() const;
    bool shouldStop() const { return isInterruptionRequested() || stopRequested.load(); }
    std::atomic_bool stopRequested{false};
    Settings settings;
    QElapsedTimer sampleClock;
private:
    QMutex targetMutex;
    bool acceptingTargets = false, targetPending = false;
    double pendingTarget = 0;
    void allowTargetUpdates(bool enabled);
    bool takeTargetVoltage(double& volts);
};
class SessionThread : public ControlThread {
    Q_OBJECT
public:
    explicit SessionThread(Settings connection, QObject* parent = nullptr) : ControlThread(connection,parent) {}
    bool startControl(const Settings& next);
    void disconnectDevice();
protected:
    void run() override;
private:
    QMutex mutex;
    QWaitCondition wake;
    Settings requested;
    bool ready=false, pending=false, busy=false;
};

#pragma once
#include <QString>

// Sampled software protection. A fault is latched for the lifetime of a run.
class MeasurementProtection {
public:
    MeasurementProtection(double limitMA, int periodMs, double feedbackLimitV = 15.0);
    void checkFeedback(double volts);
    void checkCurrent(double commandMA, double measuredMA, double elapsedSeconds);
    double currentToleranceMA() const;
    double mismatchDurationSeconds() const;
private:
    double limit, feedbackLimit, period, mismatchSince = -1, previousTime = -1;
    int mismatchSamples = 0;
    QString fault;
    void checkLatched() const;
    [[noreturn]] void trip(const QString& reason);
};

// A stable Windows kernel object shared across executable names and logon sessions.
// The OS releases it when the last handle closes, including on process termination.
class ProcessLock {
public:
    ProcessLock() = default;
    ~ProcessLock();
    ProcessLock(const ProcessLock&) = delete;
    ProcessLock& operator=(const ProcessLock&) = delete;
    void acquire(const QString& key);
    void release();
private:
    void* handle = nullptr;
};

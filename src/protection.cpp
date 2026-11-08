#include "protection.h"
#include <QCryptographicHash>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

[[noreturn]] static void fail(const QString& message) { throw std::runtime_error(message.toUtf8().constData()); }

MeasurementProtection::MeasurementProtection(double limitMA, int periodMs)
    : limit(limitMA), period(periodMs/1000.0) {
    if (!std::isfinite(limit) || limit <= 0 || limit > 10 || periodMs < 20 || periodMs > 2000)
        fail("Invalid measurement protection settings");
}
void MeasurementProtection::checkLatched() const { if (!fault.isEmpty()) fail(fault); }
void MeasurementProtection::trip(const QString& reason) { fault = reason; fail(fault); }
void MeasurementProtection::checkFeedback(double volts) {
    checkLatched();
    if (!std::isfinite(volts)) trip("保护停机：CH1 探头电压无效。");
    if (std::abs(volts) > 10.0)
        trip(QString("保护停机：CH1 探头电压 %1 V 超出 ±10 V。").arg(volts,0,'g',12));
}
double MeasurementProtection::currentToleranceMA() const { return std::max(0.005, 0.001*limit); }
double MeasurementProtection::mismatchDurationSeconds() const { return std::max(0.5, 3*period); }
void MeasurementProtection::checkCurrent(double commandMA, double measuredMA, double elapsedSeconds) {
    checkLatched();
    if (!std::isfinite(commandMA) || !std::isfinite(measuredMA) || !std::isfinite(elapsedSeconds)
        || elapsedSeconds < 0 || elapsedSeconds < previousTime)
        trip("保护停机：电流测量或采样时间无效。");
    previousTime = elapsedSeconds;
    if (std::abs(commandMA) > limit) trip("保护停机：设定电流超出配置限幅。");
    if (std::abs(measuredMA) > limit + currentToleranceMA())
        trip(QString("保护停机：CH2 实测电流 %1 mA 超出限幅 ±%2 mA（检测容差 %3 mA）。")
             .arg(measuredMA,0,'g',10).arg(limit,0,'g',8).arg(currentToleranceMA(),0,'g',8));
    // Only deviation detection has a startup grace period; overcurrent never does.
    const double deviationTolerance = std::max(0.02, 0.05*std::abs(commandMA));
    if (elapsedSeconds < 0.3 || std::abs(measuredMA-commandMA) <= deviationTolerance) {
        mismatchSince = -1; mismatchSamples = 0; return;
    }
    if (mismatchSince < 0) mismatchSince = elapsedSeconds;
    ++mismatchSamples;
    // Do not reset this timer when PID changes its command: that could mask a fault forever.
    if (mismatchSamples >= 3 && elapsedSeconds-mismatchSince >= mismatchDurationSeconds())
        trip(QString("保护停机：CH2 实测电流持续偏离设定值；设定 %1 mA，实测 %2 mA，偏差门限 %3 mA。")
             .arg(commandMA,0,'g',10).arg(measuredMA,0,'g',10).arg(deviationTolerance,0,'g',8));
}

ProcessLock::~ProcessLock() { release(); }
void ProcessLock::acquire(const QString& key) {
    if (handle) fail("Control lock is already held");
    const QString name = "Global\\B2912C_PID_Qt_" + QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(),QCryptographicHash::Sha256).toHex());
    // Existence, not mutex ownership, is the token. Never wait, steal or clear a lock.
    HANDLE candidate = CreateMutexW(nullptr,FALSE,reinterpret_cast<LPCWSTR>(name.utf16()));
    const DWORD error = GetLastError();
    if (!candidate) fail(QString("无法取得控制互斥锁（Windows %1）；为避免争用，拒绝连接。").arg(error));
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(candidate);
        fail("已有程序持有控制权，请先在原程序停止输出并断开连接。");
    }
    handle = candidate;
}
void ProcessLock::release() { if (handle) { CloseHandle(static_cast<HANDLE>(handle)); handle = nullptr; } }

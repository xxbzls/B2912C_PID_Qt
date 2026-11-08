#include "controller.h"
#include "instrument.h"
#include <QElapsedTimer>
#include <QScopeGuard>
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <memory>

void Settings::validate() const {
    for (double v : {limitMA,nplc,manualMA})
        if (!std::isfinite(v)) throw std::runtime_error("Parameters must be finite");
    if (mode < 0 || mode > 2 || limitMA <= 0 || limitMA > 10
        || ((manual || currentTest) && std::abs(manualMA) > limitMA) || periodMs < 20 || periodMs > 2000
        || nplc < 0.01 || nplc > 10)
        throw std::runtime_error("Parameters outside supported range");
    if (overrideVoltageLimit && (!std::isfinite(voltageLimit) || voltageLimit < 0.1 || voltageLimit > 20))
        throw std::runtime_error("Override voltage limit must be within 0.1 to 20 V");
    if (mode == 2 && resource.trimmed().isEmpty()) throw std::runtime_error("Enter the VISA resource from Connection Expert");
    if (!currentTest) {
        for (double v : {targetV,kp,ti,td})
            if (!std::isfinite(v)) throw std::runtime_error("PID parameters must be finite");
        if (kp < 0 || ti < 0 || td < 0 || std::abs(direction) != 1)
            throw std::runtime_error("Invalid PID settings");
        if (std::abs(targetV) > 10) throw std::runtime_error("Target voltage must be within +/-10 V");
    }
}
VoltagePid::VoltagePid(const Settings& settings) : s(settings) {
    s.validate();
    PID_Init(&pid, s.kp, s.ti, s.td, s.periodMs/1000.0, -s.limitMA, s.limitMA);
}
void VoltagePid::setTargetVoltage(double volts) {
    if (!std::isfinite(volts) || std::abs(volts)>10)
        throw std::runtime_error("Target voltage must be finite and within +/-10 V");
    s.targetV=volts;
}
double VoltagePid::step(double feedbackV, double seconds) {
    if (!std::isfinite(feedbackV) || !std::isfinite(seconds) || seconds <= 0 || seconds > 10)
        throw std::runtime_error("Invalid feedback or sample period");
    if (s.manual) return s.manualMA; // physical current, independent of PID direction
    if (first) { pid.feedback_last = feedbackV; pid.error_last = s.targetV-feedbackV; first = false; }
    pid.dt = seconds;
    const double output = PID_Compute(&pid, s.targetV, feedbackV);
    if (!std::isfinite(output)) throw std::runtime_error("Non-finite PID output");
    return s.direction * std::max(-s.limitMA, std::min(s.limitMA, output));
}
void ConnectionProbe::run() {
    Instrument instrument;
    try {
        if (!simulation && resource.trimmed().isEmpty()) throw std::runtime_error("Enter the VISA resource from Connection Expert");
        const QString identity=instrument.open(simulation ? "ProbeSimulationOnly" : resource.trimmed(),simulation,false);
        // Never configure or enable outputs. Unconfigured shutdown only closes this session.
        const QStringList errors=instrument.shutdown();
        if (!errors.isEmpty()) throw std::runtime_error(errors.join("; ").toUtf8().constData());
        emit result(true,identity);
    } catch(const std::exception& e) {
        const QStringList errors=instrument.shutdown();
        emit result(false,QString::fromUtf8(e.what())+(errors.isEmpty() ? "" : " | "+errors.join("; ")));
    }
}
std::unique_ptr<Instrument> ControlThread::createInstrument() const { return std::make_unique<Instrument>(); }
void ControlThread::requestStop() {
    stopRequested.store(true);
    allowTargetUpdates(false);
}
void ControlThread::allowTargetUpdates(bool enabled) {
    QMutexLocker lock(&targetMutex);
    acceptingTargets=enabled && !shouldStop();
    targetPending=false;
}
bool ControlThread::requestTargetVoltage(double volts) {
    if (!std::isfinite(volts) || std::abs(volts)>10) return false;
    QMutexLocker lock(&targetMutex);
    if (!acceptingTargets || shouldStop() || targetPending) return false;
    pendingTarget=volts;targetPending=true;
    return true;
}
bool ControlThread::takeTargetVoltage(double& volts) {
    QMutexLocker lock(&targetMutex);
    if (!acceptingTargets || shouldStop() || !targetPending) return false;
    volts=pendingTarget;targetPending=false;
    return true;
}
void ControlThread::run() {
    auto ownedInstrument = createInstrument();
    Instrument& instrument = *ownedInstrument;
    instrument.log = [this](QString text){ emit message(text); };
    try {
        settings.validate();
        if (settings.mode != 0) {
            const QString identity=instrument.open(settings.mode == 1 ? "Simulated" : settings.resource.trimmed(), settings.mode == 1);
            emit message(identity);
            emit connected(identity);
        }
        runControl(instrument);
    } catch(const std::exception& e) { emit failed(QString::fromUtf8(e.what())); }
    const QStringList errors = instrument.shutdown();
    if (!errors.isEmpty()) emit failed("关断未能全部确认，请在仪器面板检查输出：\n" + errors.join('\n'));
    else emit message(settings.mode == 0 ? "软件仿真已停止。" : "输出已关断并确认，SDK 会话已关闭。");
}
void ControlThread::runControl(Instrument& instrument) {
        // Every return, stop and exception closes the mailbox. No request survives a run.
        allowTargetUpdates(false);
        const auto targetCleanup=qScopeGuard([this]{allowTargetUpdates(false);});
        settings.validate();
        MeasurementProtection protection(settings.limitMA,settings.periodMs);
        std::unique_ptr<VoltagePid> pid;
        if (!settings.currentTest) pid.reset(new VoltagePid(settings));
        if (settings.mode != 0) {
            if (shouldStop()) return;
            instrument.configure(settings.voltageLimit, settings.nplc, settings.currentTest, !settings.overrideVoltageLimit, settings.limitMA);
            emit voltageLimitRead(instrument.outputVoltageLimit());
            if (shouldStop()) return;
            instrument.enable();
        }
        if (settings.mode == 1) emit message("【驱动仿真】SDK 测量值无物理对应关系，跳过电流/反馈阈值判定；保护逻辑由独立故障测试覆盖。");
        if (settings.currentTest) {
            emit message(settings.mode == 0 ? "电流测试软件仿真：模拟 1 kΩ 负载，未连接真实仪器。" :
                         settings.mode == 1 ? "电流测试驱动仿真：CH1 关闭，CH2 数据由 SDK 模拟。" :
                         "真实 CH2 电流测试：CH1 关闭；直接下发设定电流，不运行 PID。");
        } else if (settings.manual && settings.mode == 2) {
            emit message(QString("真实仪器已启动：手动固定输出 CH2 %1 mA；CH1 保持探头电压采集与越限保护。")
                         .arg(settings.manualMA, 0, 'f', 4));
        } else emit message(settings.mode == 0 ? "电压反馈仿真：1 mA 对应稳态 1 V，时间常数 0.8 s，正电流使探头电压增大。" :
                     settings.mode == 1 ? "IVI 驱动仿真：测量值由 SDK 模拟，不代表实际反馈响应" : "真实仪器已启动：CH1 探头电压与目标电压比较，PID 输出 CH2 电流。");
        QElapsedTimer& clock = sampleClock; clock.start();
        allowTargetUpdates(!settings.currentTest && !settings.manual);
        qint64 previousNs = 0;
        double plantVoltage = 0, lastCurrent = 0, lastMeasured = 0;
        int consecutiveDelayedSamples = 0;
        const double delayWarningSeconds = std::max(1.0,5.0*settings.periodMs/1000.0);
        const double delayShutdownSeconds = std::max(5.0,20.0*settings.periodMs/1000.0);
        while (!shouldStop()) {
            try {
            const qint64 cycleStart = clock.nsecsElapsed();
            if (settings.mode != 0) {
                instrument.verifyControlOwnership();
                instrument.checkCompliance(); // Detect hardware shutoff before the next control command.
            }
            const double plantDt = previousNs == 0 ? settings.periodMs/1000.0 : (cycleStart-previousNs)/1e9;
            // Exact first-order plant update for any positive interval.
            plantVoltage = lastCurrent + (plantVoltage-lastCurrent)*std::exp(-plantDt/0.8);
            double voltage = 0;
            if (!settings.currentTest)
                voltage = settings.mode == 0 ? plantVoltage : instrument.readVoltage();
            const qint64 now = clock.nsecsElapsed();
            const double dt = previousNs == 0 ? settings.periodMs/1000.0 : (now-previousNs)/1e9;
            previousNs = now;
            if (!settings.currentTest && settings.mode != 1) protection.checkFeedback(voltage);
            if (!settings.currentTest && settings.mode != 0)
                instrument.checkCompliance(); // CH1 may have tripped during its voltage acquisition.
            const bool delayed = dt > delayWarningSeconds;
            if (delayed) {
                ++consecutiveDelayedSamples;
                if (dt >= delayShutdownSeconds || consecutiveDelayedSamples >= 3)
                    throw std::runtime_error(QString("连续采样延迟或长时间未获得及时反馈（本次 %1 ms，连续 %2 次）；loop stopped")
                        .arg(dt*1000.0,0,'f',1).arg(consecutiveDelayedSamples).toUtf8().constData());
                emit message(QString("采样延迟告警：本次周期 %1 ms（连续 %2 次）；反馈与保护检查正常，保持上一条 %3 mA 电流指令并暂停本次 PID 更新。")
                    .arg(dt*1000.0,0,'f',1).arg(consecutiveDelayedSamples).arg(lastCurrent,0,'f',6));
                if (shouldStop()) break;
                if (settings.mode != 0) {
                    lastMeasured = instrument.readCurrent()*1000.0;
                    if (settings.mode != 1)
                        protection.checkCurrent(lastCurrent,lastMeasured,clock.nsecsElapsed()/1e9);
                    if (settings.currentTest) voltage = instrument.readOutputVoltage();
                    instrument.checkCompliance();
                } else {
                    lastMeasured = lastCurrent;
                    protection.checkCurrent(lastCurrent,lastMeasured,clock.nsecsElapsed()/1e9);
                    if (settings.currentTest) voltage = lastCurrent;
                }
                emit recordData({QDateTime::currentMSecsSinceEpoch(),now/1e9,voltage,
                                 settings.targetV,lastCurrent,lastMeasured,!settings.currentTest});
                emit sample(now/1e9,voltage,lastCurrent,lastMeasured,dt*1000.0,settings.targetV);
                continue;
            }
            if (consecutiveDelayedSamples > 0) {
                emit message(QString("采样延迟已恢复：当前周期 %1 ms；使用新反馈继续控制，不补算延迟期间的积分。")
                    .arg(dt*1000.0,0,'f',1));
                consecutiveDelayedSamples = 0;
            }
            double nextTarget=0;
            const bool targetChanged=takeTargetVoltage(nextTarget);
            if(targetChanged) {
                pid->setTargetVoltage(nextTarget);
                settings.targetV=nextTarget;
            }
            const double current = settings.currentTest ? settings.manualMA : pid->step(voltage,dt);
            if (shouldStop()) break;
            double measured = current;
            if (settings.mode != 0) {
                instrument.setCurrent(current/1000.0);
                measured = instrument.readCurrent()*1000.0;
                // Check as soon as the current is acquired, before another blocking voltage read.
                if (settings.mode != 1) protection.checkCurrent(current,measured,clock.nsecsElapsed()/1e9);
                if (settings.currentTest) voltage = instrument.readOutputVoltage();
                instrument.checkCompliance();
            } else {
                protection.checkCurrent(current,measured,clock.nsecsElapsed()/1e9);
                if (settings.currentTest) voltage = current; // Simulated 1 kΩ test load.
            }
            lastCurrent = current;
            lastMeasured = measured;
            emit recordData({QDateTime::currentMSecsSinceEpoch(),now/1e9,voltage,
                             settings.targetV,current,measured,!settings.currentTest});
            if(targetChanged) emit targetApplied(settings.targetV);
            emit sample(now/1e9,voltage,current,measured,dt*1000.0,settings.targetV);
            while (!shouldStop() && (clock.nsecsElapsed()-cycleStart)/1000000 < settings.periodMs) msleep(5);
            } catch(const std::exception& error) {
                const QString reason=QString::fromUtf8(error.what());
                if(settings.mode!=2 || !reason.contains("I/O error",Qt::CaseInsensitive)
                    || reason.contains("-241") || reason.contains("Module failed",Qt::CaseInsensitive)) throw;
                sdkDiagnostic("RECOVERY_BEGIN "+reason);
                emit message("通信异常，暂停PID，开始最多3次通信恢复；仪器实际输出状态待核验："+reason);
                bool recovered=false;
                for(int attempt=1;attempt<=3 && !shouldStop();++attempt) {
                    for(int wait=0;wait<10 && !shouldStop();++wait) msleep(100);
                    if(shouldStop()) break;
                    try {
                        instrument.recoverCommunication();
                        const double feedback=settings.currentTest ? 0 : instrument.readVoltage();
                        if(!settings.currentTest) protection.checkFeedback(feedback);
                        const double setting=instrument.currentSetting()*1000.0;
                        const double actual=instrument.readCurrent()*1000.0;
                        protection.checkCurrent(setting,actual,clock.nsecsElapsed()/1e9);
                        instrument.checkCompliance();
                        lastCurrent=setting;lastMeasured=actual;
                        previousNs=0;consecutiveDelayedSamples=0;
                        recovered=true;
                        emit message(QString("第%1次通信恢复成功，输出与反馈检查通过；使用新反馈继续，不补算中断期间积分。").arg(attempt));
                        sdkDiagnostic("RECOVERY_SUCCESS attempt="+QString::number(attempt));
                        break;
                    } catch(const std::exception& recoveryError) {
                        const QString detail=QString::fromUtf8(recoveryError.what());
                        sdkDiagnostic("RECOVERY_FAILED attempt="+QString::number(attempt)+" "+detail);
                        emit message(QString("第%1次恢复失败：%2").arg(attempt).arg(detail));
                        if(!detail.contains("I/O error",Qt::CaseInsensitive)
                            || detail.contains("-241") || detail.contains("Module failed",Qt::CaseInsensitive)) throw;
                    }
                }
                if(!recovered && !shouldStop()) throw;
            }
        }
}
bool SessionThread::startControl(const Settings& next) {
    next.validate();
    QMutexLocker lock(&mutex);
    if(!ready || busy || isInterruptionRequested()) return false;
    if(next.mode!=settings.mode || next.resource.trimmed()!=settings.resource.trimmed()) return false;
    requested=next;pending=true;busy=true;stopRequested.store(false);wake.wakeAll();return true;
}
void SessionThread::disconnectDevice() {
    QMutexLocker lock(&mutex);
    ready=false;requestInterruption();requestStop();wake.wakeAll();
}
void SessionThread::run() {
    auto ownedInstrument = createInstrument();
    Instrument& instrument = *ownedInstrument;
    instrument.log=[this](QString text){emit message(text);};
    try {
        if(settings.mode!=1 && settings.mode!=2) throw std::runtime_error("Instrument session requires IVI or real mode");
        settings.validate();
        const QString identity=instrument.open(settings.mode==1 ? "SessionSimulationOnly" : settings.resource.trimmed(),settings.mode==1,false);
        emit voltageLimitRead(instrument.outputVoltageLimit());
        if(!isInterruptionRequested()) instrument.prepareMonitoring(settings.nplc);
        sampleClock.start();
        qint64 previousMonitorNs=-1;
        { QMutexLocker lock(&mutex);ready=!isInterruptionRequested(); }
        if(!isInterruptionRequested()) emit connected(identity);
        while(!isInterruptionRequested()) {
            bool runPending=false;
            {
                QMutexLocker lock(&mutex);
                if(isInterruptionRequested()) break;
                if(pending) {settings=requested;pending=false;runPending=true;}
            }
            if(!runPending) {
                QElapsedTimer cycle;cycle.start();
                if(!settings.currentTest) {
                    instrument.checkMonitoring();
                    const double voltage=instrument.readVoltage();
                    if(settings.mode!=1) MeasurementProtection(settings.limitMA,settings.periodMs).checkFeedback(voltage);
                    instrument.checkMonitoring(); // A measurement may have tripped CH1 protection.
                    const qint64 now=sampleClock.nsecsElapsed();
                    const double dt=previousMonitorNs<0 ? 0 : (now-previousMonitorNs)/1e6;
                    previousMonitorNs=now;
                    if(!isInterruptionRequested()) emit monitorSample(now/1e9,voltage,dt);
                } else {
                    // Legacy offline current-only tests do not use CH1.
                    instrument.verifyControlOwnership();
                    if(instrument.outputEnabled(2)) throw std::runtime_error("CH2 unexpectedly enabled while idle");
                }
                QMutexLocker lock(&mutex);
                const qint64 remaining=settings.periodMs-cycle.elapsed();
                if(!pending && !isInterruptionRequested() && remaining>0)
                    wake.wait(&mutex,static_cast<unsigned long>(remaining));
                continue;
            }
            if(!shouldStop()) {
                runControl(instrument);
            }
            if(isInterruptionRequested()) break; // Disconnect/close shuts down BOTH channels below.
            const QStringList errors=instrument.stopControlOutput();
            if(!errors.isEmpty()) throw std::runtime_error(("关断未能全部确认，请在仪器面板检查输出："+errors.join("; ")).toUtf8().constData());
            if(!settings.currentTest) instrument.checkMonitoring();
            previousMonitorNs=-1;
            { QMutexLocker lock(&mutex);busy=false; }
            emit message("CH2 输出已关断并确认，未改写电流设定；CH1 继续采集探头电压。CH2 关断期间无有效负载电流实测值。");
            emit runStopped();
        }
    } catch(const std::exception& e) {
        { QMutexLocker lock(&mutex);ready=false; }
        emit failed(QString::fromUtf8(e.what()));
    }
    { QMutexLocker lock(&mutex);ready=false;busy=false;pending=false; }
    const QStringList errors=instrument.shutdown();
    if(!errors.isEmpty()) emit failed("连接关闭时未能确认关断，请检查仪器面板："+errors.join("; "));
}

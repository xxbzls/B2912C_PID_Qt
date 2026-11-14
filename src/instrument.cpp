#include "instrument.h"
#include <QDir>
#include <cmath>
#include <algorithm>
#include <limits>
#include <stdexcept>

static constexpr double probeHardwareBackstopV = 17.0;
static void fail(const QString& text) { throw std::runtime_error(text.toUtf8().constData()); }
Instrument::~Instrument() { shutdown(); }
void Instrument::check(ViStatus status, const char* operation) {
    if (status < 0) {
        ViStatus code = 0; char msg[1024] = {};
        if (GetError) GetError(session, &code, sizeof(msg), msg);
        QString detail = QString("%1 failed (%2): %3").arg(operation).arg(status).arg(QString::fromLocal8Bit(msg));
        // GetError returns the driver error. Only error_query retrieves the
        // instrument's SCPI error queue; capture it before cleanup changes it.
        if (status == static_cast<ViStatus>(KTB2900_ERROR_INSTRUMENT_STATUS) && session && error_query && !rawQueryPending) {
            for (int i = 0; i < 16; ++i) {
                ViInt32 instrumentCode = 0; char instrumentMessage[2048] = {};
                const ViStatus queryStatus = error_query(session, &instrumentCode, instrumentMessage);
                if (queryStatus < 0) {
                    detail += QString(" | ErrorQuery failed (%1)").arg(queryStatus);
                    break;
                }
                if (!instrumentCode) break;
                detail += QString(" | Instrument error queue entry read while handling %1 (may predate this operation): %2: %3")
                    .arg(QString::fromUtf8(operation)).arg(instrumentCode).arg(QString::fromLocal8Bit(instrumentMessage));
            }
        }
        sdkDiagnostic("SDK_FAILURE "+detail);
        sdkDiagnostic(detail.contains("-241") || detail.contains("Module failed",Qt::CaseInsensitive)
            ? "INSTRUMENT_MODULE_FAILURE reported by instrument; reboot not confirmed"
            : "COMMUNICATION_OR_DRIVER_FAILURE instrument hang/reboot UNKNOWN; no automatic reboot inferred");
        fail(detail);
    }
    if (status > 0 && log) log(QString("SDK warning %1: %2").arg(operation).arg(status));
}
QString Instrument::open(const QString& resource, bool simulation, bool resetOnOpen) {
    if (session) fail("Session already open");
    simulated = simulation;
    const QString root = qEnvironmentVariable("IVI_ROOT", "C:/Program Files/IVI Foundation/IVI");
    // The IVI-C DLL depends on sibling SDK DLLs. Scope PATH to this process.
    const QByteArray sdkBin = QDir::toNativeSeparators(QDir(root).filePath("Bin")).toLocal8Bit();
    if (!qgetenv("PATH").split(';').contains(sdkBin)) qputenv("PATH", sdkBin + ';' + qgetenv("PATH"));
    dll.setFileName(QDir(root).filePath("Bin/KtB2900_64.dll"));
    if (!dll.load()) fail(dll.errorString());
#define LOAD(name) { auto entry=reinterpret_cast<decltype(&KtB2900_##name)>(dll.resolve("KtB2900_" #name)); if(!entry) fail("Missing export: KtB2900_" #name); name=diagnosticSdk(entry,"KtB2900_" #name); }
    LOAD(InitWithOptions) LOAD(close) LOAD(GetError) LOAD(error_query) LOAD(reset)
    LOAD(GetOutputChannelName) LOAD(GetMeasurementName) LOAD(GetTransientName)
    LOAD(GetAttributeViString) LOAD(GetAttributeViReal64) LOAD(GetAttributeViBoolean) LOAD(GetAttributeViInt32)
    LOAD(SetAttributeViInt32) LOAD(SetAttributeViReal64) LOAD(SetAttributeViBoolean)
    LOAD(MeasurementInitiate) LOAD(SystemWaitForOperationComplete) LOAD(MeasurementFetchScalarData)
    LOAD(MeasurementAbort) LOAD(TransientAbort)
    LOAD(MeasurementFunctionDisableAll) LOAD(MeasurementFunctionSetEnabled) LOAD(MeasurementFunctionGetEnabled)
    LOAD(SystemRequestLock) LOAD(SystemReleaseLock) LOAD(SystemLockName) LOAD(SystemLockOwner)
    LOAD(SystemResetChannelGroups) LOAD(viWrite) LOAD(SystemReadBytes) LOAD(SystemClearIO)
    LOAD(StatusQuestionableReadConditionRegister)
    LOAD(SetSystemTracingEnabled) LOAD(SetSystemTracingFileName) LOAD(SetSystemTracingLevel)
    LOAD(MeasurementTraceGetSpace)
#undef LOAD
    QByteArray address = resource.toLatin1();
    const char* options = simulation
        ? "Cache=false, Simulate=true, QueryInstrStatus=true, DriverSetup=Model=B2912C"
        : "Cache=false, Simulate=false, QueryInstrStatus=true";
    // Normal connections preserve state. configure() snapshots compliance before resetting.
    // Never reset until exclusive control has been obtained.
    check(InitWithOptions(address.data(), VI_TRUE, VI_FALSE, options, &session), "InitWithOptions");
    sdkDiagnostic("SESSION_OPEN resource="+resource+" simulated="+QString::number(simulation));
    if(!simulation) {
        const QByteArray tracePath=QDir::toNativeSeparators(QCoreApplication::applicationDirPath()+
            "/Diagnostics/driver_"+QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz")+".log").toLocal8Bit();
        SetSystemTracingFileName(session,tracePath.constData());
        // Memory-growth comparison: keep application diagnostics, disable the
        // SDK's internal debug tracing for the whole session.
        check(SetSystemTracingEnabled(session,VI_FALSE),"Disable SDK internal tracing for memory comparison");
        sdkDiagnostic("SDK_INTERNAL_TRACING_DISABLED memory-comparison build; application diagnostics remain enabled");
    }
    diagnosticTimer.start();
    integer("", KTB2900_ATTR_SYSTEM_IO_TIMEOUT, 10000);
    char model[256] = {};
    check(GetAttributeViString(session, "", KTB2900_ATTR_INSTRUMENT_MODEL, sizeof(model), model), "Model");
    if (QString::fromLatin1(model).trimmed() != "B2912C") fail(QString("Expected B2912C, got %1").arg(model));
    for (int i = 0; i < 2; ++i) {
        char name[256] = {};
        check(GetOutputChannelName(session, i + 1, sizeof(name), name), "OutputChannelName"); out[i] = name;
        check(GetMeasurementName(session, i + 1, sizeof(name), name), "MeasurementName"); meas[i] = name;
    }
    // A new IVI simulation session returns 0 V rather than the documented 2 V
    // default. Seed ONLY the simulated channel; real connections remain read-only.
    if (simulation && outputVoltageLimit() == 0) {
        real(meas[1], KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_VALUE, 2.0);
        if (log) log("【驱动仿真】初始化模拟 CH2 电压上限为 2 V，未连接真实仪器。");
    }
    char serial[256] = {}, firmware[256] = {};
    check(GetAttributeViString(session, "", KTB2900_ATTR_SYSTEM_SERIAL_NUMBER, sizeof(serial), serial), "Serial number");
    check(GetAttributeViString(session, "", KTB2900_ATTR_INSTRUMENT_FIRMWARE_REVISION, sizeof(firmware), firmware), "Firmware revision");
    if (!simulated) {
        const QString serialNumber = QString::fromLatin1(serial).trimmed();
        if (serialNumber.isEmpty()) fail("无法识别仪器序列号，拒绝取得控制权。");
        // Serial number unifies VISA aliases, hostnames, HiSLIP, VXI-11 and USB addresses.
        deviceLock.acquire("instrument:" + serialNumber.toUpper());
        char owner[256] = {}, name[256] = {};
        check(SystemLockOwner(session,sizeof(owner),owner),"Read interface lock owner");
        const QString currentOwner = QString::fromLatin1(owner).trimmed().remove('"');
        if (currentOwner.compare("NONE",Qt::CaseInsensitive) != 0)
            fail("仪器接口已被占用（" + currentOwner + "），请先释放原控制会话；不会抢占或复位。");
        check(SystemLockName(session,sizeof(name),name),"Read interface name");
        interfaceName = QString::fromLatin1(name).trimmed().remove('"');
        ViBoolean granted = VI_FALSE;
        const ViStatus lockStatus = SystemRequestLock(session,&granted);
        interfaceLocked = granted == VI_TRUE;
        check(lockStatus,"Request instrument interface lock");
        if (!interfaceLocked) fail("仪器拒绝控制锁，尚未配置或开启输出。");
        verifyControlOwnership();
        if (log) log("已取得仪器控制锁；断开连接时释放。仪器面板在接口锁期间会锁定。");
    }
    if (resetOnOpen) { configured = true; resetForRun(); }
    return QString("%1 | 序列号 %2 | 固件 %3").arg(QString::fromLatin1(model).trimmed(), QString::fromLatin1(serial).trimmed(), QString::fromLatin1(firmware).trimmed());
}
void Instrument::integer(const QByteArray& c, ViAttr a, ViInt32 v) { check(SetAttributeViInt32(session,c.constData(),a,v),"SetInt32"); }
void Instrument::real(const QByteArray& c, ViAttr a, double v) { check(SetAttributeViReal64(session,c.constData(),a,v),"SetReal64"); }
void Instrument::boolean(const QByteArray& c, ViAttr a, bool v) { check(SetAttributeViBoolean(session,c.constData(),a,v?VI_TRUE:VI_FALSE),"SetBoolean"); }
void Instrument::writeScpi(const QByteArray& command) {
    // This IVI-C wrapper appends LF itself (verified on SOCKET and HiSLIP).
    // Supplying LF here sends two program-message terminators and can interrupt
    // an unread query response. This is NOT the VISA library's direct viWrite.
    if (command.isEmpty() || command.contains('\r') || command.contains('\n'))
        fail("内部 SCPI 指令格式无效，拒绝发送重复结束符。");
    ViInt64 written = 0;
    const ViStatus status = viWrite(session, command.size(),
        reinterpret_cast<ViByte*>(const_cast<char*>(command.constData())), &written);
    if (status < 0 || written != command.size()) rawQueryPending = true;
    check(status, "Write raw SCPI");
    if (written != command.size()) fail("SCPI 指令未完整发送，停止采集配置。");
}
void Instrument::prepareIndependentChannels() {
    if (simulated) {
        check(SystemResetChannelGroups(session), "Release simulated channel grouping");
        return;
    }
    // Explicit independent groups, as specified by the C-series SCPI reference.
    // This setting survives *RST; reset alone must not be relied on to remove it.
    writeScpi(":SYSTem:GROup:DEFine (@1),(@2)");
    rawQueryPending = true; // A failed OPC query must also be cleared before shutdown.
    check(SystemWaitForOperationComplete(session, 10000), "Complete independent channel configuration");
    rawQueryPending = false;
    verifyIndependentChannels();
}
void Instrument::verifyIndependentChannels() {
    if (simulated) return; // The IVI simulator does not model channel grouping.
    // KtB2900_SystemGetGroupedChannels parses only the first comma-separated
    // field (reproduced with the installed driver). ReadBytes preserves the full
    // response, including both groups. These raw APIs do not insert *ESR? between
    // the query and read, even with QueryInstrStatus enabled. Use the same session.
    rawQueryPending = true;
    writeScpi(":SYSTem:GROup:DEFine?");
    ViByte buffer[256] = {};
    ViInt64 received = 0;
    check(SystemReadBytes(session, sizeof(buffer), buffer, &received), "Read complete channel groups");
    if (received <= 0 || received >= static_cast<ViInt64>(sizeof(buffer)))
        fail("通道分组响应为空或超出缓冲区，拒绝独立采集。");
    rawQueryPending = false;
    // ReadBytes consumes a complete response and strips the terminator. Do not
    // accept a first-field prefix or a missing closing delimiter as independent.
    const QString response = QString::fromLatin1(reinterpret_cast<const char*>(buffer), static_cast<int>(received));
    QString groups = response;
    for (QChar whitespace : {QChar(' '), QChar('\t'), QChar('\r'), QChar('\n')}) groups.remove(whitespace);
    if (groups == "(@1,2)" || groups == "(@2,1)")
        fail("仪器通道仍处于联动分组，拒绝独立采集，以免联动开启 CH2。完整读回："+response);
    if (groups != "(@1),(@2)" && groups != "(@2),(@1)")
        fail("通道分组读回不完整或格式异常，拒绝独立采集。完整读回："+response);
}
void Instrument::configureMeasurement(int i, double nplc, bool measureCurrent) {
    boolean(meas[i], KTB2900_ATTR_MEASUREMENT_REMOTE_SENSING_ENABLED, false); // Force terminals, 2-wire.
    boolean(meas[i], KTB2900_ATTR_MEASUREMENT_VOLTAGE_NPLC_AUTO_ENABLED, false);
    real(meas[i], KTB2900_ATTR_MEASUREMENT_VOLTAGE_NPLC, nplc);
    boolean(meas[i], KTB2900_ATTR_MEASUREMENT_CURRENT_NPLC_AUTO_ENABLED, false);
    real(meas[i], KTB2900_ATTR_MEASUREMENT_CURRENT_NPLC, nplc);
    integer(meas[i], KTB2900_ATTR_MEASUREMENT_ARM_COUNT, 1);
    integer(meas[i], KTB2900_ATTR_MEASUREMENT_TRIGGER_COUNT, 1);
    integer(meas[i], KTB2900_ATTR_MEASUREMENT_ARM_SOURCE, KTB2900_VAL_ARM_TRIGGER_SOURCE_AINT);
    integer(meas[i], KTB2900_ATTR_MEASUREMENT_TRIGGER_SOURCE, KTB2900_VAL_ARM_TRIGGER_SOURCE_AINT);
    check(MeasurementFunctionDisableAll(session, meas[i].constData()), "Disable measurement functions");
    ViInt32 functions[] = {KTB2900_VAL_MEASUREMENT_TYPE_VOLTAGE, KTB2900_VAL_MEASUREMENT_TYPE_CURRENT};
    // This driver/firmware rejects a two-element FUNC:ON; separate calls are additive.
    for (int f = 0; f < (measureCurrent ? 2 : 1); ++f)
        check(MeasurementFunctionSetEnabled(session, meas[i].constData(), 1, &functions[f]), "Enable measurement function");
}
void Instrument::verifyProbeVoltageBackstop() {
    double actual = 0;
    check(GetAttributeViReal64(session,meas[0].constData(),KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_VALUE,&actual),
          "CH1 probe-voltage hardware backstop readback");
    if (!std::isfinite(actual) || std::abs(actual-probeHardwareBackstopV)>1e-6*probeHardwareBackstopV)
        fail(QString("CH1 探头硬件后备关断读回为 %1 V，不是要求的 ±%2 V；拒绝开启输出。")
             .arg(actual,0,'g',12).arg(probeHardwareBackstopV,0,'g',8));
    if(log) log(QString("CH1 探头硬件后备关断：±%1 V（已设置并读回核对）。")
                .arg(actual,0,'g',8));
}
void Instrument::prepareMonitoring(double nplc) {
    if (!session || !std::isfinite(nplc) || nplc < 0.01 || nplc > 10) fail("Invalid monitoring configuration");
    verifyControlOwnership();
    // Do not overwrite an existing source or adopt outputs started by another operator.
    if (outputEnabled(1) || outputEnabled(2))
        fail("连接采集前请先在仪器面板关闭 CH1 和 CH2；不会接管已开启的输出。");
    configured = true; // Partial configuration must also be cleaned up on failure.
    monitoringSession = true; runConfigured = false; currentTestOnly = false;
    // Grouped channels can start sourcing together during measurement. Break that
    // coupling and disarm auto-on before touching CH1; preserve CH2 level/compliance.
    for(int i=0;i<2;++i) {
        boolean(out[i], KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED, false);
        boolean(out[i], KTB2900_ATTR_OUTPUT_AUTO_OFF_ENABLED, false);
    }
    prepareIndependentChannels();
    integer(out[1], KTB2900_ATTR_OUTPUT_OFF_CONDITION, KTB2900_VAL_OUTPUT_OFF_CONDITION_HIZ);
    disableOutput(2);
    if(outputEnabled(2)) fail("CH2 未确认关断，拒绝启动探头采集。");
    integer(out[0], KTB2900_ATTR_OUTPUT_OFF_CONDITION, KTB2900_VAL_OUTPUT_OFF_CONDITION_HIZ);
    boolean(out[0], KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED, false);
    boolean(out[0], KTB2900_ATTR_OUTPUT_AUTO_OFF_ENABLED, false);
    boolean(out[0], KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED, true);
    check(MeasurementAbort(session,"(@1)"),"Abort previous CH1 acquisition");
    check(TransientAbort(session,"(@1)"),"Abort previous CH1 source trigger");
    integer(out[0], KTB2900_ATTR_OUTPUT_TYPE, KTB2900_VAL_OUTPUT_TYPE_CURRENT);
    integer(out[0], KTB2900_ATTR_OUTPUT_SHAPE, KTB2900_VAL_OUTPUT_SHAPE_DC);
    char transientName[256] = {};
    check(GetTransientName(session,1,sizeof(transientName),transientName),"CH1 transient name");
    integer(transientName, KTB2900_ATTR_TRANSIENT_CURRENT_MODE, KTB2900_VAL_TRANSIENT_CURRENT_VOLTAGE_MODE_FIXED);
    real(out[0], KTB2900_ATTR_OUTPUT_CURRENT_LEVEL, 0);
    boolean(out[0], KTB2900_ATTR_OUTPUT_CURRENT_AUTO_RANGE_ENABLED, false);
    real(out[0], KTB2900_ATTR_OUTPUT_CURRENT_RANGE, 1e-8);
    // Software protection is at most 15 V. The independent 17 V instrument
    // backstop leaves 2 V of margin while remaining inside the 20 V range.
    real(meas[0], KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_VALUE,probeHardwareBackstopV);
    verifyProbeVoltageBackstop();
    boolean(meas[0], KTB2900_ATTR_MEASUREMENT_VOLTAGE_AUTO_RANGE_ENABLED, false);
    real(meas[0], KTB2900_ATTR_MEASUREMENT_VOLTAGE_RANGE, 20);
    configureMeasurement(0, nplc, false);
    // Verify acquisition cannot autonomously reconnect a tripped channel.
    for (ViAttr attr : {KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED, KTB2900_ATTR_OUTPUT_AUTO_OFF_ENABLED,
                       KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED}) {
        ViBoolean value = VI_FALSE;
        check(GetAttributeViBoolean(session,out[0].constData(),attr,&value),"Read CH1 monitoring protection");
        if ((value == VI_TRUE) != (attr == KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED))
            fail("CH1 采集保护配置不一致，拒绝启动采集。");
    }
    double source = 0;
    ViInt32 sourceType = -1, sourceShape = -1, sourceMode = -1;
    check(GetAttributeViInt32(session,out[0].constData(),KTB2900_ATTR_OUTPUT_TYPE,&sourceType),"CH1 source type readback");
    check(GetAttributeViInt32(session,out[0].constData(),KTB2900_ATTR_OUTPUT_SHAPE,&sourceShape),"CH1 source shape readback");
    check(GetAttributeViInt32(session,transientName,KTB2900_ATTR_TRANSIENT_CURRENT_MODE,&sourceMode),"CH1 source mode readback");
    if (sourceType!=KTB2900_VAL_OUTPUT_TYPE_CURRENT || sourceShape!=KTB2900_VAL_OUTPUT_SHAPE_DC
        || sourceMode!=KTB2900_VAL_TRANSIENT_CURRENT_VOLTAGE_MODE_FIXED)
        fail("CH1 未正确配置为固定直流电流源，拒绝启动测压。");
    check(GetAttributeViReal64(session,out[0].constData(),KTB2900_ATTR_OUTPUT_CURRENT_LEVEL,&source),"CH1 zero-current readback");
    if (!std::isfinite(source) || source != 0) fail("CH1 测压电流设定不是 0 A，拒绝启动采集。");
    ViBoolean autoOn=VI_TRUE;
    check(GetAttributeViBoolean(session,out[1].constData(),KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED,&autoOn),"CH2 auto-on readback");
    if(autoOn) fail("CH2 自动开启未关闭，拒绝启动探头采集。");
    boolean(out[0], KTB2900_ATTR_OUTPUTS_ENABLED, true);
    checkMonitoring();
    if (log) log("探头采集已启动：CH1 保持 0 A 测压；CH2 保持关断，其设定电流和电压上限未改写。");
}
void Instrument::checkMonitoring() {
    verifyControlOwnership();
    verifyIndependentChannels();
    ViInt32 questionable = 0;
    check(StatusQuestionableReadConditionRegister(session,&questionable),"Read monitoring protection status");
    if (questionable & (KTB2900_VAL_STATUS_QUES_SRQ_REASON_CH1_OUTPUT_PROTECTION |
                        KTB2900_VAL_STATUS_QUES_SRQ_REASON_CH2_OUTPUT_PROTECTION))
        fail("采集保护：仪器到限关断已触发，不会自动重新开启通道。");
    ViBoolean compliance = VI_FALSE;
    check(GetAttributeViBoolean(session,meas[0].constData(),KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_STATUS_ENABLED,&compliance),"CH1 monitoring compliance");
    if (compliance || !outputEnabled(1)) fail("CH1 测压通道已关断或到限，采集停止；不会自动重启。");
    if (outputEnabled(2)) fail("采集状态下 CH2 意外开启，停止采集并关断两个通道。");
}
QStringList Instrument::stopControlOutput() {
    QStringList errors;
    if (!session || !configured) return errors;
    // CH2 OFF is the first operation. Never write 0 mA, ramp, or re-enable CH1 here.
    try { disableOutput(2); } catch (const std::exception& e) { errors << QString::fromUtf8(e.what()); }
    try { if (outputEnabled(2)) fail("CH2 output still enabled"); }
    catch (const std::exception& e) { errors << QString::fromUtf8(e.what()); }
    runConfigured = false;
    return errors;
}
void Instrument::configure(double voltageLimit, double nplc, bool currentTest, bool preserveVoltageLimit, double currentLimitMA) {
    if (!session || !std::isfinite(nplc) || nplc < 0.01 || nplc > 10) fail("Invalid instrument configuration");
    if (!std::isfinite(currentLimitMA) || currentLimitMA <= 0 || currentLimitMA > 100) fail("Invalid current limit");
    verifyControlOwnership();
    currentLimitAmps = currentLimitMA/1000.0;
    // B2912C provides discrete source/measurement ranges. Retain the 10 mA
    // range for the existing default and select 100 mA only when requested.
    const double currentRangeAmps = currentLimitMA <= 10 ? 0.01 : 0.1;
    // Read BEFORE reset: the reset would otherwise discard the customer's setting.
    // B2912C uses one symmetric compliance value (the POS/NEG attributes target other models).
    if (preserveVoltageLimit) voltageLimit = outputVoltageLimit();
    if (!std::isfinite(voltageLimit) || voltageLimit <= 0 || voltageLimit > (preserveVoltageLimit ? 210 : 20)
        || (!preserveVoltageLimit && voltageLimit < 0.1))
        fail("CH2 电压上限无效或为 0 V，请在仪器上设置，或在高级设置中明确覆盖；尚未开启输出。");
    currentTestOnly = currentTest;
    configured = true; // Cleanup is required even after a partially failed configuration.
    runConfigured = false;
    resetForRun();
    for (int i = 0; i < 2; ++i) {
        boolean(out[i], KTB2900_ATTR_OUTPUTS_ENABLED, false);
        integer(out[i], KTB2900_ATTR_OUTPUT_OFF_CONDITION, KTB2900_VAL_OUTPUT_OFF_CONDITION_HIZ);
        boolean(out[i], KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED, false);
        boolean(out[i], KTB2900_ATTR_OUTPUT_AUTO_OFF_ENABLED, false); // Continuous output between measurements.
        boolean(out[i], KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED, true);
        if (currentTestOnly && i == 0) continue; // CH1 stays disabled and is never measured.
        integer(out[i], KTB2900_ATTR_OUTPUT_TYPE, KTB2900_VAL_OUTPUT_TYPE_CURRENT);
        real(out[i], KTB2900_ATTR_OUTPUT_CURRENT_LEVEL, 0);
        boolean(out[i], KTB2900_ATTR_OUTPUT_CURRENT_AUTO_RANGE_ENABLED, false);
        real(out[i], KTB2900_ATTR_OUTPUT_CURRENT_RANGE, i == 0 ? 1e-8 : currentRangeAmps);
        if(i==1) {
            boolean(meas[i],KTB2900_ATTR_MEASUREMENT_CURRENT_AUTO_RANGE_ENABLED,false);
            real(meas[i],KTB2900_ATTR_MEASUREMENT_CURRENT_RANGE,currentRangeAmps);
        }
        real(meas[i], KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_VALUE,
             i == 0 ? probeHardwareBackstopV : voltageLimit);
        const bool autoVoltageRange = i == 1 && voltageLimit > 20;
        boolean(meas[i], KTB2900_ATTR_MEASUREMENT_VOLTAGE_AUTO_RANGE_ENABLED, autoVoltageRange);
        if (!autoVoltageRange) real(meas[i], KTB2900_ATTR_MEASUREMENT_VOLTAGE_RANGE, 20);
        configureMeasurement(i, nplc, i == 1);
    }
    if(!currentTestOnly) verifyProbeVoltageBackstop();
    const double actualLimit = outputVoltageLimit();
    if (std::abs(actualLimit-voltageLimit) > 1e-6 * std::max(1.0,voltageLimit))
        fail("CH2 电压上限读回与配置不一致；尚未开启输出。");
    verifyOutputProtection();
    // Keep the established voltage monitor available even if startup is cancelled
    // after configuration. A fault bypasses normal stop and shuts down both channels.
    if (monitoringSession && !currentTestOnly) boolean(out[0], KTB2900_ATTR_OUTPUTS_ENABLED, true);
    runConfigured = true;
    if (log) log("保护配置已读回：仪器到限关断已开启，测量自动开启输出已关闭。");
    if (log) log(QString("CH2 电压上限：±%1 V（%2）；电流由 PID 或手动设定，继续受电流限幅约束。")
        .arg(actualLimit,0,'g',8).arg(preserveVoltageLimit ? "保留仪器原值，复位后已恢复并核对" : "高级设置覆盖，已读回核对"));
    if (log) log(QString("CH2 电流量程：±%1 mA；本次软件限幅：±%2 mA。")
        .arg(currentRangeAmps*1000,0,'g',8).arg(currentLimitMA,0,'g',8));
}
void Instrument::enable() {
    if (!configured || !runConfigured) fail("Instrument not configured for control output");
    verifyControlOwnership();
    verifyOutputProtection();
    if (!currentTestOnly) {
        if (monitoringSession) checkMonitoring(); // Never re-enable CH1 if it tripped after configuration.
        else boolean(out[0], KTB2900_ATTR_OUTPUTS_ENABLED, true); // CH1: source 0 A, measure voltage.
    }
    boolean(out[1], KTB2900_ATTR_OUTPUTS_ENABLED, true);
}
double Instrument::read(ViInt32 type, const char* channels) {
    const bool current = type == KTB2900_VAL_MEASUREMENT_FETCH_TYPE_CURRENT;
    const QString channel = QByteArray(channels) == "(@1)" ? "CH1" :
                            QByteArray(channels) == "(@2)" ? "CH2" : QString::fromLatin1(channels);
    const QString measurement = channel + (current ? " 实测电流" : " 电压");
    const QString unit = current ? "A" : "V"; // Preserve the driver's native unit in diagnostics.
    double value[1] = {std::numeric_limits<double>::quiet_NaN()}; ViInt32 actual = 0;
    const auto checkMeasurement = [&](ViStatus status, const QString& operation) {
        const QByteArray context = (measurement + "：" + operation).toUtf8();
        check(status, context.constData());
    };
    // READ? also initiates source transitions (INIT:ALL), which can replace
    // the DC setpoint with the triggered level. Only acquire a new sample:
    // INIT:ACQ -> *OPC? -> FETCH. This leaves the continuous source unchanged.
    checkMeasurement(MeasurementInitiate(session, channels), "启动测量");
    rawQueryPending = true;
    checkMeasurement(SystemWaitForOperationComplete(session, 10000), "等待测量完成");
    rawQueryPending = false;
    rawQueryPending = true;
    checkMeasurement(MeasurementFetchScalarData(session, type, channels, 1, value, &actual), "读取测量结果");
    rawQueryPending = false;
    QString reason;
    if (actual != 1) reason = "返回数据数量不符";
    else if (!std::isfinite(value[0])) reason = "返回非有限值或未写入测量值";
    else if (std::abs(value[0]) > 1e30) reason = "返回仪器无效或超量程标记";
    if (!reason.isEmpty())
        fail(QString("%1测量无效：%2；返回数量=%3（应为 1），原始值=%4 %5。拒绝将该读数用于控制。")
             .arg(measurement, reason).arg(actual).arg(value[0], 0, 'g', 17).arg(unit));
    return value[0];
}
double Instrument::readVoltage() {
    if (currentTestOnly) fail("CH1 feedback is unavailable in current test mode");
    if(diagnosticTimer.isValid() && diagnosticTimer.elapsed()>=10000 && !rawQueryPending) {
        diagnosticTimer.restart();
        for(int i=0;i<2;++i) {
            ViInt32 available=0,total=0,mode=-1;
            const ViStatus spaceStatus=MeasurementTraceGetSpace(session,meas[i].constData(),&available,&total);
            const ViStatus modeStatus=GetAttributeViInt32(session,meas[i].constData(),
                KTB2900_ATTR_MEASUREMENT_TRACE_BUFFER_CONTROL,&mode);
            sdkDiagnostic(QString("TRACE_SPACE channel=%1 free=%2 total=%3 mode=%4 space_status=%5 mode_status=%6")
                .arg(i+1).arg(available).arg(total).arg(mode).arg(spaceStatus).arg(modeStatus));
        }
    }
    return read(KTB2900_VAL_MEASUREMENT_FETCH_TYPE_VOLTAGE, "(@1)");
}
double Instrument::readCurrent() { return read(KTB2900_VAL_MEASUREMENT_FETCH_TYPE_CURRENT, "(@2)"); }
double Instrument::readOutputVoltage() { return read(KTB2900_VAL_MEASUREMENT_FETCH_TYPE_VOLTAGE, "(@2)"); }
bool Instrument::outputEnabled(int channel) {
    if (!session || channel < 1 || channel > 2) fail("Invalid output channel");
    ViBoolean enabled = VI_FALSE;
    check(GetAttributeViBoolean(session, out[channel-1].constData(), KTB2900_ATTR_OUTPUTS_ENABLED, &enabled), "Output enabled");
    return enabled == VI_TRUE;
}
bool Instrument::measurementEnabled(int channel, ViInt32 function) {
    if (!session || channel < 1 || channel > 2) fail("Invalid measurement channel");
    ViInt32 functions[8] = {}, count = 0;
    check(MeasurementFunctionGetEnabled(session, meas[channel-1].constData(), 8, functions, &count), "Enabled measurement functions");
    if (count < 0 || count > 8) fail("Invalid measurement function count");
    for (int i = 0; i < count; ++i) if (functions[i] == function) return true;
    return false;
}
void Instrument::setCurrent(double amps) {
    if (!configured || !runConfigured || !std::isfinite(amps) || std::abs(amps) > currentLimitAmps) fail("Current exceeds configured limit or control output is not configured");
    real(out[1], KTB2900_ATTR_OUTPUT_CURRENT_LEVEL, amps);
    if (std::abs(currentSetting()-amps) > 1e-9)
        fail("保护停机：CH2 电流设定读回不一致，请检查是否有其他程序修改仪器。");
}
double Instrument::currentSetting() {
    double value = 0;
    check(GetAttributeViReal64(session, out[1].constData(), KTB2900_ATTR_OUTPUT_CURRENT_LEVEL, &value), "Current setting");
    if (!std::isfinite(value)) fail("CH2 电流设定读回无效。");
    return value;
}
double Instrument::outputVoltageLimit() {
    if (!session) fail("Connect the instrument before reading its voltage limit");
    double value = 0;
    check(GetAttributeViReal64(session, meas[1].constData(), KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_VALUE, &value), "CH2 voltage limit");
    if (!std::isfinite(value) || std::abs(value) > 210) fail("Invalid CH2 voltage limit readback");
    return std::abs(value);
}
void Instrument::checkCompliance() {
    ViInt32 questionable = 0;
    check(StatusQuestionableReadConditionRegister(session,&questionable),"Read output protection status");
    for (int i = currentTestOnly ? 1 : 0; i < 2; ++i) {
        const ViInt32 protectionBit = i == 0 ? KTB2900_VAL_STATUS_QUES_SRQ_REASON_CH1_OUTPUT_PROTECTION
                                             : KTB2900_VAL_STATUS_QUES_SRQ_REASON_CH2_OUTPUT_PROTECTION;
        if (questionable & protectionBit)
            fail(QString("保护停机：CH%1 仪器到限关断已触发；检查负载和电压上限后再手动启动。").arg(i+1));
        ViBoolean active = VI_FALSE;
        check(GetAttributeViBoolean(session, meas[i].constData(), KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_STATUS_ENABLED, &active), "Voltage compliance");
        if (active) fail(QString("CH%1 达到电压合规上限，设定电流可能无法达到；将停止并尝试关断。请检查负载及仪器电压上限。").arg(i+1));
        if (!outputEnabled(i+1))
            fail(QString("保护停机：CH%1 输出意外关闭，可能已触发仪器保护或被外部操作关闭；不会自动重启。").arg(i+1));
    }
}
void Instrument::verifyOutputProtection() {
    for (int i = 0; i < 2; ++i) {
        for (ViAttr attr : {KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED, KTB2900_ATTR_OUTPUT_AUTO_OFF_ENABLED,
                            KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED}) {
            ViBoolean value = VI_FALSE;
            check(GetAttributeViBoolean(session,out[i].constData(),attr,&value),"Read output protection configuration");
            const bool expected = attr == KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED;
            if ((value == VI_TRUE) != expected)
                fail(QString("CH%1 保护配置读回不一致，拒绝开启输出。").arg(i+1));
        }
    }
}
void Instrument::verifyControlOwnership() {
    if (simulated) return;
    if (!session || !interfaceLocked) fail("未取得仪器控制锁，拒绝控制。");
    char owner[256] = {};
    check(SystemLockOwner(session,sizeof(owner),owner),"Verify instrument interface lock");
    if (QString::fromLatin1(owner).trimmed().remove('"') != interfaceName)
        fail("仪器控制锁已丢失，停止运行并尝试关断输出。");
}
void Instrument::recoverCommunication() {
    check(SystemClearIO(session),"Recover communication boundary");
    rawQueryPending=false;
    verifyControlOwnership();
    verifyIndependentChannels();
    checkCompliance();
}
void Instrument::resetForRun() {
    if (!session) fail("Connect the instrument before starting output");
    check(reset(session), "Reset before output run");
    integer("", KTB2900_ATTR_SYSTEM_IO_TIMEOUT, 10000);
}
QStringList Instrument::stopOutputs() {
    QStringList errors;
    if (!session) return errors;
    auto attempt = [&](const std::function<void()>& fn) { try { fn(); } catch(const std::exception& e) { errors << QString::fromUtf8(e.what()); } };
    if (configured) {
        // A timed-out query may leave a response in the transport. Device Clear
        // restores the I/O boundary before OFF/readback; it does not write a
        // current setpoint or reset configuration. Even if clear fails, attempt
        // every OFF and verification below and report the recovery failure.
        if (rawQueryPending) attempt([&]{
            check(SystemClearIO(session), "Clear incomplete query before shutdown");
            rawQueryPending = false;
            if (log) log("未完成的查询已清理，正在关断并确认 CH2、CH1；未改写电流设定。");
        });
        // Shut down without changing the last current setpoint. A failed
        // verification must not delay the other channel's OFF command.
        for (int channel : {2,1}) attempt([&,channel]{ disableOutput(channel); });
        for (int i : {1,0}) {
            attempt([&,i]{
                if (outputEnabled(i+1)) fail(QString("CH%1 output still enabled").arg(i+1));
            });
        }
    }
    if(errors.isEmpty()) { configured = false; runConfigured = false; monitoringSession = false; }
    return errors;
}
void Instrument::disableOutput(int channel) {
    if (!session || channel < 1 || channel > 2) fail("Invalid output channel");
    boolean(out[channel-1],KTB2900_ATTR_OUTPUTS_ENABLED,false);
}
QStringList Instrument::shutdown() {
    QStringList errors=stopOutputs();
    if (!session) { deviceLock.release(); return errors; }
    if (interfaceLocked) {
        try { check(SystemReleaseLock(session),"Release instrument interface lock"); }
        catch(const std::exception& e) { errors << QString::fromUtf8(e.what()); }
        interfaceLocked = false;
    }
    try { check(close(session), "Close"); }
    catch(const std::exception& e) { errors << QString::fromUtf8(e.what()); }
    session = VI_NULL; configured = false; runConfigured = false; monitoringSession = false; rawQueryPending = false;
    deviceLock.release();
    return errors;
}

#pragma once
#include <QLibrary>
#include <QStringList>
#include <KtB2900.h>
#include <functional>
#include "protection.h"
#include "sdkdiagnostics.h"

// Official IVI-C declarations define both parameter types and calling convention.
// QLibrary avoids linking an MSVC import library from the MinGW toolchain.
class Instrument {
public:
    Instrument() = default;
    virtual ~Instrument();
    Instrument(const Instrument&) = delete;
    Instrument& operator=(const Instrument&) = delete;
    std::function<void(QString)> log;
    virtual QString open(const QString& resource, bool simulation, bool resetOnOpen = false);
    virtual void configure(double voltageLimit, double nplc, bool currentTest = false, bool preserveVoltageLimit = false, double currentLimitMA = 10);
    virtual void prepareMonitoring(double nplc); // CH1 zero-current voltmeter; never enables CH2.
    virtual void checkMonitoring();
    virtual QStringList stopControlOutput(); // Normal stop keeps CH1 measuring; no setpoint write.
    virtual void enable();
    virtual double readVoltage();
    virtual double readCurrent();
    virtual double readOutputVoltage();
    virtual bool outputEnabled(int channel);
    bool measurementEnabled(int channel, ViInt32 function);
    virtual double outputVoltageLimit();
    virtual QStringList stopOutputs();
    virtual void setCurrent(double amps);
    virtual void checkCompliance();
    void verifyOutputProtection();
    virtual void verifyControlOwnership();
    virtual void recoverCommunication();
    virtual QStringList shutdown(); // Attempt every shutdown step even if a previous step fails.
    double currentSetting();
protected:
    virtual void disableOutput(int channel);
private:
    friend int groupTest(); // Offline regression exercises the raw SCPI boundary.
    friend int measurementTest(); // Offline invalid-read diagnostics; no hardware.
    QLibrary dll;
    ViSession session = VI_NULL;
    bool configured = false;
    bool runConfigured = false, monitoringSession = false;
    bool currentTestOnly = false;
    bool simulated = false, interfaceLocked = false;
    bool rawQueryPending = false;
    QElapsedTimer diagnosticTimer;
    double currentLimitAmps = 0.01;
    QString interfaceName;
    ProcessLock deviceLock;
    QByteArray out[2], meas[2];
    void check(ViStatus status, const char* operation);
    void resetForRun();
    void configureMeasurement(int index, double nplc, bool measureCurrent);
    void verifyProbeVoltageBackstop();
    void verifyIndependentChannels();
    void prepareIndependentChannels();
    void writeScpi(const QByteArray& command);
    double read(ViInt32 type, const char* channels);
    void integer(const QByteArray& channel, ViAttr attr, ViInt32 value);
    void real(const QByteArray& channel, ViAttr attr, double value);
    void boolean(const QByteArray& channel, ViAttr attr, bool value);
#define API(name) DiagnosticFunction<decltype(&KtB2900_##name)>::Type name;
    API(InitWithOptions) API(close) API(GetError) API(error_query) API(reset)
    API(GetOutputChannelName) API(GetMeasurementName) API(GetTransientName)
    API(GetAttributeViString) API(GetAttributeViReal64) API(GetAttributeViBoolean) API(GetAttributeViInt32)
    API(SetAttributeViInt32) API(SetAttributeViReal64) API(SetAttributeViBoolean)
    API(MeasurementInitiate) API(SystemWaitForOperationComplete) API(MeasurementFetchScalarData)
    API(MeasurementAbort) API(TransientAbort)
    API(MeasurementFunctionDisableAll) API(MeasurementFunctionSetEnabled) API(MeasurementFunctionGetEnabled)
    API(SystemRequestLock) API(SystemReleaseLock) API(SystemLockName) API(SystemLockOwner)
    API(SystemResetChannelGroups) API(viWrite) API(SystemReadBytes) API(SystemClearIO)
    API(StatusQuestionableReadConditionRegister)
    API(SetSystemTracingEnabled) API(SetSystemTracingFileName) API(SetSystemTracingLevel)
    API(MeasurementTraceGetSpace)
#undef API
};

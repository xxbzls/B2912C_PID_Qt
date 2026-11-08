#include "protection.h"
#include "instrument.h"
#include "controller.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
enum class Fault { None, HighFeedback, LowFeedback, Overcurrent, Undercurrent, HardwareOff, LockLost };
struct FakeState {
    Fault fault = Fault::None;
    bool on[2] = {}, closed = false, currentOnly = false;
    int enables = 0, measurements = 0, configurations = 0;
    double amps = 0;
    QList<double> commands;
};
// Implements the transport boundary only; thresholds, PID, scheduling and exception
// cleanup are the production ControlThread code, running on its normal worker thread.
class FakeInstrument final : public Instrument {
public:
    explicit FakeInstrument(FakeState& state) : s(state) {}
    QString open(const QString&,bool simulation,bool) override {
        if(simulation) throw std::runtime_error("Fault tests must exercise real-mode protection branch");
        return "B2912C TEST DOUBLE (no physical I/O)";
    }
    void configure(double,double,bool currentOnly,bool,double) override {
        s.currentOnly=currentOnly;++s.configurations;s.on[0]=s.on[1]=false;s.amps=0;
    }
    void enable() override {++s.enables;s.on[0]=!s.currentOnly;s.on[1]=true;}
    void prepareMonitoring(double) override {s.on[0]=true;}
    void checkMonitoring() override {
        verifyControlOwnership();
        if(!s.on[0] || s.on[1]) throw std::runtime_error("Invalid idle channel state");
    }
    QStringList stopControlOutput() override {s.on[1]=false;return {};}
    double readVoltage() override {
        return s.fault==Fault::HighFeedback ? 10.0001 : s.fault==Fault::LowFeedback ? -10.0001 : 0;
    }
    double readCurrent() override {
        ++s.measurements;
        if(s.fault==Fault::HardwareOff) s.on[1]=false;
        if(s.fault==Fault::Overcurrent) return 0.002;
        if(s.fault==Fault::Undercurrent) return 0;
        return s.amps;
    }
    double readOutputVoltage() override {return s.amps*12;}
    double outputVoltageLimit() override {return 2;}
    bool outputEnabled(int channel) override {return s.on[channel-1];}
    void setCurrent(double amps) override {s.amps=amps;s.commands<<amps;}
    void checkCompliance() override {if(!s.on[1]) throw std::runtime_error("Injected hardware output trip");}
    void verifyControlOwnership() override {
        if(s.fault==Fault::LockLost) throw std::runtime_error("Injected lost device lock");
    }
    QStringList stopOutputs() override {s.on[0]=s.on[1]=false;return {};}
    QStringList shutdown() override {auto errors=stopOutputs();s.closed=true;return errors;}
private:
    FakeState& s;
};
class FaultWorker final : public ControlThread {
public:
    FaultWorker(const Settings& settings,FakeState& state):ControlThread(settings),s(state) {}
protected:
    std::unique_ptr<Instrument> createInstrument() const override {return std::make_unique<FakeInstrument>(s);}
private:
    FakeState& s;
};
class FaultSession final : public SessionThread {
public:
    FaultSession(const Settings& settings,FakeState& state):SessionThread(settings),s(state) {}
protected:
    std::unique_ptr<Instrument> createInstrument() const override {return std::make_unique<FakeInstrument>(s);}
private:
    FakeState& s;
};

// Exercise the production shutdown sequence with real IVI simulation underneath.
class ShutdownProbe final : public Instrument {
public:
    QStringList calls;
    int failDisableChannel=0, failReadChannel=0, reportEnabledChannel=0;
    bool forbidCurrentWrites=false;
    bool outputEnabled(int channel) override {
        calls << QString("read%1").arg(channel);
        if(channel==failReadChannel)throw std::runtime_error("Injected output read failure");
        if(channel==reportEnabledChannel)return true;
        return Instrument::outputEnabled(channel);
    }
    void setCurrent(double amps) override {
        calls << "setCurrent";
        if(forbidCurrentWrites)throw std::runtime_error("Current setpoint write forbidden during shutdown");
        Instrument::setCurrent(amps);
    }
protected:
    void disableOutput(int channel) override {
        calls << QString("off%1").arg(channel);
        if(channel==failDisableChannel)throw std::runtime_error("Injected OFF command failure");
        Instrument::disableOutput(channel);
    }
};
}

int protectionTest() {
    QStringList results; int exitCode = 0;
    auto require=[&](bool ok,const QString& name) {
        if (!ok) throw std::runtime_error(name.toStdString());
        results << "PASS " + name;
    };
    auto rejects=[](const std::function<void()>& action) {
        try { action(); } catch(const std::exception&) { return true; } return false;
    };
    try {
        MeasurementProtection boundary(1,100);
        for (double volts : {-10.0,0.0,10.0}) boundary.checkFeedback(volts);
        require(true,"Accept feedback endpoints +/-10 V");
        for (double volts : {-10.000001,10.000001}) {
            MeasurementProtection monitor(1,100);
            require(rejects([&]{monitor.checkFeedback(volts);}),"Reject feedback outside +/-10 V");
            require(rejects([&]{monitor.checkFeedback(0);}),"Voltage fault stays latched after signal recovers");
            require(rejects([&]{monitor.checkCurrent(0,0,1);}),"Voltage fault blocks subsequent current checks");
        }
        for (double invalid : {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
            MeasurementProtection voltage(1,100), current(1,100);
            require(rejects([&]{voltage.checkFeedback(invalid);}),"Invalid feedback trips");
            require(rejects([&]{current.checkCurrent(0,invalid,0);}),"Invalid measured current trips");
        }
        for (double limit : {0.1,1.0,10.0}) {
            for (int sign : {-1,1}) {
                MeasurementProtection monitor(limit,100);
                monitor.checkCurrent(sign*limit,sign*(limit+0.0005),0);
                require(true,"Allow small measurement noise at commanded current limit");
                require(rejects([&]{monitor.checkCurrent(sign*limit,sign*(limit+monitor.currentToleranceMA()+0.000001),0.01);}),
                        "Measured overcurrent trips immediately, including startup, both polarities");
                require(rejects([&]{monitor.checkCurrent(0,0,1);}),"Current fault stays latched");
            }
        }
        MeasurementProtection command(1,100);
        require(rejects([&]{command.checkCurrent(1.001,0,0);}),"Reject command beyond configured current limit");
        MeasurementProtection transient(1,100);
        transient.checkCurrent(1,0,0.2); // Startup grace only for tracking error.
        transient.checkCurrent(1,0,0.3);
        transient.checkCurrent(1,0,0.6);
        transient.checkCurrent(1,1,0.7); // Recovered, reset dwell timer.
        transient.checkCurrent(1,0,0.8);
        transient.checkCurrent(1,0,1.0);
        transient.checkCurrent(1,1,1.1);
        require(true,"Transient tracking errors recover without false trip");
        for (int periodMs : {20,100,2000}) {
            MeasurementProtection stuck(2,periodMs);
            stuck.checkCurrent(1,0,0.3);
            stuck.checkCurrent(1.2,0,0.4);
            const double deadline=0.3+stuck.mismatchDurationSeconds()+0.001;
            require(rejects([&]{stuck.checkCurrent(-1,0,deadline);}),
                    "Persistent tracking error trips even if PID changes command");
        }
        MeasurementProtection lowCurrent(1,100);
        lowCurrent.checkCurrent(0.001,0.004,0.5);lowCurrent.checkCurrent(0.001,0.004,2);
        require(true,"Absolute deviation floor avoids false trips around zero");

        for(bool currentOnly : {false,true}) {
            for(Fault scenario : {Fault::None,Fault::HighFeedback,Fault::LowFeedback,Fault::Overcurrent,
                                  Fault::Undercurrent,Fault::HardwareOff,Fault::LockLost}) {
                if(currentOnly && (scenario==Fault::HighFeedback || scenario==Fault::LowFeedback)) continue;
                Settings settings;settings.mode=2;settings.resource="FAKE-NO-HARDWARE";
                settings.manual=true;settings.manualMA=1;settings.limitMA=1;settings.periodMs=20;
                settings.currentTest=currentOnly;
                FakeState state;state.fault=scenario;FaultWorker worker(settings,state);
                QString failure;int samples=0;
                QObject::connect(&worker,&ControlThread::failed,[&](QString text){failure=text;});
                QObject::connect(&worker,&ControlThread::sample,[&](double,double,double,double,double){
                    if(++samples>=5 && scenario==Fault::None)worker.requestStop();
                });
                worker.start();
                if(!worker.wait(5000)) {worker.requestStop();worker.wait();throw std::runtime_error("Fault worker did not stop");}
                require((scenario==Fault::None)==failure.isEmpty(),
                        QString("Production control thread detects fault %1, current-only=%2").arg(static_cast<int>(scenario)).arg(currentOnly));
                require(state.closed && !state.on[0] && !state.on[1]
                        && state.amps==(state.commands.isEmpty()?0:state.commands.last()),
                        "Worker exception/stop path disables both outputs and closes transport, preserving last setpoint");
                require(state.enables==1 && state.configurations==1,"Fault never triggers automatic restart or reconfiguration");
                if(scenario==Fault::HighFeedback || scenario==Fault::LowFeedback || scenario==Fault::LockLost)
                    require(state.commands.isEmpty(),"Bad feedback/lost lock prevents any new current command");
                if(scenario==Fault::Overcurrent || scenario==Fault::HardwareOff)
                    require(state.measurements==1 && state.commands==QList<double>{0.001},
                            "Single-sample fault closes output without another current command");
                if(scenario==Fault::None)
                    require(samples>=5,"Normal continuous output continues until user stop");
            }
        }
        {
            Settings settings;settings.mode=2;settings.resource="FAKE-NO-HARDWARE";
            settings.limitMA=1;settings.manual=true;settings.manualMA=1;
            FakeState state;state.fault=Fault::HighFeedback;FaultSession session(settings,state);
            bool started=false;QString failure;int normalStops=0;
            QObject::connect(&session,&ControlThread::connected,[&](QString){started=session.startControl(settings);});
            QObject::connect(&session,&ControlThread::failed,[&](QString text){failure=text;});
            QObject::connect(&session,&ControlThread::runStopped,[&]{++normalStops;});
            session.start();
            if(!session.wait(5000)){session.disconnectDevice();session.wait();throw std::runtime_error("Fault session did not stop");}
            require(started && !failure.isEmpty() && normalStops==0,"GUI session propagates protection as a fault, not a normal stop");
            require(state.closed && !state.on[0] && !state.on[1] && state.commands.isEmpty(),
                    "GUI session fault path closes both outputs before releasing the connection");
            require(!session.startControl(settings),"Faulted GUI session rejects restart until a new connection is made");
        }

        // Cross-process, renamed executable: test the actual Windows kernel lock.
        const QString key="protection-test:"+QUuid::createUuid().toString();
        ProcessLock owner;owner.acquire(key);
        QTemporaryDir directory;
        require(directory.isValid(),"Create process-lock test directory");
        const QString renamed=directory.filePath("renamed-control-test.exe");
        require(QFile::copy(QCoreApplication::applicationFilePath(),renamed),"Copy renamed test executable");
        auto probe=[&] {
            QProcess child;
            // A renamed copy in a temp directory still needs the original package's DLLs/plugins.
            auto environment=QProcessEnvironment::systemEnvironment();
            environment.insert("PATH",QCoreApplication::applicationDirPath()+QDir::listSeparator()+environment.value("PATH"));
            environment.insert("QT_PLUGIN_PATH",QCoreApplication::libraryPaths().join(QDir::listSeparator()));
            child.setProcessEnvironment(environment);child.start(renamed,{"--lock-probe",key});
            if (!child.waitForStarted(5000) || !child.waitForFinished(10000)) {
                child.kill();child.waitForFinished();throw std::runtime_error("Lock test child failed to finish");
            }
            if (child.exitStatus()!=QProcess::NormalExit) throw std::runtime_error("Lock test child crashed");
            return child.exitCode();
        };
        require(probe()==23,"Second process cannot acquire a held lock, even with a renamed executable");
        owner.release();
        require(probe()==0,"Another process can acquire after explicit release");

        // IVI simulation only. The simulation does not emulate physical trip timing.
        for(int scenario=0;scenario<6;++scenario) {
            ShutdownProbe instrument;instrument.open("ShutdownOrderSimulationOnly",true);
            const double lastSetting=scenario%2 ? -0.001 : 0.001;
            instrument.configure(2,0.1,false,false,1);instrument.enable();instrument.setCurrent(lastSetting);
            instrument.forbidCurrentWrites=true;
            if(scenario==1)instrument.failDisableChannel=2;
            if(scenario==2)instrument.failDisableChannel=1;
            if(scenario==3)instrument.failReadChannel=2;
            if(scenario==4)instrument.failReadChannel=1;
            if(scenario==5)instrument.reportEnabledChannel=2;
            instrument.calls.clear();const auto errors=instrument.stopOutputs();
            const QStringList expected{"off2","off1","read2","read1"};
            require(instrument.calls==expected,QString("Shutdown %1: only OFF commands and status reads, no current write").arg(scenario));
            require(errors.isEmpty()==(scenario==0),"Shutdown failures are reported, not hidden");
            require(std::abs(instrument.currentSetting()-lastSetting)<1e-10,"Shutdown preserves last signed current setpoint even on failure");
            instrument.failDisableChannel=instrument.failReadChannel=instrument.reportEnabledChannel=0;
            require(instrument.stopOutputs().isEmpty(),"Clear injected fault and confirm outputs OFF");
            instrument.calls.clear();
            require(instrument.shutdown().isEmpty(),"Close shutdown sequence test session");
            require(!instrument.calls.contains("setCurrent"),"Disconnect does not write a zero or other current setpoint");
        }
        for(double lastSetting : {-0.001,0.001}) {
            ShutdownProbe instrument;instrument.open("DirectDisconnectSimulationOnly",true);
            instrument.configure(2,0.1,true,false,1);instrument.enable();instrument.setCurrent(lastSetting);
            instrument.forbidCurrentWrites=true;instrument.calls.clear();
            require(instrument.shutdown().isEmpty(),"Disconnect an active output without first stopping separately");
            require(instrument.calls==QStringList{"off2","off1","read2","read1"},
                    "Active disconnect closes outputs and does not modify current setpoint");
        }
        for (bool currentOnly : {false,true}) {
            Instrument instrument;
            instrument.open("ProtectionSimulationOnly",true);
            instrument.configure(2,0.1,currentOnly,false,1);
            instrument.verifyOutputProtection();
            require(true,"Read back hardware protection ON, auto-on OFF, auto-off OFF on both channels");
            instrument.enable();instrument.setCurrent(0.001);
            require(std::abs(instrument.currentSetting()-0.001)<1e-10,"1 mA is sent as 0.001 A");
            require(rejects([&]{instrument.setCurrent(0.0011);}),"Driver wrapper enforces configured 1 mA limit as well as absolute 10 mA cap");
            require(rejects([&]{instrument.setCurrent(-0.0011);}),"Negative command cannot bypass driver limit");
            instrument.readCurrent();
            require(instrument.outputEnabled(2),"Acquisition keeps continuous output enabled");
            MeasurementProtection fault(1,100);
            require(rejects([&]{fault.checkCurrent(1,2,0);}),"Injected measured overcurrent requests stop");
            require(instrument.stopOutputs().isEmpty(),"Fault cleanup succeeds in IVI simulation");
            require(!instrument.outputEnabled(1) && !instrument.outputEnabled(2) && std::abs(instrument.currentSetting()-0.001)<1e-10,
                    "Fault cleanup verifies both outputs OFF and preserves the 1 mA setpoint");
            require(rejects([&]{instrument.checkCompliance();}),"Unexpected output OFF is detected");
            instrument.readCurrent();
            require(!instrument.outputEnabled(1) && !instrument.outputEnabled(2),"Sampling does not automatically reopen stopped output");
            require(rejects([&]{instrument.enable();}),"Cannot re-enable stopped output without a new configuration");
            require(instrument.shutdown().isEmpty(),"Protected simulation session closes cleanly");
        }
        results << "LIMIT: No real instrument contacted; physical compliance trip and competing external clients require bench validation.";
    } catch(const std::exception& e) {results << "FAIL " + QString::fromUtf8(e.what());exitCode=1;}
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
    QFile file(QDir(output).filePath("protection-test.txt"));
    if(file.open(QIODevice::WriteOnly|QIODevice::Text)) {QTextStream stream(&file);stream.setCodec("UTF-8");stream<<results.join('\n')<<'\n';}
    return exitCode;
}

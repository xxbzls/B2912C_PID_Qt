#include "controller.h"
#include "instrument.h"
#include "window.h"
#include "traceplot.h"
#include <QtWidgets>
#include <QScopeGuard>
#include <atomic>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void waitUntil(const std::function<bool()>& done, int timeout=6000) {
    QElapsedTimer timer;timer.start();
    while(!done()) {
        if(timer.elapsed()>timeout) throw std::runtime_error("Monitoring test timed out");
        QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);
    }
}
struct State {
    std::atomic_bool ch1{false},ch2{false},closed{false};
    std::atomic_int prepared{0},configured{0},enabled{0},commands{0},currentReads{0},fault{0};
    std::atomic<double> amps{-.001},voltage{2};
};
class Transport final : public Instrument {
public:
    explicit Transport(State& state):s(state) {}
    QString open(const QString&,bool simulation,bool) override {
        if(simulation) throw std::runtime_error("Fake transport must exercise real-mode protection");
        return "B2912C MONITOR TEST; no hardware";
    }
    void prepareMonitoring(double) override {++s.prepared;s.ch1=true;}
    void configure(double,double,bool,bool,double) override {++s.configured;s.ch1=true;s.ch2=false;}
    void enable() override {++s.enabled;s.ch2=true;}
    bool outputEnabled(int channel) override {return channel==1 ? s.ch1.load() : s.ch2.load();}
    double outputVoltageLimit() override {return 2;}
    void verifyControlOwnership() override {
        if(s.fault==4) throw std::runtime_error("Injected lost ownership");
    }
    void checkMonitoring() override {
        verifyControlOwnership();
        if(s.fault==5) s.ch1=false;
        if(s.fault==6) s.ch2=true;
        if(!s.ch1 || s.ch2) throw std::runtime_error("Unexpected monitoring output state");
    }
    void checkCompliance() override {
        verifyControlOwnership();
        if(!s.ch1 || !s.ch2) throw std::runtime_error("Control output disabled");
    }
    double readVoltage() override {
        if(!s.ch1) throw std::runtime_error("CH1 voltage read while relay open");
        if(s.fault==1) return 15.001;
        if(s.fault==2) return -15.001;
        if(s.fault==3) return std::numeric_limits<double>::quiet_NaN();
        if(s.fault==7) throw std::runtime_error("Injected acquisition I/O failure");
        if(!s.ch2) s.voltage=s.voltage.load()*.9; // Visible return after stopping, independent of setpoint.
        return s.voltage;
    }
    double readCurrent() override {
        if(!s.ch2) throw std::runtime_error("Cannot report open-relay current as load measurement");
        ++s.currentReads;return s.amps;
    }
    void setCurrent(double amps) override {
        if(!s.ch2 || std::abs(amps)>.01) throw std::runtime_error("Invalid current command");
        ++s.commands;s.amps=amps;s.voltage=amps*1000;
    }
    QStringList stopControlOutput() override {
        if(s.fault==8) return {"Injected CH2 OFF failure"};
        s.ch2=false;return {};
    }
    QStringList shutdown() override {s.ch2=false;s.ch1=false;s.closed=true;return {};}
private:
    State& s;
};
class MonitorSession final : public SessionThread {
public:
    MonitorSession(Settings settings,State& state):SessionThread(settings),s(state) {}
    ~MonitorSession() override {disconnectDevice();wait();}
protected:
    std::unique_ptr<Instrument> createInstrument() const override {return std::make_unique<Transport>(s);}
private:
    State& s;
};
class ShutdownAudit final : public Instrument {
public:
    QStringList operations;
    void tripFeedbackForTest() {disableOutput(1);}
    bool outputEnabled(int channel) override {
        operations<<QString("read%1").arg(channel);return Instrument::outputEnabled(channel);
    }
    void setCurrent(double amps) override {operations<<"setCurrent";Instrument::setCurrent(amps);}
protected:
    void disableOutput(int channel) override {operations<<QString("off%1").arg(channel);Instrument::disableOutput(channel);}
};
}

int monitorTest() {
    QStringList checks;int result=0;
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
    auto require=[&](bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());checks<<"PASS "+message;};
    auto rejects=[](const std::function<void()>& action){try{action();}catch(const std::exception&){return true;}return false;};
    try {
        Settings settings;settings.mode=2;settings.resource="MONITOR-FAKE";settings.periodMs=20;
        settings.manual=true;settings.manualMA=1;
        {
            State state;MonitorSession session(settings,state);QObject receiver;
            int idle=0,running=0,stopped=0;double voltage=0,idleTime=0,runTime=0;QStringList errors,messages;
            QObject::connect(&session,&ControlThread::message,&receiver,[&](QString text){messages<<text;});
            QObject::connect(&session,&ControlThread::monitorSample,&receiver,[&](double t,double v,double){++idle;voltage=v;idleTime=t;});
            QObject::connect(&session,&ControlThread::sample,&receiver,[&](double t,double,double,double,double,double){++running;runTime=t;});
            QObject::connect(&session,&ControlThread::runStopped,&receiver,[&]{++stopped;});
            QObject::connect(&session,&ControlThread::failed,&receiver,[&](QString e){errors<<e;});
            session.start();waitUntil([&]{return idle>=3;});
            require(state.ch1 && !state.ch2 && state.prepared==1 && state.configured==0
                && state.enabled==0 && state.commands==0 && state.currentReads==0,
                "Connection continuously measures CH1 without starting PID, enabling CH2, writing its setpoint or reading invalid current");
            require(session.startControl(settings),"Start control on established monitoring connection");
            waitUntil([&]{return running>=3;});session.requestStop();waitUntil([&]{return stopped==1;});
            require(messages.join('\n').contains("手动固定输出 CH2 1.0000 mA")
                && !messages.join('\n').contains("PID 输出 CH2 电流"),
                "Manual startup log identifies fixed current and retained CH1 acquisition without claiming PID control");
            const int beforeIdle=idle,commands=state.commands,currentReads=state.currentReads;
            const double lastSetpoint=state.amps;waitUntil([&]{return idle>=beforeIdle+4;});
            require(state.ch1 && !state.ch2 && state.commands==commands && state.currentReads==currentReads
                && state.amps==lastSetpoint && voltage<1 && idleTime>runTime && state.prepared==1,
                "Normal stop retains CH1, observes return, keeps time increasing and preserves last setpoint without current writes/reads or re-enabling");
            settings.manualMA=-.5;const int previousRunning=running;
            require(session.startControl(settings),"Restart uses same connection");waitUntil([&]{return running>=previousRunning+3;});
            session.requestStop();waitUntil([&]{return stopped==2;});
            const int beforeNegative=idle;waitUntil([&]{return idle>=beforeNegative+3;});
            require(state.amps==-.0005 && voltage<0 && voltage>-.5 && state.enabled==2 && state.prepared==1,
                "Negative output stops directly and keeps monitoring without zeroing the last command");
            session.disconnectDevice();waitUntil([&]{return !session.isRunning();});session.wait();
            require(state.closed && !state.ch1 && !state.ch2 && errors.isEmpty(),"Disconnect closes both channels and ends monitoring");
        }
        for(int fault=1;fault<=8;++fault) {
            State state;MonitorSession session(settings,state);QObject receiver;int idle=0,running=0,stopped=0;QStringList errors;
            QObject::connect(&session,&ControlThread::monitorSample,&receiver,[&]{++idle;});
            QObject::connect(&session,&ControlThread::sample,&receiver,[&]{++running;});
            QObject::connect(&session,&ControlThread::runStopped,&receiver,[&]{++stopped;});
            QObject::connect(&session,&ControlThread::failed,&receiver,[&](QString e){errors<<e;});
            session.start();waitUntil([&]{return idle>=2;});
            if(fault==8) {session.startControl(settings);waitUntil([&]{return running>=2;});}
            state.fault=fault;if(fault==8)session.requestStop();
            waitUntil([&]{return !session.isRunning();});session.wait();QCoreApplication::processEvents();
            require(!errors.isEmpty() && state.closed && !state.ch1 && !state.ch2 && state.prepared==1 && stopped==0
                && !session.startControl(settings),QString("Fault %1 closes both channels, reports failure and never resumes monitoring or control automatically").arg(fault));
        }

        // Explicit IVI simulation only: exercise actual driver selectors and production output sequencing.
        ShutdownAudit instrument;instrument.open("MonitorDriverSimulationOnly",true);
        instrument.configure(3.5,.1);instrument.enable();instrument.setCurrent(-.001);
        require(rejects([&]{instrument.prepareMonitoring(.1);}) && instrument.outputEnabled(2),
            "Monitoring refuses to take over enabled channels without changing their outputs");
        require(instrument.stopOutputs().isEmpty(),"Prepare driver-simulated channels OFF");
        instrument.prepareMonitoring(.1);
        require(instrument.outputEnabled(1) && !instrument.outputEnabled(2)
            && std::abs(instrument.currentSetting()+.001)<1e-12 && std::abs(instrument.outputVoltageLimit()-3.5)<1e-12,
            "Driver monitor enables only CH1 and preserves CH2 signed setpoint and compliance without reset");
        require(rejects([&]{instrument.enable();}) && rejects([&]{instrument.setCurrent(0);}),
            "Monitoring alone cannot enable CH2 or accept current commands");
        for(int i=0;i<3;++i){instrument.checkMonitoring();instrument.readVoltage();}
        require(!instrument.outputEnabled(2),"Repeated acquisition does not automatically turn CH2 on");
        instrument.configure(3.5,.1);instrument.enable();instrument.setCurrent(.001);
        instrument.operations.clear();require(instrument.stopControlOutput().isEmpty(),"Normal stop succeeds");
        require(instrument.operations==QStringList{"off2","read2"},"Normal stop sends CH2 OFF first, verifies OFF, and sends no other output/current command");
        instrument.checkMonitoring();instrument.readVoltage();
        require(instrument.outputEnabled(1) && !instrument.outputEnabled(2) && std::abs(instrument.currentSetting()-.001)<1e-12,
            "Driver preserves CH1 measurement and last current setting after stop");
        instrument.configure(3.5,.1);
        require(instrument.outputEnabled(1) && !instrument.outputEnabled(2),
            "Cancelled startup can return to monitoring after configuration without enabling CH2");
        instrument.tripFeedbackForTest();
        require(rejects([&]{instrument.enable();}) && !instrument.outputEnabled(1) && !instrument.outputEnabled(2),
            "Startup refuses to re-enable CH1 after an intervening protection shutdown");
        instrument.operations.clear();require(instrument.shutdown().isEmpty(),"Disconnect succeeds");
        require(instrument.operations==QStringList{"off2","off1","read2","read1"},"Disconnect still shuts down and verifies both channels, without current writes");

        Window window(Window::Backend::DriverSimulationForTest);
        const auto cleanup=qScopeGuard([&]{window.close();for(auto* t:window.findChildren<SessionThread*>()){t->disconnectDevice();t->wait();}});
        auto* start=window.findChild<QPushButton*>("startControlButton");
        auto* probe=window.findChild<TracePlot*>("probePlot");auto* current=window.findChild<TracePlot*>("currentPlot");
        auto* probeValue=window.findChild<QLabel*>("probeReading");auto* currentValue=window.findChild<QLabel*>("measuredReading");
        auto* outputValue=window.findChild<QLabel*>("outputReading");auto* status=window.findChild<QLabel*>("measurementValues");
        window.show();window.testConnection();waitUntil([&]{return start->isEnabled() && probe->graph(0)->data()->size()>=3;});
        require(window.sampleCount==0 && probeValue->text()!="—" && outputValue->text()=="已关断" && currentValue->text()=="不可用"
            && current->graph(0)->data()->isEmpty(),"Connected UI shows fresh probe readings and clearly unavailable current without fabricated current samples");
        window.grab().save(QDir(output).filePath("monitor-connected.png"));
        window.start();waitUntil([&]{return window.sampleCount>=4 && !current->graph(0)->data()->isEmpty();});
        require(currentValue->text()!="不可用" && outputValue->text()!="已关断","Running UI displays acquired current and control command");
        window.stop();waitUntil([&]{return start->isEnabled();});
        const int voltageCount=probe->graph(0)->data()->size(),currentCount=current->graph(0)->data()->size(),controlCount=window.sampleCount;
        waitUntil([&]{return probe->graph(0)->data()->size()>=voltageCount+3;});
        require(outputValue->text()=="已关断" && currentValue->text()=="不可用" && status->text().contains("持续采集")
            && current->graph(0)->data()->size()==currentCount && window.sampleCount==controlCount,
            "Stopped UI keeps updating voltage while freezing labeled current history and not recording fake current samples");
        window.grab().save(QDir(output).filePath("monitor-stopped.png"));
        window.resize(1100,680);QCoreApplication::processEvents();window.grab().save(QDir(output).filePath("monitor-stopped-compact.png"));
        window.testConnection();waitUntil([&]{return window.findChild<QLineEdit*>("visaAddress")->isEnabled();});
        require(probeValue->text()=="—" && currentValue->text()=="—" && !start->isEnabled(),"Disconnect invalidates live readings and disables control");
        require(!window.hadError,"Monitoring GUI finishes without error");
        checks<<"LIMIT: No real instrument contacted; external CH1 voltage and physical CH2 OFF behavior need bench verification.";
    } catch(const std::exception& e) {checks<<"FAIL "+QString::fromUtf8(e.what());result=1;}
    QFile file(QDir(output).filePath("monitor-test.txt"));
    if(file.open(QIODevice::WriteOnly)){QTextStream stream(&file);stream.setCodec("UTF-8");stream<<checks.join('\n');}
    return result;
}

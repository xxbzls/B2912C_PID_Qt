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
void waitUntil(const std::function<bool()>& done, int timeout=5000) {
    QElapsedTimer clock;clock.start();
    while(!done()) {
        if(clock.elapsed()>timeout) throw std::runtime_error("Timed out waiting for asynchronous target test");
        QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        QThread::msleep(2);
    }
}
struct State {
    std::atomic_int opens{0},configures{0},enables{0},stops{0},fault{0};
    std::atomic_int slowReads{0},slowReadMs{0};
    std::atomic_bool blockRead{false},inRead{false};
    bool on=false; // Remaining state is accessed only by the worker, or after joining it.
    double amps=0;
    QVector<double> commands;
};
class Resistor final : public Instrument {
public:
    explicit Resistor(State& state):s(state) {}
    QString open(const QString&,bool simulation,bool) override {
        if(simulation) throw std::runtime_error("Test double must exercise real-mode protection");
        ++s.opens;return "B2912C TEST DOUBLE; no physical I/O";
    }
    void configure(double,double,bool,bool,double) override {++s.configures;s.amps=0;s.on=false;}
    void enable() override {++s.enables;s.on=true;}
    void prepareMonitoring(double) override {}
    void checkMonitoring() override {
        verifyControlOwnership();
        if(s.on) throw std::runtime_error("CH2 enabled while monitoring");
    }
    QStringList stopControlOutput() override {return stopOutputs();}
    double outputVoltageLimit() override {return 10;}
    bool outputEnabled(int) override {return s.on;}
    double readVoltage() override {
        s.inRead=true;
        while(s.blockRead) QThread::msleep(1);
        int remaining=s.slowReads.load();
        while(remaining>0 && !s.slowReads.compare_exchange_weak(remaining,remaining-1)) {}
        if(remaining>0) QThread::msleep(static_cast<unsigned long>(s.slowReadMs.load()));
        s.inRead=false;
        return s.fault==1 ? 15.1 : s.amps*1000;
    }
    double readCurrent() override {return s.fault==2 ? 0.02 : s.amps;}
    double readOutputVoltage() override {return s.amps*1000;}
    void setCurrent(double amps) override {
        if(!s.on) throw std::runtime_error("Command sent while output disabled");
        s.amps=amps;s.commands.append(amps);
    }
    void checkCompliance() override {if(!s.on || s.fault==3) throw std::runtime_error("Hardware protection trip");}
    void verifyControlOwnership() override {if(s.fault==4) throw std::runtime_error("Device lock lost");}
    QStringList stopOutputs() override {++s.stops;s.on=false;return {};}
    QStringList shutdown() override {return stopOutputs();}
private:
    State& s;
};
class TestSession final : public SessionThread {
public:
    TestSession(Settings settings,State& state):SessionThread(settings),s(state) {}
    ~TestSession() override {s.blockRead=false;disconnectDevice();wait();}
protected:
    std::unique_ptr<Instrument> createInstrument() const override {return std::make_unique<Resistor>(s);}
private:
    State& s;
};
struct Sample {double time,voltage,current,target;};
}

int targetTest() {
    QStringList results;int result=0;
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
    auto require=[&](bool ok,const QString& text){if(!ok)throw std::runtime_error(text.toStdString());results<<"PASS "+text;};
    try {
        Settings math;math.targetV=4;math.kp=.2;math.ti=.1;math.td=0;
        VoltagePid pid(math);double volts=0;
        for(int i=0;i<200;++i) volts=pid.step(volts,.1); // Ideal 1 kohm: mA = V.
        require(std::abs(volts-4)<.001,"PI settles at original target");
        pid.setTargetVoltage(6);
        const double first=pid.step(volts,.1);
        require(first>4 && first<5,"Retarget preserves accumulated integral instead of restarting from zero");
        volts=first;
        for(double target : {6.,-4.,0.,10.,-10.}) {
            pid.setTargetVoltage(target);
            for(int i=0;i<300;++i) {
                volts=pid.step(volts,.1);
                if(std::abs(volts)>10) throw std::runtime_error("PID command exceeds current clamp");
            }
            require(std::abs(volts-target)<.001,QString("PI converges after target change to %1 V within current limit").arg(target));
        }
        for(double target : {10.001,-10.001,std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity()}) {
            bool rejected=false;try{pid.setTargetVoltage(target);}catch(const std::exception&){rejected=true;}
            require(rejected,"Invalid runtime target rejected");
        }
        math.ti=0;math.td=.2;VoltagePid derivative(math);derivative.step(1,.1);
        derivative.setTargetVoltage(6);
        require(std::abs(derivative.step(1,.1)-1)<1e-5,"Target step does not create a derivative kick");
        math.limitMA=1;math.ti=.1;math.td=0;VoltagePid clamped(math);clamped.setTargetVoltage(10);
        require(clamped.step(0,.1)<=1,"Smaller user current limit still applies to new targets");

        Settings s;s.mode=2;s.resource="FAKE_ONLY";s.targetV=4;s.kp=.2;s.ti=.1;s.periodMs=20;
        for(int fault=1;fault<=4;++fault) {
            State state;TestSession session(s,state);QObject receiver;
            QVector<Sample> samples;QVector<double> applied;QStringList errors;bool connected=false;
            QObject::connect(&session,&ControlThread::connected,&receiver,[&]{connected=true;});
            QObject::connect(&session,&ControlThread::sample,&receiver,[&](double t,double v,double i,double,double,double target){samples.append({t,v,i,target});});
            QObject::connect(&session,&ControlThread::targetApplied,&receiver,[&](double target){applied.append(target);});
            QObject::connect(&session,&ControlThread::failed,&receiver,[&](QString error){errors<<error;});
            require(!session.requestTargetVoltage(6),"Idle session rejects target updates");
            session.start();waitUntil([&]{return connected;});require(session.startControl(s),"Start persistent control session");
            waitUntil([&]{return samples.size()>=5;});
            require(!session.requestTargetVoltage(10.001) && !session.requestTargetVoltage(-10.001)
                && !session.requestTargetVoltage(std::numeric_limits<double>::quiet_NaN()),"Worker mailbox rejects out-of-range and nonfinite targets");
            const double previousTime=samples.last().time;
            require(session.requestTargetVoltage(6),"GUI thread submits live target to worker mailbox");
            waitUntil([&]{return !applied.isEmpty() && samples.last().target==6;});
            require(applied==QVector<double>{6} && samples.last().time>previousTime
                && state.opens==1 && state.configures==1 && state.enables==1 && state.stops==0,
                "Target acknowledged with continuous clock, one connection and no reconfigure/enable/off");
            state.fault=fault;
            waitUntil([&]{return !session.isRunning();});QCoreApplication::processEvents();session.wait();
            require(!errors.isEmpty() && !state.on && state.stops>0 && !session.requestTargetVoltage(2),
                QString("Retarget keeps protection active and shuts down on fault %1").arg(fault));
            for(double amps : state.commands) if(std::abs(amps)>.01) throw std::runtime_error("Transport received more than 10 mA");
        }
        {
            s.manual=false;s.currentTest=false;
            State state;TestSession session(s,state);QObject receiver;
            int samples=0,stopped=0;bool connected=false;QVector<double> applied,targets;
            QObject::connect(&session,&ControlThread::connected,&receiver,[&]{connected=true;});
            QObject::connect(&session,&ControlThread::sample,&receiver,[&](double,double,double,double,double,double target){++samples;targets<<target;});
            QObject::connect(&session,&ControlThread::targetApplied,&receiver,[&](double target){applied<<target;});
            QObject::connect(&session,&ControlThread::runStopped,&receiver,[&]{++stopped;});
            session.start();waitUntil([&]{return connected;});session.startControl(s);waitUntil([&]{return samples>=2;});
            state.blockRead=true;waitUntil([&]{return state.inRead.load();});
            require(session.requestTargetVoltage(-4) && !session.requestTargetVoltage(6),"Only one target can wait for application");
            session.requestStop();state.blockRead=false;waitUntil([&]{return stopped==1;});
            require(applied.isEmpty() && !session.requestTargetVoltage(6),"Stop discards pending target and rejects further updates");
            const int count=samples;s.targetV=2;session.startControl(s);waitUntil([&]{return samples>=count+2;});
            require(targets.last()==2 && applied.isEmpty(),"Restart uses startup target, without stale mailbox data");
            session.requestStop();waitUntil([&]{return stopped==2;});
            for(int mode=0;mode<2;++mode) {
                s.manual=mode==0;s.currentTest=mode==1;
                const int before=samples;session.startControl(s);waitUntil([&]{return samples>=before+2;});
                require(!session.requestTargetVoltage(6),"Manual and current-test runs reject online PID targets");
                session.requestStop();waitUntil([&]{return stopped==3+mode;});
            }
            require(state.opens==1,"Stop and restart reuse the established connection");
        }
        {
            s.manual=false;s.currentTest=false;
            State state;TestSession session(s,state);QObject receiver;
            int samples=0;bool connected=false;QStringList messages,errors;
            QObject::connect(&session,&ControlThread::connected,&receiver,[&]{connected=true;});
            QObject::connect(&session,&ControlThread::sample,&receiver,[&](double,double,double,double,double,double){++samples;});
            QObject::connect(&session,&ControlThread::message,&receiver,[&](QString text){messages<<text;});
            QObject::connect(&session,&ControlThread::failed,&receiver,[&](QString text){errors<<text;});
            session.start();waitUntil([&]{return connected;});require(session.startControl(s),"Start delay recovery session");
            waitUntil([&]{return samples>=3;});
            state.slowReadMs=1100;state.slowReads=1;
            waitUntil([&]{return messages.join('\n').contains("采样延迟告警") && samples>=4;},4000);
            require(session.isRunning() && errors.isEmpty(),"One successful delayed sample holds the previous command without terminating the session");
            waitUntil([&]{return messages.join('\n').contains("采样延迟已恢复");},3000);
            state.slowReads=3;
            waitUntil([&]{return !session.isRunning();},6000);QCoreApplication::processEvents();session.wait();
            require(errors.join('\n').contains("连续采样延迟") && !state.on && state.stops>0,
                "Three consecutive delayed samples stop control and run the shutdown path");
        }

        // UI uses only the driver's explicit simulation mode, never physical hardware.
        Window window(Window::Backend::DriverSimulationForTest);
        const auto cleanup=qScopeGuard([&]{
            window.close();
            for(auto* thread : window.findChildren<SessionThread*>()) {thread->disconnectDevice();thread->wait();}
        });
        auto* address=window.findChild<QLineEdit*>("visaAddress");
        auto* target=window.findChild<QDoubleSpinBox*>("targetVoltage");
        auto* kp=window.findChild<QDoubleSpinBox*>("proportionalGain");
        auto* apply=window.findChild<QPushButton*>("applyTargetButton");
        auto* targetReading=window.findChild<QLabel*>("targetReading");
        auto* plot=window.findChild<QCustomPlot*>("probePlot");
        auto* start=window.findChild<QPushButton*>("startControlButton");
        auto* clear=window.findChild<QPushButton*>("clearCurveButton");
        target->setValue(4);kp->setValue(.2);
        window.show();window.testConnection();waitUntil([&]{return start->isEnabled();});
        require(!apply->isEnabled(),"Apply disabled before PID startup");
        window.start();waitUntil([&]{return window.sampleCount>=3;});
        require(target->isEnabled() && apply->isEnabled() && !kp->isEnabled() && !address->isEnabled(),"Only target remains editable while PID runs");
        for(auto* spin : window.findChildren<QDoubleSpinBox*>())
            if(spin!=target && spin->isEnabled()) throw std::runtime_error("Other numeric setting enabled during PID");
        target->setValue(6);const int beforeEdit=window.sampleCount;
        waitUntil([&]{return window.sampleCount>=beforeEdit+2;});
        require(targetReading->text()=="4.00000" && (plot->graph(1)->data()->constEnd()-1)->value==4,
            "Editing target without applying does not change active target or curve");
        waitUntil([&]{return !plot->graph(0)->data()->isEmpty();});
        const int beforeApply=plot->graph(0)->data()->size();
        const double firstTime=plot->graph(0)->data()->constBegin()->key;
        apply->click();require(!apply->isEnabled(),"Apply waits for worker acknowledgment");
        waitUntil([&]{return targetReading->text()=="6.00000" && apply->isEnabled()
            && plot->graph(0)->data()->size()>beforeApply;});
        require(plot->graph(0)->data()->size()>beforeApply && plot->graph(0)->data()->constBegin()->key==firstTime
            && plot->graph(1)->data()->constBegin()->value==4 && (plot->graph(1)->data()->constEnd()-1)->value==6,
            "UI confirms applied target while preserving earlier samples and target history");
        window.grab().save(QDir(output).filePath("target-gui-smoke.png"));
        clear->click();require(plot->graph(0)->data()->isEmpty() && (plot->graph(1)->data()->constEnd()-1)->value==6,
            "Clear curve during PID keeps applied target and leaves control running");
        window.stop();require(!apply->isEnabled() && !target->isEnabled(),"Stop immediately locks runtime target controls");
        waitUntil([&]{return start->isEnabled();});
        require(target->isEnabled() && kp->isEnabled() && !apply->isEnabled(),"Stopped state restores parameter editing");
        window.start(true);waitUntil([&]{return window.sampleCount>=2;});
        require(!target->isEnabled() && !apply->isEnabled(),"Manual UI cannot apply PID targets");
        window.stop();waitUntil([&]{return start->isEnabled();});
        require(!window.hadError,"Driver-simulated UI run completes without errors");
        window.testConnection();waitUntil([&]{return address->isEnabled();});
    } catch(const std::exception& e) {results<<"FAIL "+QString::fromUtf8(e.what());result=1;}
    QFile file(QDir(output).filePath("target-test.txt"));
    if(file.open(QIODevice::WriteOnly)) {QTextStream stream(&file);stream.setCodec("UTF-8");stream<<results.join('\n');}
    return result;
}

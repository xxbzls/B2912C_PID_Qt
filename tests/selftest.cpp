#include "controller.h"
#include "instrument.h"
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <cmath>
#include <limits>
#include <stdexcept>

int selftest(bool sdk) {
    QStringList results;
    auto require=[&](bool ok,QString name){if(!ok)throw std::runtime_error(name.toStdString());results<<"PASS "+name;};
    int exitCode=0;
    try {
        if(sdk) {
            Instrument instrument;
            instrument.log=[&](QString s){results<<s;};
            results << instrument.open("SimulationOnly",true); // Cannot select real I/O via this test.
            require(instrument.outputVoltageLimit()==2.0,"New simulated session has 2 V compliance");
            instrument.configure(5,0.1);instrument.enable();
            for(double amps:{0.001,-0.001,0.0}){
                instrument.setCurrent(amps);
                require(std::abs(instrument.currentSetting()-amps)<1e-10,QString("SDK current set/readback %1 A").arg(amps));
                require(std::isfinite(instrument.readVoltage()) && std::isfinite(instrument.readCurrent())
                        && std::isfinite(instrument.readOutputVoltage()),"SDK acquire / wait / fetch for CH1 and CH2");
                require(std::abs(instrument.currentSetting()-amps)<1e-10,"SDK sampling preserves current setpoint");
            }
            require(std::isfinite(instrument.readVoltage()),"SDK CH1 voltage read");
            require(std::isfinite(instrument.readCurrent()),"SDK CH2 current read");
            instrument.checkCompliance();results<<"PASS SDK compliance query";
            bool rejected=false;try{instrument.setCurrent(0.011);}catch(...){rejected=true;}
            require(rejected,"Reject output above 10 mA");
            auto errors=instrument.shutdown();
            require(errors.isEmpty(),"SDK disable / close: "+errors.join("; "));
            results << instrument.open("CurrentTestSimulationOnly",true);
            instrument.configure(3.5,0.1,true);
            require(std::abs(instrument.outputVoltageLimit()-3.5)<1e-8,"Explicit override reads back 3.5 V");
            instrument.enable();
            require(!instrument.outputEnabled(1),"Current test keeps CH1 disabled");
            require(instrument.outputEnabled(2),"Current test enables CH2");
            for(double amps:{0.001,-0.001,0.0}) {
                instrument.setCurrent(amps);
                require(std::abs(instrument.currentSetting()-amps)<1e-10,"CH2-only positive / negative / zero setpoint");
                require(std::isfinite(instrument.readCurrent()) && std::isfinite(instrument.readOutputVoltage()),"CH2-only acquire / wait / fetch");
                require(std::abs(instrument.currentSetting()-amps)<1e-10,"CH2-only sampling preserves current setpoint");
            }
            require(std::isfinite(instrument.readCurrent()) && std::isfinite(instrument.readOutputVoltage()),"CH2-only current and terminal voltage measurement");
            rejected=false;try{instrument.readVoltage();}catch(...){rejected=true;}
            require(rejected,"CH1 feedback reads forbidden in current test");
            instrument.checkCompliance();
            instrument.setCurrent(0.0007);
            require(instrument.stopOutputs().isEmpty(),"Stop outputs while keeping IVI session open");
            require(!instrument.outputEnabled(1) && !instrument.outputEnabled(2),"Stop disables both channels without closing session");
            require(std::abs(instrument.currentSetting()-0.0007)<1e-10,"Stop preserves last positive current setpoint");
            instrument.configure(5,0.1,true,true);
            require(std::abs(instrument.outputVoltageLimit()-3.5)<1e-8,"Restart preserves 3.5 V across reset instead of stale 5 V");
            require(!instrument.outputEnabled(1) && !instrument.outputEnabled(2),"Preserved limit verified before outputs enabled");
            require(std::abs(instrument.currentSetting())<1e-10,"New run initializes current while outputs remain OFF");
            instrument.enable();instrument.setCurrent(-0.0001);
            require(instrument.outputEnabled(2) && !instrument.outputEnabled(1),"Restart in the same session keeps CH1 disabled");
            require(instrument.stopOutputs().isEmpty(),"Stop before switching to PID mode");
            require(std::abs(instrument.currentSetting()+0.0001)<1e-10,"Stop preserves last negative current setpoint");
            instrument.configure(std::numeric_limits<double>::quiet_NaN(),0.1,false,true);
            require(std::abs(instrument.outputVoltageLimit()-3.5)<1e-8,"PID mode preserves limit and ignores inactive override field");
            require(!instrument.outputEnabled(1) && !instrument.outputEnabled(2),"PID configuration keeps outputs off");
            require(instrument.stopOutputs().isEmpty(),"Stop after preserved PID configuration");
            rejected=false;try{instrument.configure(21,0.1,true,false);}catch(...){rejected=true;}
            require(rejected && !instrument.outputEnabled(1) && !instrument.outputEnabled(2),"Invalid override rejected before output");
            instrument.configure(7.25,0.1,true,false);
            require(std::abs(instrument.outputVoltageLimit()-7.25)<1e-8,"New explicit override replaces previous limit");
            require(instrument.shutdown().isEmpty(),"CH2-only disable / close");
            const QString identity=instrument.open("ReadOnlyProbeSimulation",true,false);
            require(identity.contains("B2912C") && identity.contains("序列号") && identity.contains("固件"),"Non-reset open returns instrument identity");
            require(!instrument.outputEnabled(1) && !instrument.outputEnabled(2),"Non-reset connection probe does not enable outputs");
            require(instrument.shutdown().isEmpty(),"Unconfigured probe closes session without output setup");
        } else {
            Settings defaults;
            require(!defaults.overrideVoltageLimit,"Voltage limit override is opt-in");
            defaults.voltageLimit=std::numeric_limits<double>::quiet_NaN();defaults.validate();
            require(true,"Inactive voltage limit is ignored");
            defaults.overrideVoltageLimit=true;
            bool badOverride=false;try{defaults.validate();}catch(...){badOverride=true;}
            require(badOverride,"Active invalid voltage limit is rejected");
            defaults=Settings();defaults.limitMA=10.001;
            bool badCurrent=false;try{defaults.validate();}catch(...){badCurrent=true;}
            require(badCurrent,"Customer current limit cannot exceed 10 mA");
            for(double targetV:{0.012,-0.012}) {
                Settings resistor;resistor.targetV=targetV;resistor.kp=20;resistor.ti=0.5;resistor.td=0;resistor.limitMA=2;
                VoltagePid resistorPid(resistor);double voltage=0,current=0;
                for(int k=0;k<300;++k) {
                    current=resistorPid.step(voltage,k%2 ? 0.11 : 0.09);
                    if(std::abs(current)>resistor.limitMA)throw std::runtime_error("12 ohm model exceeded 2 mA limit");
                    voltage=12.0*current/1000.0;
                }
                require(std::abs(voltage-targetV)<0.00001 && std::abs(current-targetV/0.012)<0.001,
                        QString("12 ohm ideal model: target %1 V converges to %2 mA within 2 mA limit").arg(targetV).arg(current));
            }
            for(int polarity:{1,-1}) {
                Settings s;s.direction=polarity;VoltagePid pid(s);double x=0,maxCurrent=0;
                for(int k=0;k<800;++k){double dt=(k%2==0)?0.08:0.12;double current=pid.step(x,dt);maxCurrent=std::max(maxCurrent,std::abs(current));double equilibrium=polarity*current;x=equilibrium+(x-equilibrium)*std::exp(-dt/0.8);}
                require(std::abs(x-s.targetV)<0.001,QString("Voltage PID converges with physical polarity %1").arg(polarity));
                require(maxCurrent<=s.limitMA,"Current clamp with varying sample interval");
            }
            Settings s;s.ti=0;s.td=1;s.targetV=2;VoltagePid derivative(s);
            require(std::abs(derivative.step(2,0.1))<1e-6,"No derivative kick on startup");
            s.td=0;s.kp=1000;VoltagePid saturated(s);require(saturated.step(-9,0.1)<=10,"P-only saturation");
            s.manual=true;s.manualMA=-0.2;VoltagePid manual(s);require(manual.step(0,0.1)==-0.2,"Manual current is physical signed current");
            bool rejected=false;
            for(double targetV:{-10.0,10.0}) {s=Settings();s.targetV=targetV;s.validate();}
            require(true,"Accept target voltage endpoints +/-10 V");
            for(double targetV:{-10.0001,10.0001}) {
                s=Settings();s.targetV=targetV;rejected=false;try{s.validate();}catch(...){rejected=true;}
                require(rejected,"Reject target voltage outside +/-10 V");
            }
            s=Settings();s.targetV=0;s.kp=2;s.ti=0;s.td=0;VoltagePid voltsToMilliamps(s);
            require(std::abs(voltsToMilliamps.step(1,0.1)+2)<1e-12,"0 V target - 1 V feedback at 2 mA/V gives -2 mA");
            require(std::abs(voltsToMilliamps.step(-1,0.1)-2)<1e-12,"0 V target - (-1 V) feedback at 2 mA/V gives +2 mA");
            s.limitMA=1;VoltagePid limited(s);
            require(limited.step(1,0.1)==-1 && limited.step(-1,0.1)==1,"Voltage PID output is clamped in mA");
            s=Settings();VoltagePid invalid(s);rejected=false;try{invalid.step(std::numeric_limits<double>::quiet_NaN(),0.1);}catch(...){rejected=true;}require(rejected,"Reject NaN feedback");
            rejected=false;try{invalid.step(0,0);}catch(...){rejected=true;}require(rejected,"Reject zero sample time");
            s=Settings();s.currentTest=true;s.manualMA=-0.1;s.limitMA=0.2;
            s.targetV=1000;s.kp=-1;s.direction=0;s.validate();
            require(true,"Current test does not depend on PID / target voltage / direction");
            s.manualMA=0.3;rejected=false;try{s.validate();}catch(...){rejected=true;}
            require(rejected,"Current test rejects positive setpoint above configured limit");
            s.manualMA=-0.3;rejected=false;try{s.validate();}catch(...){rejected=true;}
            require(rejected,"Current test rejects negative setpoint above configured limit");
        }
    }catch(const std::exception& e){results<<"FAIL "+QString::fromUtf8(e.what());exitCode=1;}
    QString path=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(path);
    QFile file(QDir(path).filePath(sdk?"sdk-test.txt":"pid-test.txt"));
    if(file.open(QIODevice::WriteOnly|QIODevice::Text)){QTextStream stream(&file);stream.setCodec("UTF-8");stream<<results.join('\n')<<'\n';}
    QTextStream(stdout)<<results.join('\n')<<'\n';return exitCode;
}

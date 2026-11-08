#include "window.h"
#include <QApplication>
#include <QIcon>
#include <QTimer>
#include <QFile>
#include <QTextStream>
#include <QDir>
#include "controller.h"
#include "instrument.h"
#include <stdexcept>
#include <QEventLoop>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QToolButton>
#include <QMessageBox>
#include "protection.h"
#include "traceplot.h"
int selftest(bool sdk);
int plotTest();
int protectionTest();
int targetTest();
int meetingTest();
int monitorTest();
int groupTest();
int measurementTest();
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    app.setApplicationDisplayName("探头闭环控制上位机v1.0");
    app.setApplicationVersion("1.0");
    app.setWindowIcon(QIcon(":/icons/probe-control.ico"));
    app.setFont(QFont("Microsoft YaHei UI",10));
    const auto args=app.arguments();
    if(args.contains("--lock-probe")) {
        const int index=args.indexOf("--lock-probe");
        if(index+1>=args.size()) return 24;
        try { ProcessLock probe; probe.acquire(args[index+1]); return 0; }
        catch(const std::exception&) { return 23; }
    }
    if(args.contains("--protection-test")) return protectionTest();
    if(args.contains("--target-test")) return targetTest();
    if(args.contains("--meeting-test")) return meetingTest();
    if(args.contains("--monitor-test")) return monitorTest();
    if(args.contains("--group-test")) return groupTest();
    if(args.contains("--measurement-test")) return measurementTest();
    bool simulatedTest=false;
    for(const QString& flag : {"--plot-test","--self-test","--sdk-test","--configure-check",
         "--connection-check","--session-connect-check","--worker-test","--current-worker-test",
         "--connection-smoke-test","--smoke-test","--manual-smoke-test"})
        simulatedTest=simulatedTest || args.contains(flag);
    ProcessLock applicationLock;
    if(!simulatedTest || args.contains("--real-instrument")) {
        try { applicationLock.acquire("application"); }
        catch(const std::exception& e) {
            QMessageBox::warning(nullptr,"程序已在运行",QString::fromUtf8(e.what()));return 1;
        }
    }
    if(args.contains("--plot-test")) return plotTest();
    if(args.contains("--self-test") || args.contains("--sdk-test")) return selftest(args.contains("--sdk-test"));
    if(args.contains("--configure-check")) {
        // This diagnostic never calls enable() or sets a nonzero current.
        const bool simulation=!args.contains("--real-instrument");
        const int visaIndex=args.indexOf("--visa");
        const QString address=visaIndex>=0 && visaIndex+1<args.size() ? args[visaIndex+1] : QString();
        Instrument instrument;QStringList lines;bool ok=false;
        try {
            lines << instrument.open(address,simulation,false);
            if(instrument.outputEnabled(1) || instrument.outputEnabled(2))
                throw std::runtime_error("Configure check refused: an output is already enabled");
            instrument.configure(2.0,0.1,!args.contains("--full-mode"));
            if (!simulation) {
                if (!instrument.measurementEnabled(2,KTB2900_VAL_MEASUREMENT_TYPE_VOLTAGE)
                    || !instrument.measurementEnabled(2,KTB2900_VAL_MEASUREMENT_TYPE_CURRENT)
                    || (args.contains("--full-mode") && !instrument.measurementEnabled(1,KTB2900_VAL_MEASUREMENT_TYPE_VOLTAGE)))
                    throw std::runtime_error("Configure check: required measurement function is disabled");
                lines << "PASS instrument readback: CH2 voltage AND current measurement functions enabled";
                if(args.contains("--full-mode"))lines << "PASS instrument readback: CH1 voltage measurement function enabled";
            }
            if(instrument.outputEnabled(1) || instrument.outputEnabled(2) || instrument.currentSetting()!=0.0)
                throw std::runtime_error("Configure check: expected both outputs off and CH2 setpoint zero");
            lines << "PASS configuration: CH1 OFF, CH2 OFF, CH2 source setpoint 0 A; no output enabled";
            ok=true;
        } catch(const std::exception& e) {lines << QString("FAIL %1").arg(QString::fromUtf8(e.what()));}
        const QStringList errors=instrument.shutdown();lines<<errors;if(!errors.isEmpty())ok=false;
        const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
        QFile file(QDir(output).filePath(QString("configure-check-%1-%2.txt").arg(simulation?"sim":"real",args.contains("--full-mode")?"full":"current")));
        if(file.open(QIODevice::WriteOnly)){QTextStream out(&file);out.setCodec("UTF-8");out<<lines.join('\n');}
        return ok ? 0 : 1;
    }
    if(args.contains("--connection-check")) {
        const bool simulation=!args.contains("--real-instrument");
        const int visaIndex=args.indexOf("--visa");
        const QString address=visaIndex>=0 && visaIndex+1<args.size() ? args[visaIndex+1] : QString();
        ConnectionProbe probe(address,simulation);bool ok=false;QString details;
        QObject::connect(&probe,&ConnectionProbe::result,&app,[&](bool success,QString text){ok=success;details=text;});
        QObject::connect(&probe,&QThread::finished,&app,&QCoreApplication::quit);
        probe.start();app.exec();probe.wait();
        const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
        QFile file(QDir(output).filePath(simulation ? "connection-check-sim.txt" : "connection-check-real.txt"));
        if(file.open(QIODevice::WriteOnly)){QTextStream out(&file);out.setCodec("UTF-8");out<<(ok ? "PASS " : "FAIL ")<<(simulation ? "SDK simulation: " : "Real IVI connection (no reset / no output configuration): ")<<details<<'\n';}
        return ok ? 0 : 1;
    }
    if(args.contains("--session-connect-check")) {
        Settings settings;settings.mode=args.contains("--real-instrument") ? 2 : 1;
        const int visaIndex=args.indexOf("--visa");
        if(visaIndex>=0 && visaIndex+1<args.size())settings.resource=args[visaIndex+1];
        SessionThread session(settings);bool connected=false,error=false;int monitoringSamples=0;QString identity;
        QObject::connect(&session,&ControlThread::monitorSample,&app,[&]{++monitoringSamples;});
        QObject::connect(&session,&ControlThread::connected,&app,[&](QString text){
            connected=true;identity=text;QTimer::singleShot(2200,&app,[&]{session.disconnectDevice();});
        });
        QObject::connect(&session,&ControlThread::failed,&app,[&](QString text){error=true;identity+=text;});
        QObject::connect(&session,&QThread::finished,&app,&QCoreApplication::quit);
        session.start();app.exec();session.wait();
        const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
        QFile file(QDir(output).filePath(settings.mode==2 ? "session-connect-real.txt" : "session-connect-sim.txt"));
        const bool ok=connected && !error && monitoringSamples>=2;
        if(file.open(QIODevice::WriteOnly)){QTextStream out(&file);out.setCodec("UTF-8");out<<(ok ? "PASS " : "FAIL ")<<"Connect / continuous CH1 acquisition / disconnect; CH2 never started: "<<identity;}
        return ok ? 0 : 1;
    }
    if(args.contains("--worker-test") || args.contains("--current-worker-test")) {
        Settings settings; settings.mode=1;
        settings.currentTest=args.contains("--current-worker-test");
        if(settings.currentTest) {settings.manualMA=-0.1;settings.targetV=1000;}
        ControlThread worker(settings);int samples=0;bool error=false;QStringList lines;
        QObject::connect(&worker,&ControlThread::message,&app,[&](QString s){lines<<s;});
        QObject::connect(&worker,&ControlThread::failed,&app,[&](QString s){error=true;lines<<s;});
        QObject::connect(&worker,&ControlThread::sample,&app,[&](double,double,double command,double,double){
            if(settings.currentTest && command!=-0.1) {error=true;lines<<"FAIL current test changed direct setpoint";}
            if(++samples>=5)worker.requestInterruption();
        });
        QObject::connect(&worker,&QThread::finished,&app,&QCoreApplication::quit);
        QTimer::singleShot(15000,&app,[&]{error=true;worker.requestInterruption();});
        worker.start();app.exec();worker.wait();
        const bool ok=!error && samples>=5;lines<<(ok?"PASS worker thread SDK / sample / stop":"FAIL worker thread test");
        QFile f(QDir(qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath())).filePath(settings.currentTest ? "current-worker-test.txt" : "worker-test.txt"));
        if(f.open(QIODevice::WriteOnly)){QTextStream out(&f);out.setCodec("UTF-8");out<<lines.join('\n');}
        return ok?0:1;
    }
    // Prepare the default storage location before constructing/showing the operator UI.
    const QString historyDirectory=defaultDataDirectory();
    if(!QDir().mkpath(historyDirectory)) {
        QMessageBox::critical(nullptr,"无法创建数据目录",
            "无法创建默认数据文件夹：\n"+QDir::toNativeSeparators(historyDirectory)+"\n请检查程序所在文件夹的写入权限。");
        return 1;
    }
    const bool manualSmoke=args.contains("--manual-smoke-test");
    const bool guiSmoke=args.contains("--smoke-test") || manualSmoke;
    Window window((guiSmoke || args.contains("--connection-smoke-test"))
        ? Window::Backend::DriverSimulationForTest : Window::Backend::RealInstrument);
    if(args.contains("--connection-smoke-test")) {
        auto* address=window.findChild<QLineEdit*>("visaAddress");
        auto* button=window.findChild<QPushButton*>("testConnectionButton");
        auto* start=window.findChild<QPushButton*>("startControlButton");
        auto* status=window.findChild<QLabel*>("connectionStatus");
        auto* overrideLimit=window.findChild<QCheckBox*>("overrideVoltageLimit");
        auto* voltageLimit=window.findChild<QDoubleSpinBox*>("voltageLimitOverride");
        auto* limitStatus=window.findChild<QLabel*>("voltageLimitStatus");
        const QByteArray originalRoot=qgetenv("IVI_ROOT");
        bool passed=button->isEnabled() && !start->isEnabled() && !overrideLimit->isChecked() && !voltageLimit->isEnabled();int stage=0;
        passed=passed && !window.findChild<QComboBox*>("connectionMode") && !window.findChild<QComboBox*>("functionMode");
        window.start();passed=passed && window.sampleCount==0 && !start->isEnabled();
        const QString screenshotDir=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(screenshotDir);
        {
            Window operatorWindow; // Inspect the real UI without connecting or opening outputs.
            passed=passed && operatorWindow.findChild<QLabel*>("connectionStatus")->text().contains("请先连接 B2912C")
                && !operatorWindow.findChild<QComboBox*>("connectionMode") && !operatorWindow.findChild<QComboBox*>("functionMode");
            passed=passed && QDir(historyDirectory).exists()
                && operatorWindow.findChild<QLineEdit*>("storageDirectory")->text()==historyDirectory
                && historyDirectory==QDir(QCoreApplication::applicationDirPath()).filePath("HisData");
            operatorWindow.show();app.processEvents();
            operatorWindow.grab().save(QDir(screenshotDir).filePath("real-gui-idle.png"));
            operatorWindow.resize(1100,680);app.processEvents();
            operatorWindow.grab().save(QDir(screenshotDir).filePath("real-gui-compact.png"));
            operatorWindow.findChild<QToolButton*>("advancedInstrumentToggle")->setChecked(true);app.processEvents();
            operatorWindow.grab().save(QDir(screenshotDir).filePath("real-gui-compact-advanced.png"));
        }
        passed=passed && !start->isEnabled();window.start();passed=passed && window.sampleCount==0;
        window.show();
        QObject::connect(&window,&Window::connectionChanged,&app,[&](bool ok){
            if(stage==0 && ok) {
                passed=passed && start->isEnabled() && !address->isEnabled() && status->text().contains("已连接");
                const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
                window.grab().save(QDir(output).filePath("connection-gui-smoke.png"));
                passed=passed && limitStatus->text().contains("2 V");
                window.findChild<QToolButton*>("advancedInstrumentToggle")->setChecked(true);
                overrideLimit->setChecked(true);voltageLimit->setValue(3.5);
                passed=passed && voltageLimit->isEnabled();
                window.grab().save(QDir(output).filePath("advanced-settings-smoke.png"));
                stage=1;window.findChild<QDoubleSpinBox*>("manualCurrent")->setValue(0.1);
                window.start(true);QTimer::singleShot(700,&window,&Window::stop);
            } else if(stage==3 && !ok) {
                passed=passed && !start->isEnabled() && address->isEnabled();
                window.start();passed=passed && !start->isEnabled();
                qputenv("IVI_ROOT","F:/B2912C_PID_Qt/tests/nonexistent-sdk");
                stage=4;window.testConnection();
            } else {
                passed=passed && stage==4 && !ok && !start->isEnabled() && status->text().contains("异常") && button->isEnabled();
                if(originalRoot.isNull())qunsetenv("IVI_ROOT");else qputenv("IVI_ROOT",originalRoot);
                app.exit(passed ? 0 : 1);
            }
        });
        QObject::connect(&window,&Window::stopped,&app,[&]{
            passed=passed && !window.hadError && window.sampleCount>=3 && start->isEnabled() && status->text().contains("已连接");
            passed=passed && limitStatus->text().contains("3.5 V");
            if(stage==1) {
                overrideLimit->setChecked(false);voltageLimit->setValue(5);
                passed=passed && !voltageLimit->isEnabled();
                stage=2;window.findChild<QDoubleSpinBox*>("manualCurrent")->setValue(-0.1);
                window.start(true);QTimer::singleShot(700,&window,&Window::stop);
            } else {stage=3;window.testConnection();}
        });
        QTimer::singleShot(100,&window,[&]{window.testConnection();passed=passed && !button->isEnabled();});
        return app.exec();
    }
    if(manualSmoke) {
        window.findChild<QDoubleSpinBox*>("manualCurrent")->setValue(0.1);
    }
    window.show();
    bool smokePassed=false;
    if(guiSmoke) {
        QObject::connect(&window,&Window::connectionChanged,&app,[&](bool connected){
            if(connected)window.start(manualSmoke);else app.exit(smokePassed ? 0 : 1);
        });
        QTimer::singleShot(100,&window,&Window::testConnection);
        QTimer::singleShot(4500,&window,[&]{
            const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());
            QDir().mkpath(output);window.grab().save(QDir(output).filePath(manualSmoke ? "manual-gui-smoke.png" : "gui-smoke.png"));window.stop();
        });
        QObject::connect(&window,&Window::stopped,&app,[&]{
            bool ok=window.sampleCount>=20 && !window.hadError;
            if(manualSmoke) {
                ok=ok && window.findChild<QDoubleSpinBox*>("targetVoltage")->isEnabled();
                auto* plot=window.findChild<QCustomPlot*>("probePlot");
                ok=ok && plot && plot->yAxis->label()=="电压（V）";
            }
            smokePassed=ok;
            window.testConnection();
        });
        QTimer::singleShot(15000,&window,[&]{window.hadError=true;window.stop();});
    }
    return app.exec();
}

#include "stability.h"
#include "traceplot.h"
#include "simulatedsignal.h"
#include <QtWidgets>
#include <stdexcept>
#include <cmath>

namespace {
void waitUntil(const std::function<bool()>& done,int timeout=10000) {
    QElapsedTimer timer;timer.start();
    while(!done()) {
        if(timer.elapsed()>timeout)throw std::runtime_error("Offline self-test timed out");
        QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);
    }
}
QStringList dataRows(const QString& directory) {
    QStringList rows;QDir dir(directory);
    for(const auto& name:dir.entryList({"*.txt"},QDir::Files,QDir::Name)) {
        QFile file(dir.filePath(name));if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read TXT");
        rows+=QString::fromUtf8(file.readAll()).split("\r\n",Qt::SkipEmptyParts);
    }
    return rows;
}
QString readText(const QString& path) {
    QFile f(path);if(!f.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read diagnostics");return QString::fromUtf8(f.readAll());
}
}
int stabilitySelfTest() {
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath()+"/offline-test-results");QDir().mkpath(output);
    QStringList checks;int status=0;
    auto require=[&](bool ok,const QString& text){if(!ok)throw std::runtime_error(text.toStdString());checks<<"PASS "+text;};
    try {
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary test directory available");
        StabilityWindow window;window.show();QCoreApplication::processEvents();
        require(window.findChild<QLineEdit*>("visaAddress") && !window.findChild<QLineEdit*>("visaAddress")->isEnabled()
                && !window.findChild<QPushButton*>("testConnectionButton")->isEnabled(),
                "Production connection row is retained but hardware controls are disabled");
        require(!window.findChild<QDoubleSpinBox*>("proportionalGain")->isEnabled()
                && !window.findChild<QPushButton*>("applyTargetButton")->isEnabled()
                && window.findChild<QLabel*>("probeReading") && window.findChild<QLabel*>("measuredReading"),
                "Production parameter/readout layout is present; no PID controls can execute");
        require(window.findChild<QSpinBox*>("period")->value()==100
                && window.findChild<QDoubleSpinBox*>("duration")->value()==24,"Defaults are 100 ms generation and 24 real hours");
        auto* directory=window.findChild<QLineEdit*>("directory");
        auto* frequency=window.findChild<QComboBox*>("frequency");
        auto* draw=window.findChild<QCheckBox*>("draw");
        auto* probe=window.findChild<TracePlot*>("probePlot");
        auto* current=window.findChild<TracePlot*>("currentPlot");
        // Raw storage and an intentional GUI stall: the independent generator should keep producing.
        directory->setText(temporary.path()+"/raw");window.findChild<QPushButton*>("startControlButton")->click();
        waitUntil([&]{return window.testMetrics()->delivered>=4;});
        const auto before=window.testMetrics()->produced.load();
        QThread::msleep(1200); // Only this automated test blocks the GUI deliberately.
        require(window.testMetrics()->produced>=before+5,"Generator continues during a deliberately blocked GUI");
        waitUntil([&]{return window.testMetrics()->delivered==window.testMetrics()->produced && window.testMetrics()->produced>=24;});
        window.stopTest();waitUntil([&]{return !window.isTesting();});
        const auto rows=dataRows(directory->text());
        require(rows.size()==int(window.testMetrics()->produced.load()),"10 Hz option saves every generated sample and drains on stop");
        require(window.testMetrics()->maxDispatchMs>=1000,"UI dispatch delay detects the intentional GUI stall");
        bool format=true;
        for(const auto& row:rows)format=format && row.split(' ').size()==6 && !row.contains(',') && !row.contains("探头");
        require(format,"Raw TXT preserves timestamp plus four values, space-separated without a header");
        const auto logs=QDir(directory->text()+"/Diagnostics").entryList({"*_performance.csv"},QDir::Files);
        require(logs.size()==1,"Performance CSV is automatically created");
        const auto performance=readText(directory->text()+"/Diagnostics/"+logs.first());
        require(performance.contains("process_cpu_percent,working_set_mb,private_mb")
                && performance.split('\n',Qt::SkipEmptyParts).size()>=3,"Periodic diagnostics contain resource and timing fields");
        QFile::copy(directory->text()+"/Diagnostics/"+logs.first(),output+"/sample-performance.csv");
        window.grab().save(output+"/offline-running.png");
        require(!window.findChild<QLineEdit*>("visaAddress")->isEnabled()
                && !window.findChild<QPushButton*>("testConnectionButton")->isEnabled(),"Hardware controls remain disabled after stopping");
        // Independent run: curve/file means must be identical.
        window.findChild<QDoubleSpinBox*>("targetVoltage")->setValue(1.25);
        directory->setText(temporary.path()+"/mean");frequency->setCurrentIndex(0);window.startTest();
        waitUntil([&]{return window.testMetrics()->delivered>=26;});
        window.stopTest();waitUntil([&]{return !window.isTesting();});
        const auto means=dataRows(directory->text());
        bool aligned=means.size()>=2 && means.size()==probe->graph(0)->data()->size();
        for(int i=0;i<means.size() && aligned;++i) {
            const auto values=means[i].split(' ');
            aligned=values.size()==6
                && values[3].toDouble()==1.25 && std::abs(values[2].toDouble()-1.25)<.01
                && std::abs(values[2].toDouble()-(probe->graph(0)->data()->constBegin()+i)->value)<1e-9
                && std::abs(values[4].toDouble()-(current->graph(0)->data()->constBegin()+i)->value)<1e-9
                && std::abs(values[5].toDouble()-(current->graph(1)->data()->constBegin()+i)->value)<1e-9;
        }
        require(aligned,"1 Hz TXT means align with both voltage and current plots");
        window.findChild<QDoubleSpinBox*>("targetVoltage")->setValue(0);
        // Intentionally slow one worker cycle. Offline diagnostic mode must record and continue.
        directory->setText(temporary.path()+"/delay");frequency->setCurrentIndex(1);draw->setChecked(false);
        window.optionsForTest.injectAfterSamples=3;window.optionsForTest.injectDelayMs=1200;
        window.startTest();waitUntil([&]{return window.testMetrics()->produced>=10;});
        require(window.testMetrics()->overruns>=1 && window.isTesting(),"A worker delay over one second is recorded without ending the offline run");
        window.stopTest();waitUntil([&]{return !window.isTesting();});
        require(probe->graph(0)->data()->isEmpty() && current->graph(0)->data()->isEmpty()
                && dataRows(directory->text()).size()==int(window.testMetrics()->produced.load()),"No-drawing comparison still generates and saves every raw sample");
        const auto events=QDir(directory->text()+"/Diagnostics").entryList({"*_events.log"},QDir::Files);
        const auto eventText=readText(directory->text()+"/Diagnostics/"+events.first());
        require(eventText.contains("SAMPLING_DELAY") && eventText.contains("TEST_STOP"),"Delay and final summary survive in the event log");
        QFile::copy(directory->text()+"/Diagnostics/"+events.first(),output+"/sample-events.log");
        // Reuse the shipped plot implementation with a complete day of synthetic samples.
        TracePlot dayProbe,dayCurrent;dayCurrent.setCurrentMode(true);dayProbe.resize(800,400);dayCurrent.resize(800,400);
        SimulatedSignal synthetic;QElapsedTimer load;load.start();
        for(int i=0;i<=864010;++i) {
            const double t=i/10.;const auto point=synthetic.next(t,0);
            dayProbe.appendSample(t,point.probeV,0);dayCurrent.appendCurrentSample(t,point.outputMA,point.measuredMA);
        }
        checks<<QString("INFO Synthetic 24h data load: %1 ms").arg(load.elapsed());
        require(dayProbe.graph(0)->data()->size()==86401 && dayCurrent.graph(1)->data()->size()==86401,"24-hour averaged history is bounded and retained");
        load.restart();dayProbe.replot();dayCurrent.replot();checks<<QString("INFO Synthetic full-day repaint: %1 ms").arg(load.elapsed());
        dayProbe.grab().save(output+"/offline-24h-probe.png");
        double sum=0,sum2=0;
        for(const auto& point:*dayProbe.graph(0)->data()){sum+=point.value;sum2+=point.value*point.value;}
        const double count=dayProbe.graph(0)->data()->size();
        const double deviation=std::sqrt(sum2/count-(sum/count)*(sum/count));
        require(deviation>.0004 && deviation<.0009 && dayProbe.yAxis->range().size()<.01,
                "One-second synthetic voltage forms a visible millivolt noise band");
        // Visual-only history preview. The real start path always begins with empty curves.
        probe->resetData(0);current->resetData(0);SimulatedSignal preview;
        for(int i=0;i<=324010;++i){
            const double t=i/10.;const auto point=preview.next(t,0);
            probe->appendSample(t,point.probeV,0);current->appendCurrentSample(t,point.outputMA,point.measuredMA);
        }
        window.findChild<QLabel*>("state")->setText("曲线样式预览 · 9小时合成数据，仅用于显示检查；不是连续运行9小时的结果。");
        window.resize(1600,950);QCoreApplication::processEvents();probe->replot();current->replot();
        window.grab().save(output+"/offline-layout-9h-preview.png");
        window.resize(1100,680);QCoreApplication::processEvents();probe->replot();current->replot();
        window.grab().save(output+"/offline-layout-small.png");
        // Automatic deadline and clean shutdown with saving disabled.
        window.optionsForTest={};directory->setText(temporary.path()+"/deadline");
        window.findChild<QCheckBox*>("save")->setChecked(false);window.findChild<QDoubleSpinBox*>("duration")->setValue(.001);
        window.startTest();waitUntil([&]{return !window.isTesting();});
        require(window.testMetrics()->produced>=20,"A short real-time deadline automatically ends the offline test");
        const QString blocked=temporary.path()+"/diagnostic-failure";QDir().mkpath(blocked);
        {QFile f(blocked+"/Diagnostics");require(f.open(QIODevice::WriteOnly),"Create diagnostic failure fixture");}
        directory->setText(blocked);window.findChild<QCheckBox*>("save")->setChecked(true);
        window.startTest();waitUntil([&]{return !window.isTesting();});
        require(window.findChild<QPlainTextEdit*>("runLog")->toPlainText().contains("测试错误")
                && window.testMetrics()->produced==0,"Diagnostic failure visibly stops the test and cleans up its recording thread");
        directory->setText(temporary.path()+"/close");window.findChild<QCheckBox*>("save")->setChecked(true);
        window.findChild<QDoubleSpinBox*>("duration")->setValue(24);window.startTest();
        waitUntil([&]{return window.testMetrics()->produced>=3;});window.close();waitUntil([&]{return !window.isTesting();});
        require(dataRows(directory->text()).size()==int(window.testMetrics()->produced.load()),"Closing the window stops the worker and drains pending storage");
    } catch(const std::exception& e) {checks<<QString("FAIL %1").arg(QString::fromUtf8(e.what()));status=1;}
    QFile outputFile(output+"/self-test.txt");if(outputFile.open(QIODevice::WriteOnly))outputFile.write(checks.join('\n').toUtf8());
    return status;
}

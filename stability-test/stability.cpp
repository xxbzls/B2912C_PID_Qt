#include "stability.h"
#include "traceplot.h"
#include "simulatedsignal.h"
#include <QtWidgets>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {
void maximum(std::atomic<double>& value,double candidate) {
    double old=value.load();
    while(old<candidate && !value.compare_exchange_weak(old,candidate)) {}
}
QString elapsedText(double seconds) {
    const qint64 s=static_cast<qint64>(seconds);
    return QString("%1:%2:%3").arg(s/3600,2,10,QChar('0')).arg((s/60)%60,2,10,QChar('0')).arg(s%60,2,10,QChar('0'));
}
struct Resources { double cpu=-1,working=-1,privateMB=-1; };
class ResourceReader {
    quint64 previousCpu=0;
    qint64 previousWall=0;
public:
    Resources read() {
        Resources r;
#ifdef Q_OS_WIN
        FILETIME created,exited,kernel,user;
        const qint64 wall=monotonicNs();
        if(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) {
            auto number=[](FILETIME t){return (quint64(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
            const quint64 cpu=number(kernel)+number(user);
            if(previousWall && wall>previousWall)
                r.cpu=100.*double(cpu-previousCpu)*100./double(wall-previousWall)/std::max(1,QThread::idealThreadCount());
            previousCpu=cpu;previousWall=wall;
        }
        PROCESS_MEMORY_COUNTERS_EX m{};m.cb=sizeof(m);
        if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m),sizeof(m))) {
            r.working=double(m.WorkingSetSize)/(1024*1024);r.privateMB=double(m.PrivateUsage)/(1024*1024);
        }
#endif
        return r;
    }
};
QString openUnique(QFile& file,const QString& directory,const QString& stem,const QString& extension) {
    if(!QDir().mkpath(directory)) throw std::runtime_error("无法创建诊断目录");
    for(int i=0;i<10000;++i) {
        file.setFileName(QDir(directory).filePath(stem+(i ? QString("_%1").arg(i) : "")+extension));
        if(file.open(QIODevice::WriteOnly|QIODevice::NewOnly)) return file.fileName();
        if(!QFileInfo::exists(file.fileName())) break;
    }
    throw std::runtime_error(("无法创建诊断文件："+file.errorString()).toStdString());
}
void writeChecked(QFile& file,const QByteArray& bytes) {
    if(file.write(bytes)!=bytes.size() || !file.flush())
        throw std::runtime_error(("诊断日志写入失败："+file.errorString()).toStdString());
}
}
qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
TestWorker::TestWorker(TestOptions options,std::shared_ptr<TestMetrics> metrics,QObject* parent)
    :QThread(parent),options(std::move(options)),metrics(std::move(metrics)) {}
void TestWorker::interruptibleWait(qint64 deadlineNs) {
    while(!isInterruptionRequested()) {
        const qint64 remaining=deadlineNs-monotonicNs();
        if(remaining<=0) break;
        QThread::msleep(static_cast<unsigned long>(std::max<qint64>(1,std::min<qint64>(5,remaining/1000000))));
    }
}
void TestWorker::run() {
    QFile performance,events;
    try {
        const QString diagnosticDirectory=QDir(options.directory).filePath("Diagnostics");
        const QString stem=beijingTime(QDateTime::currentMSecsSinceEpoch(),"yyyyMMdd_HHmmss");
        const QString path=openUnique(performance,diagnosticDirectory,stem+"_performance",".csv");
        openUnique(events,diagnosticDirectory,stem+"_events",".log");
        emit diagnostics(path);
        writeChecked(performance,"beijing_time,elapsed_s,samples,last_gap_ms,max_gap_ms,gaps_over_1s,ui_received,ui_pending,ui_heartbeat_age_ms,max_ui_dispatch_ms,max_plot_ms,process_cpu_percent,working_set_mb,private_mb\r\n");
        auto event=[&](const QString& text){writeChecked(events,(beijingTime(QDateTime::currentMSecsSinceEpoch())+" "+text+"\r\n").toUtf8());emit message(text);};
        event(QString("TEST_START offline_only=true period_ms=%1 hours=%2 draw=%3 save=%4 storage_hz=%5; 延迟超过1秒只记录，继续测试")
              .arg(options.periodMs).arg(options.hours).arg(options.draw).arg(options.save).arg(options.storageHz));
        const qint64 begin=monotonicNs();
        qint64 previous=0,lastReport=begin;
        ResourceReader resources;resources.read();
        auto report=[&](qint64 now){
            const auto r=resources.read();
            const quint64 count=metrics->produced.load(),delivered=metrics->delivered.load();
            const QStringList columns={beijingTime(QDateTime::currentMSecsSinceEpoch()),QString::number((now-begin)/1e9,'f',3),
                QString::number(count),QString::number(metrics->lastGapMs.load(),'f',3),QString::number(metrics->maxGapMs.load(),'f',3),
                QString::number(metrics->overruns.load()),QString::number(delivered),QString::number(count>delivered ? count-delivered : 0),
                QString::number(std::max<qint64>(0,now-metrics->heartbeatNs.load())/1e6,'f',3),
                QString::number(metrics->maxDispatchMs.load(),'f',3),QString::number(metrics->maxPaintMs.load(),'f',3),
                QString::number(r.cpu,'f',3),QString::number(r.working,'f',3),QString::number(r.privateMB,'f',3)};
            writeChecked(performance,(columns.join(',')+"\r\n").toUtf8());
        };
        SimulatedSignal signal;
        while(!isInterruptionRequested()) {
            if(options.injectAfterSamples>0 && metrics->produced.load()==quint64(options.injectAfterSamples))
                interruptibleWait(monotonicNs()+qint64(options.injectDelayMs)*1000000);
            if(isInterruptionRequested()) break;
            const qint64 cycle=monotonicNs();
            const double seconds=(cycle-begin)/1e9;
            if(options.hours>0 && seconds>=options.hours*3600) break;
            const double gap=previous ? (cycle-previous)/1e6 : 0.;previous=cycle;
            metrics->lastGapMs.store(gap);maximum(metrics->maxGapMs,gap);
            if(gap>1000) {
                ++metrics->overruns;
                event(QString("SAMPLING_DELAY elapsed_s=%1 actual_ms=%2 threshold_ms=1000; offline test continues")
                      .arg(seconds,0,'f',3).arg(gap,0,'f',3));
            }
            // Open-loop, deterministic signals. None of these values are instrument readings or PID outputs.
            const auto generated=signal.next(seconds,options.targetV);
            const RecordSample raw{QDateTime::currentMSecsSinceEpoch(),seconds,generated.probeV,options.targetV,generated.outputMA,generated.measuredMA,true};
            const quint64 sequence=++metrics->produced;
            emit rawSample(raw); // Same bounded-queue storage path as the instrument program.
            emit frame({raw,sequence,monotonicNs(),gap});
            const qint64 now=monotonicNs();
            if(now-lastReport>=1000000000) {report(now);lastReport=now;}
            interruptibleWait(cycle+qint64(options.periodMs)*1000000);
        }
        report(monotonicNs());
        event(QString("TEST_STOP reason=%1 samples=%2 gaps_over_1s=%3 max_gap_ms=%4")
              .arg(isInterruptionRequested() ? "requested" : "duration_reached").arg(metrics->produced.load())
              .arg(metrics->overruns.load()).arg(metrics->maxGapMs.load(),0,'f',3));
    } catch(const std::exception& e) {
        if(events.isOpen()) {
            events.write((beijingTime(QDateTime::currentMSecsSinceEpoch())+" FATAL "+QString::fromUtf8(e.what())+"\r\n").toUtf8());events.flush();
        }
        emit failed(QString::fromUtf8(e.what()));
    }
}

StabilityWindow::StabilityWindow(QWidget* parent):QWidget(parent) {
    buildUi();
    connect(probe,&QCustomPlot::beforeReplot,this,[this]{paintProbe.start();});
    connect(probe,&QCustomPlot::afterReplot,this,[this]{maximum(metrics->maxPaintMs,paintProbe.nsecsElapsed()/1e6);});
    connect(current,&QCustomPlot::beforeReplot,this,[this]{paintCurrent.start();});
    connect(current,&QCustomPlot::afterReplot,this,[this]{maximum(metrics->maxPaintMs,paintCurrent.nsecsElapsed()/1e6);});
    auto* heartbeat=new QTimer(this);heartbeat->setInterval(250);
    connect(heartbeat,&QTimer::timeout,this,[this]{
        metrics->heartbeatNs.store(monotonicNs());if(worker)updateHealth();
    });heartbeat->start();metrics->heartbeatNs.store(monotonicNs());updateControls();updateFiles();
}
StabilityWindow::~StabilityWindow() {
    if(worker){worker->requestInterruption();worker->wait();}
    if(recorder){recorder->finish();recorder->wait();}
}
void StabilityWindow::append(const QString& text){log->appendPlainText(beijingTime(QDateTime::currentMSecsSinceEpoch(),"HH:mm:ss")+"  "+text);}
void StabilityWindow::updateControls() {
    const bool idle=!isTesting() && !closing;
    start->setEnabled(idle);stop->setEnabled(isTesting() && !stopping);
    for(auto* w:QList<QWidget*>{target,period,duration,draw,save,directory,browse})w->setEnabled(idle);
    frequency->setEnabled(idle && save->isChecked());
    const QString color=hadFailure ? "#dc2626" : isTesting() ? "#16a34a" : "#9ca3af";
    loopLamp->setStyleSheet(QString("background:%1;border:1px solid %1;border-radius:6px;").arg(color));
    loopState->setText(hadFailure ? "离线测试故障" : stopping ? "正在停止" : isTesting() ? "离线测试中" : "离线待机");
}
void StabilityWindow::updateFiles() {
    const QString details="模拟数据："+(dataPath.isEmpty() ? "未开始" : QDir::toNativeSeparators(dataPath))
        +"\n诊断日志："+(diagnosticPath.isEmpty() ? "开始后自动创建" : QDir::toNativeSeparators(diagnosticPath));
    files->setText(dataPath.isEmpty() ? "模拟数据尚未保存 · 性能诊断在开始测试后自动记录到数据目录下的 Diagnostics。"
        : "模拟数据："+QDir::toNativeSeparators(dataPath));
    files->setToolTip(details);
}
void StabilityWindow::updateHealth() {
    const auto p=metrics->produced.load(),d=metrics->delivered.load();
    health->setText(QString("运行 %1  ·  样本 %2  ·  最大采样间隔 %3 ms  ·  超1秒 %4次  ·  界面待处理 %5  ·  最大绘图 %6 ms")
        .arg(elapsedText((monotonicNs()-startedNs)/1e9)).arg(p).arg(metrics->maxGapMs.load(),0,'f',1)
        .arg(metrics->overruns.load()).arg(p>d ? p-d : 0).arg(metrics->maxPaintMs.load(),0,'f',1));
}
void StabilityWindow::startTest() {
    if(isTesting()) return;
    activeOptions={};activeOptions.periodMs=period->value();activeOptions.hours=duration->value();activeOptions.draw=draw->isChecked();activeOptions.save=save->isChecked();
    activeOptions.targetV=target->value();
    activeOptions.storageHz=frequency->currentData().toInt();activeOptions.directory=directory->text().trimmed();
    activeOptions.injectAfterSamples=optionsForTest.injectAfterSamples;activeOptions.injectDelayMs=optionsForTest.injectDelayMs;
    if(activeOptions.directory.isEmpty() || !QDir().mkpath(activeOptions.directory)) {append("无法创建测试数据目录。");return;}
    stopping=false;recorderDone=false;hadFailure=false;metrics=std::make_shared<TestMetrics>();metrics->heartbeatNs.store(monotonicNs());dataPath.clear();diagnosticPath.clear();
    probe->resetData(activeOptions.targetV);current->resetData(0);log->clear();state->setText("正在准备离线测试……");updateFiles();
    if(!activeOptions.save) {launchWorker();return;}
    RecordSettings s;s.directory=activeOptions.directory;s.frequencyHz=activeOptions.storageHz;
    recorder=new DataRecorder(s,this);auto* writer=recorder;
    connect(writer,&DataRecorder::ready,this,[this,writer]{if(recorder==writer && !stopping && !closing)launchWorker();else writer->finish();});
    connect(writer,&DataRecorder::fileChanged,this,[this](QString path){dataPath=path;updateFiles();append("模拟数据文件："+path);});
    connect(writer,&DataRecorder::saved,this,[this](quint64 rows){append(QString("模拟数据已保存，共 %1 条。").arg(rows));});
    connect(writer,&DataRecorder::failed,this,[this](QString error){hadFailure=true;append("存储错误："+error);state->setText("存储故障 · 测试正在停止");stopTest();});
    connect(writer,&QThread::finished,this,[this,writer]{
        if(recorder==writer)recorderDone=true;
        finishIfIdle();
    });
    updateControls();writer->start();
}
void StabilityWindow::launchWorker() {
    startedNs=monotonicNs();worker=new TestWorker(activeOptions,metrics,this);auto* running=worker;
    if(recorder)connect(running,&TestWorker::rawSample,recorder,&DataRecorder::submit,Qt::DirectConnection);
    connect(running,&TestWorker::frame,this,&StabilityWindow::receiveFrame);
    connect(running,&TestWorker::message,this,&StabilityWindow::append);
    connect(running,&TestWorker::diagnostics,this,[this](QString path){diagnosticPath=path;updateFiles();append("性能诊断文件："+path);});
    connect(running,&TestWorker::failed,this,[this](QString error){hadFailure=true;append("测试错误："+error);state->setText("诊断故障 · 测试停止");});
    connect(running,&QThread::finished,this,[this,running]{
        if(worker==running){worker=nullptr;running->deleteLater();}
        if(recorder)recorder->finish();
        finishIfIdle();
    });
    state->setText("离线测试运行中 · 延迟超限只记录");updateControls();running->start();
}
void StabilityWindow::receiveFrame(const TestFrame& frame) {
    maximum(metrics->maxDispatchMs,(monotonicNs()-frame.sentNs)/1e6);metrics->delivered.store(frame.sequence);
    const auto& s=frame.sample;
    probeReading->setText(QString::number(s.probeV,'f',5));targetReading->setText(QString::number(s.targetV,'f',5));
    outputReading->setText(QString::number(s.outputMA,'f',4));measuredReading->setText(QString::number(s.measuredMA,'f',4));
    readings->setText(QString("模拟数据 · 电压误差 %1 V    ·    实际周期 %2 ms")
        .arg(s.targetV-s.probeV,0,'f',5).arg(frame.gapMs,0,'f',1));
    if(activeOptions.draw) {probe->appendSample(s.seconds,s.probeV,s.targetV);current->appendCurrentSample(s.seconds,s.outputMA,s.measuredMA);}
}
void StabilityWindow::stopTest() {
    if(!isTesting())return;
    stopping=true;state->setText("正在停止并保存……");updateControls();
    if(worker)worker->requestInterruption();else if(recorder)recorder->finish();
}
void StabilityWindow::finishIfIdle() {
    // The producer must have ended before deleting its direct-connected storage queue.
    if(!worker && recorder && recorderDone) {auto* writer=recorder;recorder=nullptr;writer->deleteLater();}
    if(isTesting()){updateControls();return;}
    stopping=false;state->setText(QString("%1 · %2 个样本 · 延迟超1秒 %3次")
        .arg(hadFailure ? "测试因故障结束，请查看日志" : "测试结束").arg(metrics->produced.load()).arg(metrics->overruns.load()));
    append(QString("测试结束：生成 %1 点，界面收到 %2 点，最大间隔 %3 ms，最大界面延迟 %4 ms。")
        .arg(metrics->produced.load()).arg(metrics->delivered.load()).arg(metrics->maxGapMs.load(),0,'f',2).arg(metrics->maxDispatchMs.load(),0,'f',2));
    if(startedNs)updateHealth();
    updateControls();emit testFinished();if(closing)close();
}
void StabilityWindow::closeEvent(QCloseEvent* event) {
    if(isTesting()){closing=true;stopTest();event->ignore();}else event->accept();
}

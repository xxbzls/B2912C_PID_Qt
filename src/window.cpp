#include "window.h"
#include "controller.h"
#include "traceplot.h"
#include <QtWidgets>

Window::Window(Backend backend) : driverSimulation(backend==Backend::DriverSimulationForTest) {buildUi();}
Window::~Window() {
    if(session) {session->disconnectDevice();session->wait();}
    if(recorder) {recorder->finish();recorder->wait();}
}
void Window::showConnectionStatus(const QString& text,const QString& color) {
    connectionStatus->setText(text);
    connectionStatus->setStyleSheet(QString("padding:2px 0px;color:%1;font-size:12px;").arg(color));
}
void Window::resetConnectionStatus() {
    if(session) return;
    voltageLimitStatus->setText("默认保留仪器设置，连接后读取");
    showConnectionStatus(!driverSimulation ? "未连接 · 请先连接 B2912C，连接成功后才能启动控制" :
                         "驱动仿真未连接 · 请先连接模拟驱动","#536575");
    refreshControls();
}
void Window::testConnection() {
    if(active) return;
    if(session) {
        if(!sessionReady) return;
        sessionReady=false;session->disconnectDevice();clearLiveReadings("正在断开，实时采集停止。");refreshControls();
        showConnectionStatus("正在断开连接……","#536575");return;
    }
    const bool simulation=driverSimulation;
    if(!simulation && resource->text().trimmed().isEmpty()) {
        showConnectionStatus("请先填写完整 VISA 地址","#b42318");return;
    }
    hadError=false;sessionReady=false;
    Settings connection;connection.mode=driverSimulation ? 1 : 2;connection.resource=resource->text().trimmed();
    connection.nplc=nplc->value();connection.periodMs=period->value();connection.probeVoltageLimit=probeVoltageLimit->value();
    session=new SessionThread(connection,this);
    refreshControls();
    showConnectionStatus(simulation ? "正在连接模拟驱动……" : "正在连接 B2912C……","#536575");
    clearLiveReadings("正在连接，等待探头采集。");
    append("开始连接：核对身份并申请控制锁；开启 CH1 的 0 A 测压，CH2 保持关断，不改写 CH2 电流设定或电压上限。");
    attachSignals(session);
    connect(session,&ControlThread::connected,this,[this,simulation](QString details){
        if(closing)return;
        sessionReady=true;identity=(simulation ? "【驱动仿真】" : "")+details;
        trace->resetData(target->value());currentTrace->resetData(0);showOutputOff();
        showConnectionStatus("已连接 · "+identity+" · CH1 持续采集，CH2 已关断","#18703c");
        append("连接成功："+identity);refreshControls();emit connectionChanged(true);
    });
    connect(session,&ControlThread::runStopped,this,[this]{
        if(!hadError && sessionReady) {
            showConnectionStatus("已连接 · "+identity+" · CH2 已关断，CH1 持续采集","#18703c");
            showOutputOff();values->setText("CH2 已关断，等待下一次探头电压采集。电流曲线保留历史数据。");
        }
        finishRun();
    });
    connect(session,&QThread::finished,this,[this]{
        session->deleteLater();session=nullptr;sessionReady=false;
        voltageLimitStatus->setText("已断开，重新连接后读取");
        clearLiveReadings(hadError ? "采集已停止，请查看异常信息。" : "已断开，实时采集已停止。");
        if(!hadError)showConnectionStatus("已断开 · 请先连接电源再启动控制","#536575");
        finishRun();emit connectionChanged(false);
        if(closing && !recorder)close();
    });
    session->start();
}
void Window::refreshControls() {
    const bool waiting=session && !sessionReady;
    const bool editable=!active && !waiting && !closing && !recorder;
    const bool livePid=active && sampleCount>0 && sessionReady && !hadError && !stopping && !closing
        && !activeSettings.manual;
    parameters->setEnabled(!waiting && !closing);
    for(auto* w : {kp,ti,td}) w->setEnabled(editable);
    for(auto* w : {limit,probeVoltageLimit,nplc,manual}) w->setEnabled(editable);
    direction->setEnabled(editable);period->setEnabled(editable);
    overrideVoltageLimit->setEnabled(editable);
    voltageLimit->setEnabled(editable && overrideVoltageLimit->isChecked());
    target->setEnabled(editable || livePid);
    applyTargetButton->setEnabled(livePid && !targetPending);
    applyTargetButton->setText(targetPending ? "正在应用…" : "应用目标");
    resource->setEnabled(editable && !session);
    connectButton->setText(sessionReady ? "断开连接" : driverSimulation ? "连接驱动（仿真）" : "连接电源");
    connectButton->setEnabled(editable);
    const bool canStart=!active && !closing && sessionReady && !recorder;
    startButton->setEnabled(canStart);manualButton->setEnabled(canStart);
    stopButton->setEnabled(active && !stopping && !closing);
    saveData->setEnabled(editable);storageFrequency->setEnabled(editable && saveData->isChecked());
    storageDirectory->setEnabled(editable);browseStorage->setEnabled(editable);
    refreshLoopState();
}
void Window::refreshLoopState() {
    QString color="#9ca3af",text="闭环未使能",state="idle";
    if(hadError) {color="#dc2626";text="闭环故障";state="fault";}
    else if(active && (stopping || closing)) {color="#d97706";text="正在停止";state="stopping";}
    else if(active && !activeSettings.manual) {
        if(preparingControl || sampleCount==0) {color="#d97706";text="闭环启动中";state="starting";}
        else {color="#16a34a";text="闭环已使能";state="running";}
    } else if(active) text="闭环未使能 · 手动输出";
    loopLamp->setStyleSheet(QString("background:%1;border:1px solid %1;border-radius:6px;").arg(color));
    loopLamp->setProperty("state",state);loopLamp->setToolTip(text);loopState->setText(text);
}
void Window::append(QString message){log->appendPlainText(beijingTime(QDateTime::currentMSecsSinceEpoch(),"HH:mm:ss")+"  "+message);}
void Window::showOutputOff() {
    outputReading->setText("已关断");measuredReading->setText("不可用");
    outputReading->setToolTip("CH2 输出已关断；保留仪器最后的设定电流，不下发 0 mA。");
    measuredReading->setToolTip("CH2 高阻关断时继电器断开，无有效负载电流实测值。恢复输出后重新采集。");
    currentTrace->setToolTip("CH2 已关断，当前显示关断前的电流历史曲线；关断期间不补写 0 或旧值。");
}
void Window::clearLiveReadings(const QString& reason) {
    for(auto* label:{probeReading,outputReading,measuredReading}) {
        label->setText("—");label->setToolTip(reason);
    }
    values->setText(reason);values->setAccessibleDescription(reason);
}
void Window::start(bool isManual) {
    if(active || recorder) return;
    if(!session || !sessionReady) {
        append("请先成功连接电源，再启动控制。");return;
    }
    Settings s;s.mode=driverSimulation ? 1 : 2;s.resource=resource->text();s.targetV=target->value();s.kp=kp->value();s.ti=ti->value();s.td=td->value();s.limitMA=limit->value();s.probeVoltageLimit=probeVoltageLimit->value();s.voltageLimit=voltageLimit->value();s.overrideVoltageLimit=overrideVoltageLimit->isChecked();s.nplc=nplc->value();s.manualMA=manual->value();s.direction=direction->currentIndex()==0?1:-1;s.periodMs=period->value();s.manual=isManual;
    s.currentTest=false;
    try{s.validate();}catch(const std::exception& e){hadError=true;append(QString::fromUtf8(e.what()));return;}
    hadError=false;sampleCount=0;activeSettings=s;active=true;preparingControl=true;stopping=false;targetPending=false;storageFailed=false;
    clearLiveReadings("正在准备控制，等待新的实时采样。");
    trace->resetData(s.targetV);currentTrace->resetData(s.manual ? s.manualMA : 0);
    if(!saveData->isChecked()) {storageStatus->setStyleSheet("color:#536575;");storageStatus->setText("本次未启用数据存储。");refreshControls();beginControl();return;}
    RecordSettings options;options.directory=storageDirectory->text().trimmed();options.frequencyHz=storageFrequency->currentData().toDouble();
    options.simulated=s.mode!=2;
    recorderFinished=false;recorder=new DataRecorder(options,this);auto* writer=recorder;
    connect(session,&ControlThread::recordData,writer,&DataRecorder::submit,Qt::DirectConnection);
    connect(writer,&DataRecorder::ready,this,[this,writer]{if(recorder==writer && preparingControl && !stopping && !closing) beginControl();});
    connect(writer,&DataRecorder::fileChanged,this,[this,writer](QString path){
        if(recorder!=writer)return;
        storageStatus->setStyleSheet("color:#18703c;");storageStatus->setText("存储文件："+QDir::toNativeSeparators(path));append("数据存储文件："+path);
    });
    connect(writer,&DataRecorder::failed,this,[this,writer](QString error){
        if(recorder!=writer)return;
        storageFailed=true;storageStatus->setStyleSheet("color:#b42318;");
        const QString text="数据存储失败："+error+(preparingControl ? "；已取消启动。" : "；控制状态未改变，请检查存储目录。");
        storageStatus->setText(text);append(text);if(preparingControl)finishRun();
    });
    connect(writer,&DataRecorder::saved,this,[this,writer](quint64 rows){if(recorder==writer && !storageFailed) {storageStatus->setText(QString("数据已保存，共 %1 条记录。").arg(rows));append(storageStatus->text());}});
    connect(writer,&QThread::finished,this,[this,writer]{if(recorder==writer) {recorderFinished=true;if(!active)releaseRecorder();}});
    storageStatus->setStyleSheet("color:#536575;");storageStatus->setText("正在准备数据存储……");refreshControls();writer->start();
}
void Window::beginControl() {
    if(!active || !preparingControl || stopping || closing || !session || !sessionReady) return;
    preparingControl=false;
    if(!session->startControl(activeSettings)) {
        active=false;sessionReady=false;session->disconnectDevice();refreshControls();
        if(recorder)recorder->finish();
        showConnectionStatus("连接不可用，请重新连接","#b42318");return;
    }
    showConnectionStatus("已连接 · "+identity+" · 控制运行中","#18703c");
    refreshControls();
}
void Window::finishRun() {
    notifyStopped=notifyStopped || active;
    active=false;preparingControl=false;stopping=false;targetPending=false;
    if(recorder)recorder->finish();
    releaseRecorder();
}
void Window::releaseRecorder() {
    if(recorder && !recorderFinished) {refreshControls();return;}
    if(recorder) {recorder->deleteLater();recorder=nullptr;}
    refreshControls();
    if(notifyStopped) {notifyStopped=false;emit stopped();}
    if(closing && !session)close();
}
void Window::applyTarget() {
    if(!session || !sessionReady || !active || stopping || closing || hadError || targetPending
        || sampleCount==0 || activeSettings.manual) return;
    if(!target->hasAcceptableInput()) {append("目标未应用：请输入 -10～+10 V 范围内的有效电压。");return;}
    target->interpretText();
    const double volts=target->value();
    if(std::abs(volts)>activeSettings.probeVoltageLimit) {
        append(QString("目标未应用：目标电压不得超过探头保护阈值 ±%1 V。")
               .arg(activeSettings.probeVoltageLimit,0,'g',8));return;
    }
    if(volts==activeSettings.targetV) return;
    if(!session->requestTargetVoltage(volts)) {append("目标未应用：PID 已停止或当前无法接受更新。");return;}
    targetPending=true;refreshControls();
    append(QString("已提交目标 %1 V，等待控制线程应用。").arg(volts,0,'f',4));
}
void Window::attachSignals(ControlThread* thread) {
    connect(thread,&ControlThread::message,this,&Window::append);
    connect(thread,&ControlThread::voltageLimitRead,this,[this](double volts){
        voltageLimitStatus->setText(QString("%1仪器读回：±%2 V").arg(driverSimulation ? "【仿真】" : "").arg(volts,0,'g',8));
    });
    connect(thread,&ControlThread::failed,this,[this](QString message){
        hadError=true;sessionReady=false;targetPending=false;append("错误："+message);
        clearLiveReadings("采集异常，当前实时值不可用。");
        showConnectionStatus("连接或运行异常 · "+message,"#b42318");refreshControls();
    });
    connect(thread,&ControlThread::targetApplied,this,[this](double volts){
        targetPending=false;
        append(QString("目标电压已应用：%1 V，PID 持续运行。").arg(volts,0,'f',4));
        refreshControls();
        // The following sample carries the exact target used for that computation.
    });
    connect(thread,&ControlThread::sample,this,[this](double t,double v,double command,double measured,double dt,double targetV){
        ++sampleCount; // Includes the final recorded sample while closing/draining the worker.
        if(hadError || !sessionReady || closing) return;
        activeSettings.targetV=targetV;
        const Settings& s=activeSettings;
        const QString prefix=s.mode==2 ? "" : "【仿真数据】 ";
        probeReading->setText(QString::number(v,'f',5));targetReading->setText(QString::number(s.targetV,'f',5));
        outputReading->setText(QString::number(command,'f',4));measuredReading->setText(QString::number(measured,'f',4));
        if(sampleCount==1) {
            probeReading->setToolTip("CH1 最新采集的探头反馈电压。");
            outputReading->setToolTip("实际下发至 CH2 的控制电流。");
            measuredReading->setToolTip("CH2 输出开启时采集的实测电流。");currentTrace->setToolTip("");
            refreshControls();
        }
        values->setText(prefix+QString("电压误差 %1 V    ·    实际周期 %2 ms").arg(s.targetV-v,0,'f',5).arg(dt,0,'f',1));
        values->setAccessibleDescription(prefix+QString("CH1 探头电压 %1 V  |  目标电压 %2 V  |  电压误差 %3 V\nCH2 输出电流 %4 mA  |  实测电流 %5 mA  |  实际周期 %6 ms").arg(v,0,'f',5).arg(s.targetV,0,'f',5).arg(s.targetV-v,0,'f',5).arg(command,0,'f',4).arg(measured,0,'f',4).arg(dt,0,'f',1));
        trace->appendSample(t,v,targetV);
        currentTrace->appendCurrentSample(t,command,measured);
    });
    connect(thread,&ControlThread::monitorSample,this,[this](double t,double v,double dt){
        if(!sessionReady || active || hadError || closing) return;
        showOutputOff();probeReading->setText(QString::number(v,'f',5));
        probeReading->setToolTip("CH1 在 0 A 测压状态下持续采集，包括 CH2 关断期间。");
        targetReading->setText(QString::number(target->value(),'f',5));
        const QString cadence=dt>0 ? QString("采集周期 %1 ms").arg(dt,0,'f',1) : "首个采集点";
        const QString prefix=driverSimulation ? "【仿真数据】 " : "";
        values->setText(prefix+"探头持续采集 · "+cadence+" · CH2 已关断，实测电流不可用；电流曲线为历史数据。");
        values->setAccessibleDescription(values->text());
        trace->appendSample(t,v,target->value());
    });
}
void Window::stop(){if(active){
    if(preparingControl) {append("已取消启动。");finishRun();return;}
    stopping=true;targetPending=false;
    if(session)session->requestStop();
    refreshControls();append("正在停止，等待当前 I/O 返回并关断输出……");
}}
void Window::closeEvent(QCloseEvent* event){
    if(session || recorder){
        closing=true;sessionReady=false;
        if(session)session->disconnectDevice();
        // A running session may still return its last sample before acknowledging stop.
        if(recorder && (!session || !active || preparingControl))recorder->finish();
        refreshControls();event->ignore();
    }
    else event->accept();
}

#include "window.h"
#include "traceplot.h"
#include <QtWidgets>
#include <algorithm>

void Window::buildUi() {
    setObjectName("mainWindow");
    setWindowTitle(QApplication::applicationDisplayName());
    const QRect available=QGuiApplication::primaryScreen()->availableGeometry();
    setMinimumSize(1100,680);
    resize(std::min(1360,available.width()-48),std::min(900,available.height()-64));
    QFile theme(":/styles/app.qss");
    if(theme.open(QIODevice::ReadOnly)) setStyleSheet(QString::fromUtf8(theme.readAll()));
    auto* root=new QVBoxLayout(this);root->setContentsMargins(12,10,12,10);root->setSpacing(8);
    auto panel=[] {
        auto* frame=new QFrame;frame->setProperty("panel",true);return frame;
    };
    auto section=[](const QString& text) {
        auto* label=new QLabel(text);label->setProperty("section",true);return label;
    };
    auto muted=[](const QString& text) {
        auto* label=new QLabel(text);label->setProperty("muted",true);return label;
    };

    auto* header=new QFrame;header->setObjectName("appHeader");
    auto* heading=new QHBoxLayout(header);heading->setContentsMargins(15,6,18,6);heading->setSpacing(12);
    auto* logo=new QLabel;logo->setPixmap(QApplication::windowIcon().pixmap(42,42));logo->setFixedSize(42,42);heading->addWidget(logo);
    auto* titles=new QVBoxLayout;titles->setSpacing(2);
    auto* title=new QLabel(QApplication::applicationDisplayName());title->setObjectName("appTitle");titles->addWidget(title);
    auto* subtitle=new QLabel("B2912C  /  CH1 探头电压 · CH2 控制电流");subtitle->setObjectName("appSubtitle");titles->addWidget(subtitle);
    heading->addLayout(titles,1);
    beijingClock=new QLabel;beijingClock->setObjectName("beijingClock");
    beijingClock->setToolTip("北京时间（UTC+8），根据本机系统时钟换算。");heading->addWidget(beijingClock);root->addWidget(header);
    auto updateClock=[this]{beijingClock->setText("北京时间  "+beijingTime(QDateTime::currentMSecsSinceEpoch(),"yyyy-MM-dd HH:mm:ss"));};
    auto* clockTimer=new QTimer(this);connect(clockTimer,&QTimer::timeout,this,updateClock);clockTimer->start(250);updateClock();

    auto* connectionPanel=panel();auto* connectionLayout=new QVBoxLayout(connectionPanel);
    connectionLayout->setContentsMargins(14,6,14,6);connectionLayout->setSpacing(5);
    auto* addressRow=new QHBoxLayout;addressRow->setSpacing(10);addressRow->addWidget(section("仪器连接"));
    addressRow->addSpacing(10);addressRow->addWidget(muted("VISA 地址"));
    resource=new QLineEdit("TCPIP0::169.254.215.212::hislip0::INSTR");resource->setObjectName("visaAddress");resource->setPlaceholderText("从 Connection Expert 复制完整 VISA 地址");
    connectButton=new QPushButton("连接电源");connectButton->setObjectName("testConnectionButton");
    connectButton->setToolTip("连接前请先关闭仪器两个通道。连接后 CH1 开启 0 A 测压并持续采集，CH2 保持关断；断开时关闭两个通道。");
    addressRow->addWidget(resource,1);addressRow->addWidget(connectButton);connectionLayout->addLayout(addressRow);
    connectionStatus=new QLabel;connectionStatus->setObjectName("connectionStatus");connectionStatus->setWordWrap(true);connectionStatus->setTextFormat(Qt::PlainText);
    auto* statusRow=new QHBoxLayout;statusRow->addWidget(connectionStatus,1);
    loopLamp=new QLabel;loopLamp->setObjectName("loopLamp");loopLamp->setFixedSize(12,12);
    loopState=new QLabel;loopState->setObjectName("loopState");statusRow->addWidget(loopLamp);statusRow->addWidget(loopState);connectionLayout->addLayout(statusRow);
    root->addWidget(connectionPanel);

    auto* body=new QHBoxLayout;body->setSpacing(8);
    auto* controlPanel=panel();controlPanel->setObjectName("controlPanel");controlPanel->setMinimumWidth(310);controlPanel->setMaximumWidth(345);
    auto* controls=new QVBoxLayout(controlPanel);controls->setContentsMargins(12,10,12,10);controls->setSpacing(9);
    controls->addWidget(section("控制参数"));
    auto* scroll=new QScrollArea;scroll->setObjectName("parameterScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);scroll->viewport()->setAutoFillBackground(false);
    parameters=new QWidget;parameters->setObjectName("parameterContents");
    auto* grid=new QGridLayout(parameters);grid->setContentsMargins(0,0,5,0);grid->setHorizontalSpacing(10);grid->setVerticalSpacing(8);
    grid->setColumnStretch(0,1);grid->setColumnStretch(1,1);
    auto field=[&](const QString& text,QWidget* widget,int row,int column,int span=1) {
        auto* cell=new QWidget;auto* layout=new QVBoxLayout(cell);layout->setContentsMargins(0,0,0,0);layout->setSpacing(4);
        auto* label=new QLabel(text);label->setProperty("field",true);layout->addWidget(label);layout->addWidget(widget);
        grid->addWidget(cell,row,column,1,span);
    };
    auto spin=[](double value,double minimum,double maximum) {
        auto* box=new QDoubleSpinBox;box->setDecimals(4);box->setRange(minimum,maximum);box->setValue(value);box->setSingleStep(.1);return box;
    };
    target=spin(Settings{}.targetV,-10,10);target->setObjectName("targetVoltage");target->setKeyboardTracking(false);
    target->setToolTip("CH1 探头电压的目标值，范围 -10～+10 V。\nPID 运行中修改后，点击“应用目标”才生效；启动前直接填写即可。");
    auto* targetCell=new QWidget;auto* targetRow=new QHBoxLayout(targetCell);targetRow->setContentsMargins(0,0,0,0);targetRow->setSpacing(7);
    applyTargetButton=new QPushButton("应用目标");applyTargetButton->setObjectName("applyTargetButton");
    applyTargetButton->setToolTip("PID 运行中应用新目标，保持输出开启和积分状态；继续执行现有保护。");
    targetRow->addWidget(target,1);targetRow->addWidget(applyTargetButton);field("目标电压（V）",targetCell,0,0,2);
    kp=spin(Settings{}.kp,0,1e4);kp->setObjectName("proportionalGain");kp->setToolTip("比例增益：每 1 V 电压误差对应多少 mA 控制电流。");field("Kp（mA/V）",kp,1,0);
    limit=spin(10,.001,10);limit->setToolTip("客户允许范围为 -10～+10 mA；测试时可设更小限幅。");field("电流限幅（±mA）",limit,1,1);
    ti=spin(Settings{}.ti,0,1e4);ti->setObjectName("integralTime");td=spin(0,0,100);field("Ti（s，0 关闭积分）",ti,2,0);field("Td（s）",td,2,1);
    manual=spin(0,-10,10);manual->setObjectName("manualCurrent");field("手动电流（mA）",manual,3,0);
    direction=new QComboBox;direction->addItems({"+1 正电流使探头电压增大","-1 正电流使探头电压减小"});field("控制方向",direction,4,0,2);
    voltageLimitStatus=new QLabel;voltageLimitStatus->setObjectName("voltageLimitStatus");voltageLimitStatus->setWordWrap(true);
    field("CH2 电压上限",voltageLimitStatus,3,1);
    auto* advancedToggle=new QToolButton;advancedToggle->setText("高级设置");advancedToggle->setCheckable(true);advancedToggle->setObjectName("advancedInstrumentToggle");
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advancedToggle->setArrowType(Qt::RightArrow);grid->addWidget(advancedToggle,5,0,1,2,Qt::AlignLeft);
    auto* advanced=new QWidget;auto* advancedLayout=new QVBoxLayout(advanced);advancedLayout->setContentsMargins(0,0,0,0);advancedLayout->setSpacing(6);
    auto* samplingLayout=new QGridLayout;samplingLayout->setHorizontalSpacing(10);samplingLayout->setVerticalSpacing(4);
    samplingLayout->setColumnStretch(0,1);samplingLayout->setColumnStretch(1,1);
    auto* nplcLabel=new QLabel("测量积分时间（NPLC）");nplcLabel->setProperty("field",true);samplingLayout->addWidget(nplcLabel,0,0);
    auto* periodLabel=new QLabel("期望采样周期");periodLabel->setProperty("field",true);samplingLayout->addWidget(periodLabel,0,1);
    nplc=spin(Settings{}.nplc,.01,10);nplc->setObjectName("measurementNplc");samplingLayout->addWidget(nplc,1,0);
    nplc->setToolTip("测量积分时间：决定仪器每次对电压、电流信号积分取平均的时间。\n"
                     "读数噪声较大时可适当调大，读数通常更稳定，但测量更慢。\n"
                     "测量耗时过长时可适当调小，测量更快，但噪声可能增大。");nplc->setToolTipDuration(20000);
    period=new QSpinBox;period->setRange(20,2000);period->setValue(100);period->setSuffix(" ms");samplingLayout->addWidget(period,1,1);
    advancedLayout->addLayout(samplingLayout);
    overrideVoltageLimit=new QCheckBox("覆盖仪器 CH2 电压上限");overrideVoltageLimit->setObjectName("overrideVoltageLimit");advancedLayout->addWidget(overrideVoltageLimit);
    voltageLimit=spin(5,.1,20);voltageLimit->setObjectName("voltageLimitOverride");voltageLimit->setSuffix(" V");voltageLimit->setEnabled(false);
    voltageLimit->setToolTip("仅勾选覆盖时生效；取消勾选则读取并保留仪器当前上限。");advancedLayout->addWidget(voltageLimit);
    auto* advancedHint=muted("默认保留仪器原值，需要调整时才勾选覆盖。");advancedHint->setWordWrap(true);advancedLayout->addWidget(advancedHint);
    grid->addWidget(advanced,6,0,1,2);advanced->hide();grid->setRowStretch(7,1);
    connect(advancedToggle,&QToolButton::toggled,this,[scroll,advanced,advancedToggle](bool visible){
        advanced->setVisible(visible);advancedToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        if(visible) QTimer::singleShot(0,scroll,[scroll,advanced]{scroll->ensureWidgetVisible(advanced,0,8);});
    });
    connect(overrideVoltageLimit,&QCheckBox::toggled,voltageLimit,&QDoubleSpinBox::setEnabled);
    scroll->setWidget(parameters);controls->addWidget(scroll,1);
    auto* startRow=new QHBoxLayout;startRow->setSpacing(8);
    startButton=new QPushButton("开始PID闭环输出");startButton->setObjectName("startControlButton");manualButton=new QPushButton("开始手动输出");
    startButton->setMinimumHeight(34);manualButton->setMinimumHeight(34);startRow->addWidget(startButton);startRow->addWidget(manualButton);controls->addLayout(startRow);
    stopButton=new QPushButton("停止并关断");stopButton->setObjectName("stopControlButton");stopButton->setMinimumHeight(34);stopButton->setEnabled(false);controls->addWidget(stopButton);
    stopButton->setToolTip("直接关断 CH2 控制输出，保留 CH1 探头电压采集；不写入 0 mA。故障或断开连接时关闭两个通道。");
    body->addWidget(controlPanel);

    auto* monitor=new QVBoxLayout;monitor->setSpacing(6);
    auto* readingsPanel=panel();auto* readings=new QGridLayout(readingsPanel);readings->setContentsMargins(12,5,12,5);readings->setHorizontalSpacing(12);readings->setVerticalSpacing(2);
    auto reading=[&](const QString& label,const QString& name,int column) {
        readings->addWidget(muted(label),0,column);
        auto* value=new QLabel("—");value->setObjectName(name);value->setProperty("reading",true);readings->addWidget(value,1,column);readings->setColumnStretch(column,1);return value;
    };
    probeReading=reading("探头电压 / V","probeReading",0);targetReading=reading("目标电压 / V","targetReading",1);
    outputReading=reading("输出电流 / mA","outputReading",2);measuredReading=reading("实测电流 / mA","measuredReading",3);
    values=new QLabel("未运行 · 等待采样");values->setObjectName("measurementValues");readings->addWidget(values,2,0,1,4);monitor->addWidget(readingsPanel);

    auto* monitorTabs=new QTabWidget;monitorTabs->setObjectName("monitorTabs");monitorTabs->setDocumentMode(true);
    auto* plotsPanel=new QWidget;plotsPanel->setObjectName("curvesPage");auto* plotsLayout=new QVBoxLayout(plotsPanel);plotsLayout->setContentsMargins(8,4,8,6);plotsLayout->setSpacing(2);
    auto* plotTools=new QHBoxLayout;plotTools->setSpacing(10);
    auto* plotHint=muted("整秒平均（1秒/点）  ·  保留最近24小时  ·  拖动/滚轮后暂停  ·  双击恢复动态显示");
    plotHint->setToolTip("拖动或滚轮缩放后，当前画面停止自动刷新，后台继续采集并保留曲线数据；双击图像区域后显示全部保留数据并恢复动态更新。曲线按采集时间的整秒窗口取平均，无数据的秒不补点；1 Hz文件与控制期间的曲线均值对齐。");
    plotTools->addWidget(plotHint,1);
    auto* clearCurveButton=new QPushButton("清除曲线");clearCurveButton->setObjectName("clearCurveButton");
    clearCurveButton->setToolTip("清除历史采样曲线，保留目标参考线；运行中的采样和输出继续进行。");plotTools->addWidget(clearCurveButton);plotsLayout->addLayout(plotTools);
    auto* plots=new QSplitter(Qt::Vertical);plots->setObjectName("curveSplitter");plots->setChildrenCollapsible(false);plots->setHandleWidth(5);
    trace=new TracePlot;trace->setObjectName("probePlot");currentTrace=new TracePlot;currentTrace->setObjectName("currentPlot");currentTrace->setCurrentMode(true);currentTrace->resetData(0);
    plots->addWidget(trace);plots->addWidget(currentTrace);plots->setStretchFactor(0,1);plots->setStretchFactor(1,1);plots->setSizes({1,1});plotsLayout->addWidget(plots,1);
    auto* logPanel=new QWidget;logPanel->setObjectName("logPage");auto* logLayout=new QVBoxLayout(logPanel);logLayout->setContentsMargins(8,6,8,8);logLayout->setSpacing(0);
    log=new QPlainTextEdit;log->setObjectName("runLog");log->setReadOnly(true);log->setPlaceholderText("连接、运行及保护信息将在这里显示。");
    log->setMaximumBlockCount(300);logLayout->addWidget(log);
    monitorTabs->addTab(plotsPanel,"实时曲线");monitorTabs->addTab(logPanel,"运行日志");monitorTabs->setCurrentIndex(0);monitor->addWidget(monitorTabs,1);
    body->addLayout(monitor,1);root->addLayout(body,1);

    auto* storagePanel=panel();auto* storageLayout=new QVBoxLayout(storagePanel);storageLayout->setContentsMargins(14,5,14,5);storageLayout->setSpacing(5);
    auto* storageRow=new QHBoxLayout;storageRow->setSpacing(9);
    saveData=new QCheckBox("存储数据（TXT）");saveData->setObjectName("saveData");saveData->setChecked(true);
    storageFrequency=new QComboBox;storageFrequency->setObjectName("storageFrequency");
    for(int hz:RecordSettings::supportedFrequenciesHz)storageFrequency->addItem(QString::number(hz)+" Hz",hz);
    storageFrequency->setCurrentIndex(storageFrequency->findData(static_cast<int>(RecordSettings{}.frequencyHz)));
    storageFrequency->setToolTip("1 Hz：保存与曲线对齐的整秒平均值，停止时未完成的最后一秒不另存。10 Hz：每收到一个原始采样点就保存一条，不平均、不抽点；实际条数取决于采集速度。默认1 Hz，停止后可修改。");
    storageDirectory=new QLineEdit(defaultDataDirectory());storageDirectory->setObjectName("storageDirectory");
    if(driverSimulation && !qEnvironmentVariable("TEST_OUTPUT_DIR").isEmpty())storageDirectory->setText(QDir(qEnvironmentVariable("TEST_OUTPUT_DIR")).filePath("recordings"));
    browseStorage=new QPushButton("选择目录");openStorage=new QPushButton("打开目录");
    storageRow->addWidget(saveData);storageRow->addSpacing(5);storageRow->addWidget(muted("存储频率"));storageRow->addWidget(storageFrequency);
    storageRow->addWidget(storageDirectory,1);storageRow->addWidget(browseStorage);storageRow->addWidget(openStorage);storageLayout->addLayout(storageRow);
    storageStatus=muted("勾选后在启动控制时保存数据，按整点分文件，同一小时继续追加。");storageStatus->setObjectName("storageStatus");
    saveData->setToolTip("按北京时间每个自然小时保存一个TXT，例如FB_261028_10.txt；同一小时停止后再次开始，或重启软件后继续存储，均追加到原文件。无表头，时间及四项数据用空格分隔。");
    storageStatus->setWordWrap(true);storageStatus->setTextFormat(Qt::PlainText);storageLayout->addWidget(storageStatus);root->addWidget(storagePanel);

    connect(applyTargetButton,&QPushButton::clicked,this,&Window::applyTarget);
    connect(startButton,&QPushButton::clicked,this,[this]{start(false);});connect(manualButton,&QPushButton::clicked,this,[this]{start(true);});connect(stopButton,&QPushButton::clicked,this,&Window::stop);
    connect(clearCurveButton,&QPushButton::clicked,this,[this]{trace->clearData();currentTrace->clearData();});
    connect(browseStorage,&QPushButton::clicked,this,[this]{
        const QString directory=QFileDialog::getExistingDirectory(this,"选择数据存储目录",storageDirectory->text());if(!directory.isEmpty())storageDirectory->setText(directory);
    });
    connect(openStorage,&QPushButton::clicked,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(storageDirectory->text()));});
    connect(saveData,&QCheckBox::toggled,this,[this]{refreshControls();});
    connect(resource,&QLineEdit::textChanged,this,[this]{resetConnectionStatus();});connect(connectButton,&QPushButton::clicked,this,&Window::testConnection);
    trace->resetData(target->value());resetConnectionStatus();
}

// Layout adapted from src/windowui.cpp; hardware and PID controls are inert in this build.
#include "stability.h"
#include "traceplot.h"
#include <QtWidgets>
#include <algorithm>

void StabilityWindow::buildUi() {
    setObjectName("mainWindow");
    setWindowTitle(QApplication::applicationDisplayName());
    const QRect available=QGuiApplication::primaryScreen()->availableGeometry();
    setMinimumSize(1100,680);
    resize(std::min(1360,available.width()-48),std::min(900,available.height()-64));
    QFile theme(":/styles/app.qss");
    if(theme.open(QIODevice::ReadOnly)) setStyleSheet(QString::fromUtf8(theme.readAll()));
    auto* root=new QVBoxLayout(this);root->setContentsMargins(16,12,16,12);root->setSpacing(10);
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
    auto* subtitle=new QLabel("离线稳定性测试 · 模拟数据 · 不连接仪器，不执行 PID");subtitle->setObjectName("appSubtitle");titles->addWidget(subtitle);
    heading->addLayout(titles,1);
    clock=new QLabel;clock->setObjectName("beijingClock");
    clock->setToolTip("北京时间（UTC+8），根据本机系统时钟换算。");heading->addWidget(clock);root->addWidget(header);
    auto updateClock=[this]{clock->setText("北京时间  "+beijingTime(QDateTime::currentMSecsSinceEpoch(),"yyyy-MM-dd HH:mm:ss"));};
    auto* clockTimer=new QTimer(this);connect(clockTimer,&QTimer::timeout,this,updateClock);clockTimer->start(250);updateClock();

    auto* connectionPanel=panel();auto* connectionLayout=new QVBoxLayout(connectionPanel);
    connectionLayout->setContentsMargins(14,6,14,6);connectionLayout->setSpacing(5);
    auto* addressRow=new QHBoxLayout;addressRow->setSpacing(10);addressRow->addWidget(section("仪器连接"));
    addressRow->addSpacing(10);addressRow->addWidget(muted("VISA 地址"));
    resource=new QLineEdit("TCPIP0::169.254.215.212::hislip0::INSTR");resource->setObjectName("visaAddress");resource->setPlaceholderText("从 Connection Expert 复制完整 VISA 地址");
    connectButton=new QPushButton("离线测试");connectButton->setObjectName("testConnectionButton");
    connectButton->setToolTip("连接前请先关闭仪器两个通道。连接后 CH1 开启 0 A 测压并持续采集，CH2 保持关断；断开时关闭两个通道。");
    addressRow->addWidget(resource,1);addressRow->addWidget(connectButton);connectionLayout->addLayout(addressRow);
    state=new QLabel;state->setObjectName("state");state->setWordWrap(true);state->setTextFormat(Qt::PlainText);
    auto* statusRow=new QHBoxLayout;statusRow->addWidget(state,1);
    loopLamp=new QLabel;loopLamp->setObjectName("loopLamp");loopLamp->setFixedSize(12,12);
    loopState=new QLabel;loopState->setObjectName("loopState");statusRow->addWidget(loopLamp);statusRow->addWidget(loopState);connectionLayout->addLayout(statusRow);
    root->addWidget(connectionPanel);

    auto* body=new QHBoxLayout;body->setSpacing(10);
    auto* controlPanel=panel();controlPanel->setMinimumWidth(350);controlPanel->setMaximumWidth(395);
    auto* controls=new QVBoxLayout(controlPanel);controls->setContentsMargins(14,12,14,12);controls->setSpacing(10);
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
    target=spin(0.0,-10,10);target->setObjectName("targetVoltage");target->setKeyboardTracking(false);
    target->setToolTip("CH1 探头电压的目标值，范围 -10～+10 V。\nPID 运行中修改后，点击“应用目标”才生效；启动前直接填写即可。");
    auto* targetCell=new QWidget;auto* targetRow=new QHBoxLayout(targetCell);targetRow->setContentsMargins(0,0,0,0);targetRow->setSpacing(7);
    applyTargetButton=new QPushButton("应用目标");applyTargetButton->setObjectName("applyTargetButton");
    applyTargetButton->setToolTip("PID 运行中应用新目标，保持输出开启和积分状态；继续执行现有保护。");
    targetRow->addWidget(target,1);targetRow->addWidget(applyTargetButton);field("目标电压（V）",targetCell,0,0,2);
    kp=spin(0.2,0,1e4);kp->setObjectName("proportionalGain");kp->setToolTip("比例增益：每 1 V 电压误差对应多少 mA 控制电流。");field("Kp（mA/V）",kp,1,0);
    limit=spin(10,.001,10);limit->setToolTip("客户允许范围为 -10～+10 mA；测试时可设更小限幅。");field("电流限幅（±mA）",limit,1,1);
    ti=spin(0.1,0,1e4);td=spin(0,0,100);field("Ti（s，0 关闭积分）",ti,2,0);field("Td（s）",td,2,1);
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
    nplc=spin(.1,.01,10);nplc->setObjectName("nplc");samplingLayout->addWidget(nplc,1,0);
    nplc->setToolTip("测量积分时间：决定仪器每次对电压、电流信号积分取平均的时间。\n"
                     "读数噪声较大时可适当调大，读数通常更稳定，但测量更慢。\n"
                     "测量耗时过长时可适当调小，测量更快，但噪声可能增大。");nplc->setToolTipDuration(20000);
    period=new QSpinBox;period->setObjectName("period");period->setRange(20,2000);period->setValue(100);period->setSuffix(" ms");period->setToolTip("离线数据生成周期，不执行仪器采样。");samplingLayout->addWidget(period,1,1);
    advancedLayout->addLayout(samplingLayout);
    overrideVoltageLimit=new QCheckBox("覆盖仪器 CH2 电压上限");overrideVoltageLimit->setObjectName("overrideVoltageLimit");advancedLayout->addWidget(overrideVoltageLimit);
    voltageLimit=spin(5,.1,20);voltageLimit->setObjectName("voltageLimitOverride");voltageLimit->setSuffix(" V");voltageLimit->setEnabled(false);
    voltageLimit->setToolTip("仅勾选覆盖时生效；取消勾选则读取并保留仪器当前上限。");advancedLayout->addWidget(voltageLimit);
    auto* advancedHint=muted("仪器参数仅保留界面显示，离线测试中不生效。");advancedHint->setWordWrap(true);advancedLayout->addWidget(advancedHint);
    duration=new QDoubleSpinBox;duration->setObjectName("duration");duration->setDecimals(3);
    duration->setRange(0,168);duration->setValue(24);duration->setSuffix(" 小时");
    duration->setSpecialValueText("持续运行，直到手动停止");
    advancedLayout->addWidget(muted("离线测试时长（0为不限时）"));advancedLayout->addWidget(duration);
    draw=new QCheckBox("启用曲线绘制");draw->setObjectName("draw");draw->setChecked(true);
    draw->setToolTip("停止后取消勾选再运行，可对比不绘图时的延迟；采样和存储仍继续。");advancedLayout->addWidget(draw);
    grid->addWidget(advanced,6,0,1,2);advanced->hide();grid->setRowStretch(7,1);
    connect(advancedToggle,&QToolButton::toggled,this,[scroll,advanced,advancedToggle](bool visible){
        advanced->setVisible(visible);advancedToggle->setArrowType(visible ? Qt::DownArrow : Qt::RightArrow);
        if(visible) QTimer::singleShot(0,scroll,[scroll,advanced]{scroll->ensureWidgetVisible(advanced,0,8);});
    });
    connect(overrideVoltageLimit,&QCheckBox::toggled,voltageLimit,&QDoubleSpinBox::setEnabled);
    scroll->setWidget(parameters);controls->addWidget(scroll,1);
    auto* startRow=new QHBoxLayout;startRow->setSpacing(8);
    start=new QPushButton("开始离线测试");start->setObjectName("startControlButton");manualButton=new QPushButton("开始手动输出");
    start->setMinimumHeight(34);manualButton->setMinimumHeight(34);startRow->addWidget(start);startRow->addWidget(manualButton);controls->addLayout(startRow);
    stop=new QPushButton("停止测试并保存");stop->setObjectName("stopControlButton");stop->setMinimumHeight(34);stop->setEnabled(false);controls->addWidget(stop);
    stop->setToolTip("直接关断 CH2 控制输出，保留 CH1 探头电压采集；不写入 0 mA。故障或断开连接时关闭两个通道。");
    body->addWidget(controlPanel);

    auto* monitor=new QVBoxLayout;monitor->setSpacing(10);
    auto* readingsPanel=panel();auto* readingLayout=new QGridLayout(readingsPanel);readingLayout->setContentsMargins(15,8,15,8);readingLayout->setHorizontalSpacing(12);readingLayout->setVerticalSpacing(3);
    auto reading=[&](const QString& label,const QString& name,int column) {
        readingLayout->addWidget(muted(label),0,column);
        auto* value=new QLabel("—");value->setObjectName(name);value->setProperty("reading",true);readingLayout->addWidget(value,1,column);readingLayout->setColumnStretch(column,1);return value;
    };
    probeReading=reading("探头电压 / V","probeReading",0);targetReading=reading("目标电压 / V","targetReading",1);
    outputReading=reading("输出电流 / mA","outputReading",2);measuredReading=reading("实测电流 / mA","measuredReading",3);
    readings=new QLabel("未运行 · 等待采样");readings->setObjectName("measurementValues");readings->setWordWrap(true);readingLayout->addWidget(readings,2,0,1,4);health=new QLabel("默认测试24小时 · 延迟超过1秒仅记录，继续运行");
    health->setProperty("muted",true);health->setWordWrap(true);readingLayout->addWidget(health,3,0,1,4);
    monitor->addWidget(readingsPanel);

    auto* plotsPanel=panel();auto* plotsLayout=new QVBoxLayout(plotsPanel);plotsLayout->setContentsMargins(12,6,12,6);plotsLayout->setSpacing(3);
    auto* plotTools=new QHBoxLayout;plotTools->addWidget(section("实时曲线"));plotTools->addStretch();
    auto* clearCurveButton=new QPushButton("清除曲线");clearCurveButton->setObjectName("clearCurveButton");
    clearCurveButton->setToolTip("清除历史采样曲线，保留目标参考线；运行中的采样和输出继续进行。");plotTools->addWidget(clearCurveButton);plotsLayout->addLayout(plotTools);
    auto* plots=new QSplitter(Qt::Horizontal);plots->setChildrenCollapsible(false);plots->setHandleWidth(9);
    probe=new TracePlot;probe->setObjectName("probePlot");current=new TracePlot;current->setObjectName("currentPlot");current->setCurrentMode(true);current->resetData(0);
    plots->addWidget(probe);plots->addWidget(current);plotsLayout->addWidget(plots,1);
    auto* plotHint=muted("模拟数据 · 整秒平均（1秒/点）  ·  保留最近24小时  ·  拖动/滚轮缩放  ·  双击恢复全程视图");plotsLayout->addWidget(plotHint);monitor->addWidget(plotsPanel,1);
    plotHint->setToolTip("模拟原始采样按整秒求平均；1 Hz 文件与曲线均值一致。图例置于绘图区上方。");
    auto* logPanel=panel();auto* logLayout=new QVBoxLayout(logPanel);logLayout->setContentsMargins(12,4,12,4);logLayout->setSpacing(5);logLayout->addWidget(section("运行日志"));
    log=new QPlainTextEdit;log->setObjectName("runLog");log->setReadOnly(true);log->setPlaceholderText("连接、运行及保护信息将在这里显示。");
    log->setMaximumBlockCount(300);log->setMinimumHeight(42);log->setMaximumHeight(95);logLayout->addWidget(log);monitor->addWidget(logPanel);
    body->addLayout(monitor,1);root->addLayout(body,1);

    auto* storagePanel=panel();auto* storageLayout=new QVBoxLayout(storagePanel);storageLayout->setContentsMargins(14,5,14,5);storageLayout->setSpacing(5);
    auto* storageRow=new QHBoxLayout;storageRow->setSpacing(9);
    save=new QCheckBox("存储数据（TXT）");save->setObjectName("save");save->setChecked(true);
    frequency=new QComboBox;frequency->setObjectName("frequency");
    for(int hz:RecordSettings::supportedFrequenciesHz)frequency->addItem(QString::number(hz)+" Hz",hz);
    frequency->setCurrentIndex(frequency->findData(static_cast<int>(RecordSettings{}.frequencyHz)));
    frequency->setToolTip("1 Hz：保存与曲线对齐的整秒平均值，停止时未完成的最后一秒不另存。10 Hz：每收到一个原始采样点就保存一条，不平均、不抽点；实际条数取决于采集速度。默认10 Hz，停止后可修改。");
    directory=new QLineEdit(defaultDataDirectory());directory->setObjectName("directory");
    browse=new QPushButton("选择目录");openFolder=new QPushButton("打开目录");
    storageRow->addWidget(save);storageRow->addSpacing(5);storageRow->addWidget(muted("存储频率"));storageRow->addWidget(frequency);
    storageRow->addWidget(directory,1);storageRow->addWidget(browse);storageRow->addWidget(openFolder);storageLayout->addLayout(storageRow);
    files=muted("测试数据保存在独立目录；诊断日志始终记录。");files->setObjectName("files");
    files->setWordWrap(false);files->setMinimumWidth(0);files->setTextFormat(Qt::PlainText);storageLayout->addWidget(files);root->addWidget(storagePanel);

    connect(start,&QPushButton::clicked,this,&StabilityWindow::startTest);
    connect(stop,&QPushButton::clicked,this,&StabilityWindow::stopTest);
    connect(clearCurveButton,&QPushButton::clicked,this,[this]{probe->clearData();current->clearData();});
    connect(browse,&QPushButton::clicked,this,[this]{
        const QString path=QFileDialog::getExistingDirectory(this,"选择测试数据目录",directory->text());
        if(!path.isEmpty())directory->setText(path);
    });
    connect(openFolder,&QPushButton::clicked,this,[this]{QDesktopServices::openUrl(QUrl::fromLocalFile(directory->text()));});
    connect(save,&QCheckBox::toggled,this,[this]{updateControls();});
    connect(target,qOverload<double>(&QDoubleSpinBox::valueChanged),this,[this](double v){
        if(!isTesting()){probe->resetData(v);targetReading->setText(QString::number(v,'f',5));}
    });
    for(QWidget* widget:QList<QWidget*>{resource,connectButton,kp,ti,td,limit,manual,direction,nplc,
                                      overrideVoltageLimit,voltageLimit,manualButton,applyTargetButton}) {
        widget->setEnabled(false);
        widget->setToolTip("离线测试保留正式版界面；本控件不参与模拟数据生成，不连接仪器或执行 PID。");
    }
    target->setToolTip("模拟电压围绕此值随机波动；停止测试后可修改。此值不发送至任何仪器。");
    stop->setToolTip("停止模拟采样并写完数据文件，不涉及真实仪器输出。");
    voltageLimitStatus->setText("离线测试 · 不适用");
    state->setStyleSheet("color:#975d0b;font-size:12px;");
    state->setText("纯软件测试 · 无需连接电源，所有读数均为模拟数据。");
    log->setPlaceholderText("离线测试运行及性能信息将在这里显示。");
    probe->resetData(target->value());targetReading->setText("0.00000");
}

// Offline display uses a 0.25 mV/mA minimum margin; all other plot code matches production.
#include "traceplot.h"
#include <QLabel>
#include <QTimer>
#include <algorithm>
#include <cmath>

TracePlot::TracePlot(QWidget* parent) : QCustomPlot(parent) {
    setMinimumHeight(150);
    setMouseTracking(true);
    setBackground(Qt::white);
    for(auto* axis:{xAxis,yAxis}) {
        axis->setBasePen(QPen(QColor("#cedbe6")));
        axis->setTickPen(QPen(QColor("#cedbe6")));
        axis->setSubTicks(false);
        axis->setTickLabelColor(QColor("#64798c"));axis->setLabelColor(QColor("#496276"));
        axis->setTickLabelFont(QFont("Microsoft YaHei UI",9));axis->setLabelFont(QFont("Microsoft YaHei UI",9));
        axis->grid()->setPen(QPen(QColor("#e6edf3"),1,Qt::DotLine));axis->grid()->setZeroLinePen(QPen(QColor("#d3dfe9")));
    }
    setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    axisRect()->setRangeDrag(Qt::Horizontal | Qt::Vertical);
    axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    axisRect()->setRangeZoomFactor(0.85);
    auto timeTicker=QSharedPointer<QCPAxisTickerTime>::create();
    timeTicker->setTimeFormat("%h:%m:%s");
    timeTicker->setTickCount(5);
    xAxis->setTicker(timeTicker);
    xAxis->setLabel("时间（时:分:秒）");
    connect(this,&QCustomPlot::beforeReplot,this,[this,timeTicker]{
        const bool milliseconds=xAxis->range().size()<5;
        timeTicker->setTimeFormat(milliseconds ? "%h:%m:%s.%z" : "%h:%m:%s");
        xAxis->setLabel(milliseconds ? "时间（时:分:秒.毫秒）" : "时间（时:分:秒）");
        const int labelWidth=QFontMetrics(xAxis->tickLabelFont()).horizontalAdvance(
            milliseconds ? "00:00:00.000" : "00:00:00");
        timeTicker->setTickCount(qBound(2,(width()-100)/(labelWidth+28),6));
        // Keep millivolt labels legible when the production layout is used on a small screen.
        yAxis->ticker()->setTickCount(qBound(2,(height()-110)/30,6));
        axisRect()->setMinimumMargins(QMargins(0,0,labelWidth/2+8,0));
    });
    yAxis->setLabel("电压（V）");
    addGraph(); graph(0)->setName("探头电压"); graph(0)->setPen(QPen(QColor("#098c95"),2));
    addGraph(); graph(1)->setName("目标电压"); graph(1)->setPen(QPen(QColor("#d58a28"),1.5,Qt::DashLine));
    graph(1)->setLineStyle(QCPGraph::lsStepLeft);
    for(auto* curve : {graph(0),graph(1)}) curve->setAdaptiveSampling(true);
    // Store each completed one-second average and coalesce screen paints.
    redrawTimer=new QTimer(this);
    redrawTimer->setSingleShot(true);
    redrawTimer->setInterval(200);
    connect(redrawTimer,&QTimer::timeout,this,[this]{
        if(hovering) updateHover(hoverPixel);
        replot(QCustomPlot::rpQueuedReplot);
    });
    legend->setVisible(true);
    legend->setFont(QFont("Microsoft YaHei UI",9));legend->setTextColor(QColor("#415b70"));
    legend->setBrush(Qt::NoBrush);
    legend->setBorderPen(Qt::NoPen);
    // Give the legend its own row above the axes so data never runs underneath it.
    legend->setFillOrder(QCPLayoutGrid::foColumnsFirst);
    axisRect()->insetLayout()->take(legend);
    auto* legendRow=new QCPLayoutInset;
    legendRow->addElement(legend,Qt::AlignLeft | Qt::AlignVCenter);
    const int legendHeight=legend->minimumOuterSizeHint().height();
    legendRow->setMinimumSize(0,legendHeight);
    legendRow->setMaximumSize(QWIDGETSIZE_MAX,legendHeight);
    plotLayout()->insertRow(0);
    plotLayout()->addElement(0,0,legendRow);
    plotLayout()->setRowSpacing(4);
    connect(xAxis,qOverload<const QCPRange&>(&QCPAxis::rangeChanged),this,[this](const QCPRange&){
        updateTargetLine();
    });
    cursorLine = new QCPItemStraightLine(this);
    cursorLine->setPen(QPen(QColor("#8797a5"),1,Qt::DotLine));
    marker = new QCPItemTracer(this);
    marker->setGraph(graph(0)); marker->setInterpolating(false);
    marker->setStyle(QCPItemTracer::tsCircle); marker->setSize(8);
    marker->setPen(QPen(QColor("#098c95"),2)); marker->setBrush(Qt::white);
    hoverLabel = new QLabel(this);
    hoverLabel->setObjectName("plotHoverData");
    hoverLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
    hoverLabel->setStyleSheet("QLabel {background:#ffffff;color:#15334b;border:1px solid #95afc2;border-radius:4px;padding:6px;}");
    resetData(targetValue);
}
void TracePlot::setCurrentMode(bool enabled) {
    currentMode=enabled;
    yAxis->setLabel(enabled ? "电流（mA）" : "电压（V）");
    graph(0)->setName(enabled ? "输出电流" : "探头电压");
    graph(1)->setName(enabled ? "实测电流" : "目标电压");
    graph(1)->setLineStyle(enabled ? QCPGraph::lsLine : QCPGraph::lsStepLeft);
    resetData(enabled ? 0 : targetValue);
}
void TracePlot::resetData(double target) {
    targetValue=target;
    targetHistory.clear();
    averaging={};
    lastSampleTime=-std::numeric_limits<double>::infinity();
    minimums.clear();maximums.clear();redrawTimer->stop();
    graph(0)->data()->clear();
    graph(1)->data()->clear();
    autoFollow=true;
    hideHover();
    fitLatest();
    // Target may change without the x range changing.
    updateTargetLine();
    replot();
}
void TracePlot::clearData() {
    // Clearing the display must not change the current second's mean relative
    // to the recorder. Only resetData (a new run/connection) resets acquisition.
    const auto pendingAverage=averaging;
    const double previousTime=lastSampleTime;
    resetData(targetValue);
    averaging=pendingAverage;lastSampleTime=previousTime;
}
void TracePlot::appendSample(double seconds,double value) {
    appendSample(seconds,value,targetValue);
}
void TracePlot::appendSample(double seconds,double value,double target) {
    if (currentMode || !std::isfinite(seconds) || !std::isfinite(value) || !std::isfinite(target)) return;
    if(seconds<lastSampleTime) return;
    const bool targetChanged=targetHistory.isEmpty() || targetHistory.last()!=target;
    if(targetChanged) targetHistory.insert(seconds,target);
    targetValue=target;
    // Targets are commands, not measurements: preserve their actual step times.
    if(targetChanged) {
        updateTargetLine();
        if(!redrawTimer->isActive()) redrawTimer->start();
    }
    accumulateSample(seconds,value);
}
void TracePlot::appendCurrentSample(double seconds,double outputMA,double measuredMA) {
    if(!currentMode || !std::isfinite(seconds) || !std::isfinite(outputMA) || !std::isfinite(measuredMA)) return;
    if(seconds<lastSampleTime) return;
    accumulateSample(seconds,outputMA,measuredMA);
}
void TracePlot::accumulateSample(double seconds,double value,double measured) {
    SecondAverage<2>::Point completed;
    if(averaging.append(seconds,{{value,measured}},completed)) {
        const double end=completed.end;
        const double average=completed.values[0];
        const double other=currentMode ? completed.values[1] : targetAt(end);
        graph(0)->addData(end,average);
        if(currentMode) graph(1)->addData(end,other);
        finishSample(end,std::min(average,other),std::max(average,other));
    }
    lastSampleTime=seconds;
}
void TracePlot::finishSample(double seconds,double lower,double upper) {
    const auto data=graph(0)->data();
    const double cutoff=seconds-historySeconds;
    data->removeBefore(cutoff);
    if(currentMode) graph(1)->data()->removeBefore(cutoff);
    while(!minimums.empty() && minimums.back().second>=lower) minimums.pop_back();
    while(!maximums.empty() && maximums.back().second<=upper) maximums.pop_back();
    minimums.emplace_back(seconds,lower);maximums.emplace_back(seconds,upper);
    while(!minimums.empty() && minimums.front().first<cutoff) minimums.pop_front();
    while(!maximums.empty() && maximums.front().first<cutoff) maximums.pop_front();
    // Retain the target in effect at the oldest retained sample.
    while(targetHistory.size()>1 && (targetHistory.constBegin()+1).key()<=data->constBegin()->key)
        targetHistory.erase(targetHistory.begin());
    if (autoFollow) fitLatest();
    updateTargetLine();
    if(!redrawTimer->isActive()) redrawTimer->start();
}
double TracePlot::targetAt(double seconds) const {
    if(targetHistory.isEmpty()) return targetValue;
    auto it=targetHistory.upperBound(seconds);
    if(it!=targetHistory.constBegin()) --it;
    return it.value();
}
void TracePlot::updateTargetLine() {
    if(currentMode) return;
    const auto range=xAxis->range();
    QVector<double> times{range.lower},values{targetAt(range.lower)};
    for(auto it=targetHistory.upperBound(range.lower);it!=targetHistory.constEnd() && it.key()<range.upper;++it) {
        times.append(it.key());values.append(it.value());
    }
    times.append(range.upper);values.append(targetAt(range.upper));
    graph(1)->setData(times,values,true);
}
void TracePlot::fitLatest() {
    const auto data=graph(0)->data();
    double left=0,right=10;
    if (!data->isEmpty()) {
        right=std::max(data->constBegin()->key+10.0,(data->constEnd()-1)->key);
        left=std::max(data->constBegin()->key,right-historySeconds);
    }
    double lower=minimums.empty() ? (currentMode ? 0 : targetValue) : minimums.front().second;
    double upper=maximums.empty() ? lower : maximums.front().second;
    const double padding=std::max(0.00025,(upper-lower)*0.12);
    xAxis->setRange(left,right);
    yAxis->setRange(lower-padding,upper+padding);
}
void TracePlot::restoreView() {
    autoFollow=true;hideHover();fitLatest();replot();
}
void TracePlot::hideHover() {
    hovering=false;hoverLabel->hide();marker->setVisible(false);cursorLine->setVisible(false);
}
void TracePlot::updateHover(QPoint pixel) {
    const auto data=graph(0)->data();
    if(!axisRect()->rect().contains(pixel) || data->isEmpty()) {hideHover();return;}
    const double key=xAxis->pixelToCoord(pixel.x());
    auto it=data->findBegin(key,false);
    if(it==data->constEnd()) --it;
    else if(it!=data->constBegin() && std::abs((it-1)->key-key)<std::abs(it->key-key)) --it;
    // Only show retained samples visible in the current time range.
    if(!xAxis->range().contains(it->key)) {hideHover();return;}
    hovering=true;hoverPixel=pixel;
    marker->setGraphKey(it->key);marker->updatePosition();marker->setVisible(true);
    cursorLine->point1->setCoords(it->key,0);cursorLine->point2->setCoords(it->key,1);cursorLine->setVisible(true);
    if(currentMode) {
        const auto measured=graph(1)->data()->findBegin(it->key,false);
        const QString measuredText=measured!=graph(1)->data()->constEnd() && measured->key==it->key
            ? QString::number(measured->value,'f',6) : "--";
        hoverLabel->setText(QString("1秒平均值\n时间窗：[%1, %2) s\n输出电流：%3 mA\n实测电流：%4 mA")
            .arg(it->key-1,0,'f',0).arg(it->key,0,'f',0).arg(it->value,0,'f',6).arg(measuredText));
    } else {
        hoverLabel->setText(QString("1秒平均值\n时间窗：[%1, %2) s\n探头电压：%3 V\n目标电压（窗末时刻）：%4 V\n电压误差（目标−均值）：%5 V")
            .arg(it->key-1,0,'f',0).arg(it->key,0,'f',0).arg(it->value,0,'f',6).arg(targetAt(it->key),0,'f',6).arg(targetAt(it->key)-it->value,0,'f',6));
    }
    hoverLabel->adjustSize();
    int x=pixel.x()+15,y=pixel.y()+15;
    if(x+hoverLabel->width()>width()-5)x=pixel.x()-hoverLabel->width()-15;
    if(y+hoverLabel->height()>height()-5)y=pixel.y()-hoverLabel->height()-15;
    hoverLabel->move(std::max(5,x),std::max(5,y));hoverLabel->show();hoverLabel->raise();
}
void TracePlot::mousePressEvent(QMouseEvent* event) {
    if(event->button()==Qt::LeftButton && axisRect()->rect().contains(event->pos())){autoFollow=false;hideHover();}
    QCustomPlot::mousePressEvent(event);
}
void TracePlot::mouseMoveEvent(QMouseEvent* event) {
    QCustomPlot::mouseMoveEvent(event);
    if(event->buttons()==Qt::NoButton)updateHover(event->pos());else hideHover();
    replot(QCustomPlot::rpQueuedReplot);
}
void TracePlot::wheelEvent(QWheelEvent* event) {
    if(axisRect()->rect().contains(event->position().toPoint())){autoFollow=false;hideHover();}
    QCustomPlot::wheelEvent(event);
}
void TracePlot::mouseDoubleClickEvent(QMouseEvent* event) {
    QCustomPlot::mouseDoubleClickEvent(event);
    if(event->button()==Qt::LeftButton)restoreView();
}
void TracePlot::leaveEvent(QEvent* event) {
    hideHover();replot(QCustomPlot::rpQueuedReplot);QCustomPlot::leaveEvent(event);
}

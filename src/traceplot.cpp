#include "traceplot.h"
#include <QTimer>
#include <QApplication>
#include <algorithm>
#include <cmath>

TracePlot::TracePlot(QWidget* parent) : QCustomPlot(parent) {
    setMinimumHeight(150);
    // Button-drag events are still delivered without mouse tracking. Avoid
    // processing every passive mouse move over a large, dense plot.
    setMouseTracking(false);
    setBackground(Qt::white);
    setPlottingHint(QCP::phCacheLabels,true);
    setPlottingHint(QCP::phFastPolylines,true);
    setNoAntialiasingOnDrag(true);
    for(auto* axis:{xAxis,yAxis}) {
        axis->setBasePen(QPen(QColor("#cedbe6")));
        axis->setTickPen(QPen(QColor("#cedbe6")));
        axis->setSubTicks(false);
        axis->setTickLabelColor(QColor("#64798c"));axis->setLabelColor(QColor("#496276"));
        axis->setTickLabelFont(QFont("Microsoft YaHei UI",9));axis->setLabelFont(QFont("Microsoft YaHei UI",9));
        axis->grid()->setPen(QPen(QColor("#e6edf3"),1,Qt::DotLine));axis->grid()->setZeroLinePen(QPen(QColor("#d3dfe9")));
    }
    setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    // Live time is authoritative: dragging may adjust Y, but must never pan X
    // away from the newest sample at the right edge.
    axisRect()->setRangeDrag(Qt::Vertical);
    axisRect()->setRangeZoom(Qt::Horizontal | Qt::Vertical);
    axisRect()->setRangeZoomFactor(0.85);
    auto timeTicker=QSharedPointer<QCPAxisTickerTime>::create();
    timeTicker->setTimeFormat("%h:%m:%s");
    timeTicker->setTickCount(5);
    xAxis->setTicker(timeTicker);
    xAxis->setLabel("时间（时:分:秒）");
    connect(this,&QCustomPlot::beforeReplot,this,
            [this,timeTicker,lastMilliseconds=false,lastTickCount=-1,lastRightMargin=-1]() mutable {
        const bool milliseconds=xAxis->range().size()<5;
        if(milliseconds!=lastMilliseconds) {
            timeTicker->setTimeFormat(milliseconds ? "%h:%m:%s.%z" : "%h:%m:%s");
            xAxis->setLabel(milliseconds ? "时间（时:分:秒.毫秒）" : "时间（时:分:秒）");
            lastMilliseconds=milliseconds;
        }
        const int labelWidth=QFontMetrics(xAxis->tickLabelFont()).horizontalAdvance(
            milliseconds ? "00:00:00.000" : "00:00:00");
        const int tickCount=qBound(2,(width()-100)/(labelWidth+28),6);
        if(tickCount!=lastTickCount) {timeTicker->setTickCount(tickCount);lastTickCount=tickCount;}
        const int rightMargin=labelWidth/2+8;
        if(rightMargin!=lastRightMargin) {
            axisRect()->setMinimumMargins(QMargins(0,0,rightMargin,0));
            lastRightMargin=rightMargin;
        }
    });
    yAxis->setLabel("电压（V）");
    addGraph(); graph(0)->setName("探头电压"); graph(0)->setPen(QPen(QColor("#098c95"),1.2));
    addGraph(); graph(1)->setName("目标电压"); graph(1)->setPen(QPen(QColor("#d58a28"),1.2,Qt::DashLine));
    graph(1)->setLineStyle(QCPGraph::lsStepLeft);
    // Every retained one-second mean must remain a real polyline vertex.
    // Performance is handled by coalescing repaints, not by dropping points.
    for(auto* curve : {graph(0),graph(1)}) {
        curve->setAdaptiveSampling(false);
        curve->setAntialiased(false);
    }
    liveData[0]=graph(0)->data();
    liveData[1]=graph(1)->data();
    // Store each completed one-second average and coalesce screen paints.
    redrawTimer=new QTimer(this);
    redrawTimer->setSingleShot(true);
    // Voltage and current means complete together. Stagger their paints so
    // two dense widgets never block the GUI thread back-to-back.
    redrawTimer->setInterval(120);
    connect(redrawTimer,&QTimer::timeout,this,[this]{
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
    resetData(targetValue);
}
void TracePlot::setCurrentMode(bool enabled) {
    currentMode=enabled;
    redrawTimer->setInterval(enabled ? 620 : 120);
    yAxis->setLabel(enabled ? "电流（mA）" : "电压（V）");
    graph(0)->setName(enabled ? "输出电流" : "探头电压");
    graph(1)->setName(enabled ? "实测电流" : "目标电压");
    graph(0)->setPen(QPen(QColor("#098c95"),1.2));
    graph(1)->setPen(QPen(QColor("#d58a28"),1.2,enabled ? Qt::SolidLine : Qt::DashLine));
    graph(1)->setLineStyle(enabled ? QCPGraph::lsLine : QCPGraph::lsStepLeft);
    resetData(enabled ? 0 : targetValue);
}
void TracePlot::resetData(double target) {
    graph(0)->setData(liveData[0]);
    graph(1)->setData(liveData[1]);
    targetValue=target;
    targetHistory.clear();
    averaging={};
    lastSampleTime=-std::numeric_limits<double>::infinity();
    minimums.clear();maximums.clear();redrawTimer->stop();
    liveData[0]->clear();
    liveData[1]->clear();
    autoFollow=true;
    manualYRange=false;
    followWindowSeconds=0;
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
        liveData[0]->add(QCPGraphData(end,average));
        if(currentMode) liveData[1]->add(QCPGraphData(end,other));
        // Voltage scaling follows measured feedback; a distant target must
        // not flatten its small fluctuations. Current mode fits both traces.
        finishSample(end,currentMode ? std::min(average,other) : average,
                     currentMode ? std::max(average,other) : average);
    }
    lastSampleTime=seconds;
}
void TracePlot::finishSample(double seconds,double lower,double upper) {
    const auto data=liveData[0];
    const double cutoff=seconds-historySeconds;
    data->removeBefore(cutoff);
    if(currentMode) liveData[1]->removeBefore(cutoff);
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
double TracePlot::displayedTargetAt(double seconds) const {
    return targetAt(seconds);
}
void TracePlot::updateTargetLine() {
    if(currentMode) return;
    const auto range=xAxis->range();
    QVector<double> times{range.lower},values{displayedTargetAt(range.lower)};
    for(auto it=targetHistory.upperBound(range.lower);it!=targetHistory.constEnd() && it.key()<range.upper;++it) {
        times.append(it.key());values.append(it.value());
    }
    times.append(range.upper);values.append(displayedTargetAt(range.upper));
    graph(1)->setData(times,values,true);
}
void TracePlot::fitLatest() {
    const auto data=liveData[0];
    double left=0,right=10;
    if (!data->isEmpty()) {
        const double first=data->constBegin()->key;
        const double latest=(data->constEnd()-1)->key;
        if(followWindowSeconds>0) {
            right=latest;
            left=right-followWindowSeconds;
        } else {
            right=std::max(first+10.0,latest);
            left=std::max(first,right-historySeconds);
        }
    }
    double lower=minimums.empty() ? (currentMode ? 0 : targetValue) : minimums.front().second;
    double upper=maximums.empty() ? lower : maximums.front().second;
    // Keep small current/voltage variations visible. Constant signals still
    // need a nonzero range for the axis, but never a fixed 0.1-unit margin.
    const double padding=std::max({(upper-lower)*0.12,
        std::max(std::abs(lower),std::abs(upper))*1e-6,1e-9});
    xAxis->setRange(left,right);
    if(manualYRange) yAxis->setRange(lockedYRange);
    else yAxis->setRange(lower-padding,upper+padding);
}
void TracePlot::restoreView() {
    manualYRange=false;
    followWindowSeconds=0;
    graph(0)->setData(liveData[0]);
    graph(1)->setData(liveData[1]);
    autoFollow=true;
    fitLatest();updateTargetLine();replot();
}
void TracePlot::mousePressEvent(QMouseEvent* event) {
    if(event->button()==Qt::LeftButton && axisRect()->rect().contains(event->pos())) {
        dragCandidate=true;
        dragInProgress=false;
        dragStartPixel=event->pos();
    }
    QCustomPlot::mousePressEvent(event);
}
void TracePlot::mouseReleaseEvent(QMouseEvent* event) {
    const bool completedDrag=event->button()==Qt::LeftButton && dragInProgress;
    QCustomPlot::mouseReleaseEvent(event);
    if(event->button()==Qt::LeftButton) {dragCandidate=false;dragInProgress=false;}
    if(completedDrag) {
        // Preserve the selected time span and locked Y range, and explicitly
        // reaffirm the newest sample at the right edge after Y dragging.
        followWindowSeconds=qBound(0.01,xAxis->range().size(),historySeconds);
        autoFollow=true;
        fitLatest();updateTargetLine();replot(QCustomPlot::rpQueuedReplot);
    }
}
void TracePlot::mouseMoveEvent(QMouseEvent* event) {
    const bool manualDrag=event->buttons().testFlag(Qt::LeftButton) && dragCandidate
        && (event->pos()-dragStartPixel).manhattanLength()>=QApplication::startDragDistance();
    if(manualDrag) {
        dragInProgress=true;
        manualYRange=true;
    }
    QCustomPlot::mouseMoveEvent(event);
    if(manualDrag) lockedYRange=yAxis->range();
}
void TracePlot::wheelEvent(QWheelEvent* event) {
    const bool inside=axisRect()->rect().contains(event->position().toPoint());
    const bool wasFollowing=autoFollow;
    if(inside) {
        manualYRange=true;
    }
    QCustomPlot::wheelEvent(event);
    if(inside) lockedYRange=yAxis->range();
    if(inside && wasFollowing) {
        // Preserve the zoom level, but keep the newest point visible. Dragging
        // still pauses range following for deliberate historical inspection.
        followWindowSeconds=qBound(0.01,xAxis->range().size(),historySeconds);
        fitLatest();
        updateTargetLine();
        replot(QCustomPlot::rpQueuedReplot);
    }
}
void TracePlot::mouseDoubleClickEvent(QMouseEvent* event) {
    QCustomPlot::mouseDoubleClickEvent(event);
    if(event->button()==Qt::LeftButton && axisRect()->rect().contains(event->pos())) restoreView();
}

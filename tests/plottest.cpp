#include "traceplot.h"
#include <QApplication>
#include <QFile>
#include <QDir>
#include <QTextStream>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <cmath>
#include <stdexcept>

int plotTest() {
    QStringList results; int result=0;
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
    auto require=[&](bool ok,const QString& text){if(!ok)throw std::runtime_error(text.toStdString());results<<"PASS "+text;};
    auto approximatelyEqual=[](double a,double b){return std::abs(a-b)<1e-9;};
    auto mouse=[](QWidget& widget,QEvent::Type type,QPoint pos,Qt::MouseButton button,Qt::MouseButtons buttons){
        QMouseEvent event(type,QPointF(pos),QPointF(widget.mapToGlobal(pos)),button,buttons,Qt::NoModifier);
        QApplication::sendEvent(&widget,&event);
    };
    try {
        TracePlot plot;plot.resize(900,440);plot.show();QApplication::processEvents();
        require(!plot.graph(0)->adaptiveSampling() && !plot.graph(1)->adaptiveSampling(),
            "Every retained mean is drawn as a polyline vertex without adaptive sampling");
        require(!plot.hasMouseTracking() && !plot.findChild<QWidget*>("plotHoverData"),
            "Passive mouse tracking and hover overlays are disabled");
        plot.resetData(2);
        // Acquisition begins partway through a second, and intervals are irregular.
        plot.appendSample(.3,1,2);plot.appendSample(.5,2,2);plot.appendSample(.999,6,2);
        require(plot.graph(0)->data()->isEmpty(),"An unfinished second does not create a plotted point");
        plot.appendSample(.9995,std::numeric_limits<double>::quiet_NaN(),2);
        plot.appendSample(.4,99,2); // stale sample
        plot.appendSample(1,10,2);
        require(plot.graph(0)->data()->size()==1 && approximatelyEqual(plot.graph(0)->data()->constBegin()->value,3)
            && plot.graph(0)->data()->constBegin()->key==1,
            "First integer-second window averages its three valid samples and excludes the sample exactly at the next boundary");
        plot.appendSample(1.8,14,2);plot.appendSample(2.2,20,2);
        require(plot.graph(0)->data()->size()==2 && approximatelyEqual((plot.graph(0)->data()->constEnd()-1)->value,12)
            && (plot.graph(0)->data()->constEnd()-1)->key==2,
            "The next second averages two samples independently of the previous sample count");
        plot.appendSample(4.1,40,2);plot.appendSample(5,50,2);
        require(plot.graph(0)->data()->size()==4 && (plot.graph(0)->data()->constBegin()+2)->key==3
            && (plot.graph(0)->data()->constEnd()-1)->key==5 && (plot.graph(0)->data()->constEnd()-1)->value==40,
            "Single-sample windows are valid and an empty second is not fabricated or merged with a later window");
        plot.replot();plot.grab().save(QDir(output).filePath("plot-average.png"));

        plot.clearData();plot.appendSample(5.3,1.9,2);plot.appendSample(6,1.8,2);
        require(plot.graph(0)->data()->size()==1 && approximatelyEqual(plot.graph(0)->data()->constBegin()->value,25.95),
            "Clear preserves the current second's samples so the next plotted mean still matches a 1 Hz file");
        plot.resetData(2);
        for(int i=0;i<=100;++i)plot.appendSample(i/10.0,2*(1-std::exp(-i/20.0)),2);
        require(plot.graph(0)->data()->size()==10,"100 ms acquisition produces ten completed second averages in ten seconds");
        plot.replot();
        const QPoint center=plot.axisRect()->rect().center();
        mouse(plot,QEvent::MouseButtonPress,center,Qt::LeftButton,Qt::LeftButton);
        mouse(plot,QEvent::MouseButtonRelease,center,Qt::LeftButton,Qt::NoButton);
        require(plot.followsLatest(),"A normal click does not pause dynamic display");
        const auto beforeZoom=plot.xAxis->range();
        const auto beforeZoomY=plot.yAxis->range();
        QWheelEvent wheel(QPointF(center),QPointF(plot.mapToGlobal(center)),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(&plot,&wheel);
        const double zoomedSpan=plot.xAxis->range().size();
        const auto zoomedYRange=plot.yAxis->range();
        require(zoomedSpan<beforeZoom.size() && zoomedYRange.size()<beforeZoomY.size() && plot.followsLatest(),
            "Wheel zoom preserves live X following and records the selected Y range");
        const int zoomedPointCount=plot.graph(0)->data()->size();
        plot.appendSample(100,20,2);plot.appendSample(101,2,2);QApplication::processEvents();
        require(plot.followsLatest() && plot.graph(0)->data()->size()>zoomedPointCount
            && approximatelyEqual(plot.xAxis->range().size(),zoomedSpan) && plot.xAxis->range().upper==101
            && plot.yAxis->range()==zoomedYRange,
            "A zoomed plot follows the latest X point while preserving the exact manual Y range despite an outside value");
        const auto beforeDrag=plot.xAxis->range();
        const auto beforeDragY=plot.yAxis->range();
        mouse(plot,QEvent::MouseButtonPress,center,Qt::LeftButton,Qt::LeftButton);
        mouse(plot,QEvent::MouseMove,center+QPoint(70,25),Qt::NoButton,Qt::LeftButton);
        require(plot.xAxis->range()==beforeDrag,
            "Dragging never moves X away from the newest sample, even before mouse release");
        mouse(plot,QEvent::MouseButtonRelease,center+QPoint(70,25),Qt::LeftButton,Qt::NoButton);
        const auto draggedYRange=plot.yAxis->range();
        require(plot.followsLatest() && plot.xAxis->range().upper==101
            && approximatelyEqual(plot.xAxis->range().size(),beforeDrag.size()) && draggedYRange!=beforeDragY,
            "Dragging adjusts Y while X remains on the latest sample with the selected time span");
        const int draggedPointCount=plot.graph(0)->data()->size();
        plot.appendSample(102,2,2);plot.appendSample(103,2,2);QApplication::processEvents();
        require(plot.xAxis->range().upper==103 && approximatelyEqual(plot.xAxis->range().size(),beforeDrag.size())
            && plot.yAxis->range()==draggedYRange && plot.graph(0)->data()->size()>draggedPointCount,
            "After dragging, new data advances X to the latest time without changing the locked Y range");
        mouse(plot,QEvent::MouseButtonDblClick,center,Qt::LeftButton,Qt::LeftButton);
        mouse(plot,QEvent::MouseButtonRelease,center,Qt::LeftButton,Qt::NoButton);
        require(plot.followsLatest() && plot.xAxis->range().upper==103 && plot.yAxis->range().contains(2)
            && plot.yAxis->range().contains(20)
            && plot.graph(0)->data()->size()>draggedPointCount,
            "Double click unlocks Y autoscaling and restores dynamic display with every retained background point");
        plot.resetData(-1);
        for(int i=0;i<=31010;++i)plot.appendSample(i/10.0,-1,-1);
        require(plot.graph(0)->data()->size()==3101 && plot.xAxis->range().lower==1,
            "Average history remains visible beyond 3000 points and 60 seconds");
        plot.replot();

        plot.resetData(4);
        for(int i=0;i<=100;++i) plot.appendSample(i/10.0,i<55 ? 3.9 : 5.9,i<55 ? 4 : 6);
        plot.replot();const auto changes=plot.graph(1)->data();
        require(plot.graph(0)->data()->size()==10 && changes->size()==3
            && changes->constBegin()->value==4 && approximatelyEqual((changes->constBegin()+1)->key,5.5)
            && (changes->constEnd()-1)->value==6 && plot.graph(1)->lineStyle()==QCPGraph::lsStepLeft,
            "Target remains an unsmoothed step at its true application time inside a second");
        plot.xAxis->setRange(0,4);
        require(plot.graph(1)->data()->constBegin()->value==4 && (plot.graph(1)->data()->constEnd()-1)->value==4,"Panning to old history keeps the old target");
        plot.restoreView();plot.replot();plot.grab().save(QDir(output).filePath("plot-target-change.png"));
        plot.clearData();
        require(plot.graph(0)->data()->isEmpty() && plot.graph(1)->data()->constBegin()->value==6,"Clear keeps the latest applied target");
        plot.appendSample(11,100,6);plot.setCurrentMode(true);plot.resetData(999);
        plot.appendCurrentSample(.2,-1,.1);plot.appendCurrentSample(.7,-2,.3);plot.appendCurrentSample(1,-9,3);
        plot.replot();
        require(plot.graphCount()==2 && plot.legend->itemCount()==2 && plot.graph(0)->name()=="输出电流"
            && plot.graph(1)->name()=="实测电流" && plot.graph(1)->lineStyle()==QCPGraph::lsLine,
            "Current plot retains the two unambiguous curve names");
        require(approximatelyEqual(plot.graph(0)->data()->constBegin()->value,-1.5) && approximatelyEqual(plot.graph(1)->data()->constBegin()->value,.2)
            && plot.graph(0)->data()->constBegin()->key==plot.graph(1)->data()->constBegin()->key,
            "Output and measured currents average separately over the same second and sample count");
        require(plot.yAxis->range().contains(-1.5) && plot.yAxis->range().contains(.2) && plot.yAxis->range().upper<1,
            "Current autoscaling includes both means and ignores a voltage target");
        plot.replot();plot.grab().save(QDir(output).filePath("plot-current.png"));
        plot.clearData();plot.appendCurrentSample(1.1,-.1,-.09);plot.appendCurrentSample(2,0,0);
        require(plot.graph(0)->data()->size()==1 && approximatelyEqual(plot.graph(0)->data()->constBegin()->value,-4.55)
            && approximatelyEqual(plot.graph(1)->data()->constBegin()->value,1.455),"Current clear preserves the shared second window for both curves");
        plot.setCurrentMode(false);plot.resetData(2);
        require(plot.graph(0)->data()->isEmpty() && plot.graph(1)->data()->constBegin()->value==2,"Voltage mode resets the averaging state");

        // Exercise a full day at 100 ms plus the next boundary, without physical I/O or wall-clock waits.
        TracePlot dayCurrent;dayCurrent.setCurrentMode(true);dayCurrent.resize(900,440);
        plot.resetData(2);QElapsedTimer loadClock;loadClock.start();
        for(int i=0;i<=864010;++i) {
            const double seconds=i/10.0;
            const double voltage=i==20 ? 5 : i<30 ? 2 : 2+.05*std::sin(seconds/1800)+.001*std::sin(seconds*6);
            plot.appendSample(seconds,voltage,seconds<43200 ? 2 : 2.02);
            dayCurrent.appendCurrentSample(seconds,.2+.02*std::sin(seconds/3600),i==20 ? -4 : i<30 ? .201 : .201+.02*std::sin(seconds/3600));
        }
        results<<QString("INFO 24-hour voltage/current averaging and append: %1 ms").arg(loadClock.elapsed());
        require(plot.graph(0)->data()->size()==86401 && plot.graph(0)->data()->constBegin()->key==1
            && (plot.graph(0)->data()->constEnd()-1)->key==86401,"24-hour history retains 86401 second means across inclusive endpoints");
        require(dayCurrent.graph(0)->data()->size()==86401 && dayCurrent.graph(1)->data()->size()==86401,
            "Both current curves retain 24 hours of second means");
        require(plot.xAxis->range().size()==86400 && plot.yAxis->range().contains(2.3) && dayCurrent.yAxis->range().contains(-.2191),
            "Full-day automatic view includes the averaged early extrema");
        auto ticker=qSharedPointerDynamicCast<QCPAxisTickerTime>(plot.xAxis->ticker());
        QVector<double> ticks;QVector<QString> labels;ticker->generate(QCPRange(0,86400),QLocale::c(),'g',6,ticks,nullptr,&labels);
        require(labels.contains("24:00:00"),"Time axis reaches 24:00:00 without wrapping");
        QElapsedTimer renderClock;renderClock.start();plot.replot();dayCurrent.replot();
        results<<QString("INFO Full-day voltage/current repaint: %1 ms").arg(renderClock.elapsed());
        plot.xAxis->setRange(2.5,3.5);plot.yAxis->setRange(1.8,2.5);plot.replot();
        const auto earlyMean=plot.graph(0)->data()->findBegin(3,false);
        require(earlyMean!=plot.graph(0)->data()->constEnd() && earlyMean->key==3
            && approximatelyEqual(earlyMean->value,2.3),
            "Zoomed history retains the exact earliest-day mean and window endpoint");
        plot.restoreView();const auto entireRange=plot.xAxis->range();
        QWheelEvent dayWheel(QPointF(plot.axisRect()->rect().center()),QPointF(plot.mapToGlobal(plot.axisRect()->rect().center())),
            QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        QApplication::sendEvent(&plot,&dayWheel);
        const double dayZoomSpan=plot.xAxis->range().size();
        require(plot.followsLatest() && dayZoomSpan<entireRange.size(),
            "Full-day history remains zoomable while following live data");
        for(int i=864011;i<=864050;++i) {plot.appendSample(i/10.0,2,2.02);dayCurrent.appendCurrentSample(i/10.0,.2,.201);}
        require(approximatelyEqual(plot.xAxis->range().size(),dayZoomSpan) && plot.xAxis->range().upper==86405,
            "Rolling expiry preserves the zoom span and keeps the newest mean visible");
        plot.restoreView();dayCurrent.restoreView();
        require(plot.graph(0)->data()->size()==86401 && plot.graph(0)->data()->constBegin()->key==5
            && dayCurrent.graph(0)->data()->constBegin()->key==5 && dayCurrent.graph(1)->data()->constBegin()->key==5,
            "Only means older than 24 hours expire from all curves");
        require(plot.xAxis->range().size()==86400 && plot.yAxis->range().upper<2.2 && dayCurrent.yAxis->range().lower>0,
            "Expired extrema leave automatic ranges while the latest 24 hours stays visible");
        require(plot.graph(1)->data()->constBegin()->value==2 && (plot.graph(1)->data()->constEnd()-1)->value==2.02,
            "Target changes survive the rolling day boundary");
        plot.replot();plot.grab().save(QDir(output).filePath("plot-24-hours.png"));
        dayCurrent.replot();dayCurrent.grab().save(QDir(output).filePath("plot-current-24-hours.png"));

        // Keep both production plot widgets live with a complete 24-hour history.
        // A 10 ms heartbeat measures real GUI-thread stalls while 10 Hz samples,
        // delayed repaint timers, zooming, dragging, restoring and resizing all run.
        TracePlot stressVoltage;TracePlot stressCurrent;stressCurrent.setCurrentMode(true);
        for(int i=0;i<=864010;++i) {
            const double seconds=i/10.0;
            stressVoltage.appendSample(seconds,2+.02*std::sin(seconds*6),2.02);
            stressCurrent.appendCurrentSample(seconds,.2+.02*std::sin(seconds/3600),.201+.02*std::sin(seconds/3600));
        }
        stressVoltage.resize(1520,360);stressCurrent.resize(1520,360);
        stressVoltage.show();stressCurrent.show();
        QApplication::processEvents();
        int voltageFrames=0,currentFrames=0,heartbeatCount=0,gapsOver50Ms=0;
        qint64 maxHeartbeatGapMs=0,lastHeartbeatMs=0;
        QObject::connect(&stressVoltage,&QCustomPlot::afterReplot,&stressVoltage,[&]{++voltageFrames;});
        QObject::connect(&stressCurrent,&QCustomPlot::afterReplot,&stressCurrent,[&]{++currentFrames;});
        QElapsedTimer heartbeatClock;heartbeatClock.start();
        QTimer heartbeat;heartbeat.setTimerType(Qt::PreciseTimer);heartbeat.setInterval(10);
        QObject::connect(&heartbeat,&QTimer::timeout,&stressVoltage,[&]{
            const qint64 now=heartbeatClock.elapsed();
            if(lastHeartbeatMs) {
                const qint64 gap=now-lastHeartbeatMs;
                maxHeartbeatGapMs=std::max(maxHeartbeatGapMs,gap);
                if(gap>50)++gapsOver50Ms;
            }
            lastHeartbeatMs=now;++heartbeatCount;
        });
        int liveTick=864010;
        QTimer input;input.setTimerType(Qt::PreciseTimer);input.setInterval(100);
        QObject::connect(&input,&QTimer::timeout,&stressVoltage,[&]{
            const double seconds=++liveTick/10.0;
            stressVoltage.appendSample(seconds,2+.02*std::sin(seconds*6),2.02);
            stressCurrent.appendCurrentSample(seconds,.2+.02*std::sin(seconds/3600),.201+.02*std::sin(seconds/3600));
        });
        QTimer::singleShot(1500,&stressVoltage,[&]{
            const QPoint p=stressVoltage.axisRect()->rect().center();
            QWheelEvent event(QPointF(p),QPointF(stressVoltage.mapToGlobal(p)),QPoint(),QPoint(0,120),
                Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
            QApplication::sendEvent(&stressVoltage,&event);
        });
        QTimer::singleShot(3300,&stressVoltage,[&]{
            const QPoint p=stressVoltage.axisRect()->rect().center();
            mouse(stressVoltage,QEvent::MouseButtonPress,p,Qt::LeftButton,Qt::LeftButton);
            mouse(stressVoltage,QEvent::MouseMove,p+QPoint(90,20),Qt::NoButton,Qt::LeftButton);
            mouse(stressVoltage,QEvent::MouseButtonRelease,p+QPoint(90,20),Qt::LeftButton,Qt::NoButton);
        });
        QTimer::singleShot(5100,&stressVoltage,[&]{
            const QPoint p=stressVoltage.axisRect()->rect().center();
            mouse(stressVoltage,QEvent::MouseButtonDblClick,p,Qt::LeftButton,Qt::LeftButton);
            mouse(stressVoltage,QEvent::MouseButtonRelease,p,Qt::LeftButton,Qt::NoButton);
        });
        QTimer::singleShot(6500,&stressVoltage,[&]{stressVoltage.resize(1180,330);stressCurrent.resize(1180,330);});
        QTimer::singleShot(7100,&stressVoltage,[&]{stressVoltage.resize(1520,360);stressCurrent.resize(1520,360);});
        QEventLoop liveStress;
        QTimer::singleShot(8000,&liveStress,&QEventLoop::quit);
        heartbeat.start();input.start();liveStress.exec();input.stop();heartbeat.stop();
        QApplication::processEvents();
        results<<QString("INFO 24-hour live GUI stress: max heartbeat gap %1 ms; gaps >50 ms %2; "
                         "heartbeats %3; voltage/current repaints %4/%5")
                     .arg(maxHeartbeatGapMs).arg(gapsOver50Ms).arg(heartbeatCount)
                     .arg(voltageFrames).arg(currentFrames);
        require(heartbeatCount>=500 && maxHeartbeatGapMs<150,
            "A full-day two-plot live run remains responsive through zoom, drag, restore and resize");
        require(stressVoltage.followsLatest() && stressVoltage.xAxis->range().upper>=86408,
            "The stress-test plot resumes following the newest data after historical inspection");

        plot.clearData();dayCurrent.clearData();
        plot.appendSample(86405.5,1.9,2.02);plot.appendSample(86406,1.8,2.02);
        dayCurrent.appendCurrentSample(86405.5,-.1,-.09);dayCurrent.appendCurrentSample(86406,0,0);
        require(plot.graph(0)->data()->size()==1 && dayCurrent.graph(0)->data()->size()==1
            && plot.xAxis->range().size()==10 && approximatelyEqual(plot.graph(0)->data()->constBegin()->value,1.95)
            && approximatelyEqual(dayCurrent.graph(0)->data()->constBegin()->value,.05),
            "Clear resets full-day view caches while preserving the ongoing second for recorder alignment");
    }catch(const std::exception& e){results<<"FAIL "+QString::fromUtf8(e.what());result=1;}
    QFile file(QDir(output).filePath("plot-test.txt"));
    if(file.open(QIODevice::WriteOnly)){QTextStream stream(&file);stream.setCodec("UTF-8");stream<<results.join('\n');}
    return result;
}

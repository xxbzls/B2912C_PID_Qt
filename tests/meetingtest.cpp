#include "window.h"
#include "datarecorder.h"
#include "traceplot.h"
#include <QtWidgets>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
void waitUntil(const std::function<bool()>& done,int timeout=8000) {
    QElapsedTimer timer;timer.start();
    while(!done()) {
        if(timer.elapsed()>timeout) throw std::runtime_error("Meeting test timed out");
        QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);
    }
}
struct Recording {QStringList paths;QString error;quint64 rows=0;bool ready=false;};
Recording record(const RecordSettings& settings,const QVector<RecordSample>& samples) {
    DataRecorder writer(settings);QObject receiver;Recording result;bool finished=false;
    QObject::connect(&writer,&DataRecorder::fileChanged,&receiver,[&](QString path){result.paths<<path;});
    QObject::connect(&writer,&DataRecorder::failed,&receiver,[&](QString message){result.error=message;});
    QObject::connect(&writer,&DataRecorder::saved,&receiver,[&](quint64 rows){result.rows=rows;});
    QObject::connect(&writer,&DataRecorder::ready,&receiver,[&]{
        result.ready=true;for(const auto& sample:samples)writer.submit(sample);writer.finish();
    });
    QObject::connect(&writer,&QThread::finished,&receiver,[&]{finished=true;});
    writer.start();waitUntil([&]{return finished;});writer.wait();
    // The startup preflight opens the current wall-clock hour; synthetic samples
    // may belong to other dates. Count only the files that actually received rows.
    result.paths.removeDuplicates();
    for(auto it=result.paths.begin();it!=result.paths.end();)
        if(QFileInfo(*it).size()==0)it=result.paths.erase(it);else ++it;
    return result;
}
QStringList lines(const QString& path) {
    QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Cannot read recording");
    QByteArray bytes=file.readAll();
    if(bytes.startsWith(QByteArray::fromHex("efbbbf")))throw std::runtime_error("Unexpected BOM before first timestamp");
    const QStringList rows=QString::fromUtf8(bytes).split("\r\n",Qt::SkipEmptyParts);
    for(const auto& row:rows) {
        const auto columns=row.split(' ');
        if(columns.size()!=6 || row.contains(',') || row.contains('\t')
            || !QDateTime::fromString(columns[0]+" "+columns[1],"yy/MM/dd HH:mm:ss.zzz").isValid())
            throw std::runtime_error("TXT must contain date, time and four values separated by single spaces, without a header");
    }
    return rows;
}
QStringList recordedRows(const QString& directory) {
    QStringList rows,paths;QDirIterator iterator(directory,{"*.txt"},QDir::Files,QDirIterator::Subdirectories);
    while(iterator.hasNext()) paths.append(iterator.next());
    paths.sort();for(const auto& path:paths)rows.append(lines(path));
    return rows;
}
}

int meetingTest() {
    QStringList checks;int result=0;
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
    auto require=[&](bool ok,const QString& message){if(!ok)throw std::runtime_error(message.toStdString());checks<<"PASS "+message;};
    try {
        QTemporaryDir temporary;require(temporary.isValid(),"Temporary data directory created");
        const qint64 epoch=QDateTime::fromString("2026-10-13T15:59:59.123Z",Qt::ISODateWithMs).toMSecsSinceEpoch();
        require(beijingTime(epoch)=="2026-10-13 23:59:59.123" && beijingTime(epoch+1000)=="2026-10-14 00:00:00.123",
            "Beijing time converts UTC+8 and crosses midnight correctly");
        QVector<RecordSample> samples;
        for(int i=0;i<=2000;++i)samples.append({epoch+i,i/1000.,-1.25+i*.001,i<1500 ? 2.5 : 3,-.8+i*.0001,-.75+i*.0002,true});
        for(int frequency:{1,10}) {
            RecordSettings settings;settings.directory=temporary.path()+QString("/rate-%1").arg(frequency);settings.frequencyHz=frequency;
            const auto run=record(settings,samples);
            const int expected=frequency==1 ? 2 : samples.size();
            require(run.ready && run.error.isEmpty() && run.paths.size()==(frequency==1 ? 1 : 2) && run.rows==quint64(expected),
                frequency==1 ? "1 Hz saves only two completed second means and excludes the unfinished tail"
                             : "10 Hz saves every original sample without averaging or rate-based discarding");
            const auto rows=recordedRows(settings.directory);
            require(rows.size()==expected
                && rows[0]==(frequency==1 ? "26/10/14 00:00:00.123 -0.750500000 2.500000000 -0.750050000 -0.650100000"
                                         : "26/10/13 23:59:59.123 -1.250000000 2.500000000 -0.800000000 -0.750000000"),
                "TXT starts directly with timestamp and four space-separated values, preserves nine decimals and drains on stop");
            bool cadenceMatches=true;
            for(int row=0;row<rows.size();++row)
                cadenceMatches=cadenceMatches && rows[row].section(' ',0,1)==beijingTime(epoch+(frequency==1 ? (row+1)*1000 : row),"yy/MM/dd HH:mm:ss.zzz");
            require(cadenceMatches,frequency==1 ? "1 Hz timestamps match the curve's integer-second window ends across midnight"
                                               : "10 Hz preserves each actual raw timestamp, including faster-than-100-ms samples");
            if(frequency==1) {
                TracePlot probe,current;probe.resetData(2.5);current.setCurrentMode(true);
                for(int i=0;i<samples.size();++i) {
                    const auto& sample=samples[i];
                    probe.appendSample(sample.seconds,sample.probeV,sample.targetV);
                    current.appendCurrentSample(sample.seconds,sample.outputMA,sample.measuredMA);
                    if(i==500){probe.clearData();current.clearData();}
                }
                bool aligned=probe.graph(0)->data()->size()==rows.size() && current.graph(0)->data()->size()==rows.size();
                for(int row=0;row<rows.size();++row) {
                    const auto values=rows[row].split(' ');
                    aligned=aligned && (probe.graph(0)->data()->constBegin()+row)->key==row+1
                        && std::abs(values[2].toDouble()-(probe.graph(0)->data()->constBegin()+row)->value)<1e-9
                        && std::abs(values[4].toDouble()-(current.graph(0)->data()->constBegin()+row)->value)<1e-9
                        && std::abs(values[5].toDouble()-(current.graph(1)->data()->constBegin()+row)->value)<1e-9;
                }
                require(aligned && rows[1].split(' ')[3]=="3.000000000",
                    "1 Hz file values align with voltage and both current curves, even when cleared mid-second; target stays an actual setpoint");
            }
            require(QRegularExpression("^FB_[0-9]{6}_[0-9]{2}\\.txt$")
                .match(QFileInfo(run.paths[0]).fileName()).hasMatch(),"Filename uses FB_ plus Beijing yyMMdd_HH and .txt");
        }
        RecordSettings hourly;hourly.directory=temporary.path()+"/hourly";hourly.frequencyHz=10;
        const qint64 hourEpoch=QDateTime::fromString("2026-10-13T14:30:00.000Z",Qt::ISODateWithMs).toMSecsSinceEpoch();
        QVector<RecordSample> hours;
        for(double t:{0.,1799.999,1800.,5399.999,5400.})hours.append({hourEpoch+qRound64(t*1000),t,1,2,.8,.75,true});
        const auto rotated=record(hourly,hours);
        require(rotated.error.isEmpty() && rotated.paths.size()==3 && rotated.rows==5
            && lines(rotated.paths[0]).size()==2 && lines(rotated.paths[1]).size()==2 && lines(rotated.paths[2]).size()==1,
            "Calendar-hour rotation uses row time, including exact boundaries after a mid-hour start");
        require(QFileInfo(rotated.paths[0]).fileName()=="FB_261013_22.txt"
            && QFileInfo(rotated.paths[1]).fileName()=="FB_261013_23.txt"
            && QFileInfo(rotated.paths[2]).fileName()=="FB_261014_00.txt","Files switch at Beijing hour and midnight boundaries");
        RecordSettings resume;resume.directory=temporary.path()+"/resume";resume.frequencyHz=10;
        const auto firstRun=record(resume,{{hourEpoch,0,1,2,.8,.75,true},{hourEpoch+100,0.1,1.1,2,.81,.76,true}});
        const auto firstRows=recordedRows(resume.directory);
        const auto resumedRun=record(resume,{{hourEpoch+600000,0,3,4,-.2,-.21,true},
            {hourEpoch+600100,0.1,3.1,4,-.22,-.23,true}});
        const auto resumedRows=recordedRows(resume.directory);
        require(firstRun.error.isEmpty() && resumedRun.error.isEmpty() && resumedRun.rows==2
            && firstRun.paths==resumedRun.paths && firstRun.paths.size()==1
            && resumedRows.size()==4 && resumedRows.mid(0,2)==firstRows
            && resumedRows[2]=="26/10/13 22:40:00.000 3.000000000 4.000000000 -0.200000000 -0.210000000",
            "A fresh recorder resumes the same calendar-hour file without a suffix, overwrite, header or gap filler");
        const auto resumedNextHour=record(resume,{{hourEpoch+1800000,0,5,6,.3,.31,true}});
        require(resumedNextHour.error.isEmpty() && resumedNextHour.paths.size()==1
            && QFileInfo(resumedNextHour.paths[0]).fileName()=="FB_261013_23.txt"
            && lines(firstRun.paths[0])==resumedRows,
            "Resuming in a later hour creates that hour's file and preserves the earlier hour");
        RecordSettings year;year.directory=temporary.path()+"/year";year.frequencyHz=10;
        const qint64 yearEnd=QDateTime::fromString("2026-12-31T15:59:59.999Z",Qt::ISODateWithMs).toMSecsSinceEpoch();
        const auto newYear=record(year,{{yearEnd,0,1,2,3,4,true},{yearEnd+1,.001,5,6,7,8,true}});
        require(newYear.error.isEmpty() && newYear.paths.size()==2
            && QFileInfo(newYear.paths[0]).fileName()=="FB_261231_23.txt"
            && QFileInfo(newYear.paths[1]).fileName()=="FB_270101_00.txt",
            "Calendar-hour names also distinguish dates across New Year");
        RecordSettings averageHours;averageHours.directory=temporary.path()+"/average-hours";averageHours.frequencyHz=1;
        QVector<RecordSample> sparse;
        for(double t:{0.,.2,3599.9,3600.,3600.2,7199.9,7200.,7201.})
            sparse.append({epoch+qRound64(t*1000),t,1,2,.8,.75,true});
        const auto hourlyMeans=record(averageHours,sparse);
        require(hourlyMeans.error.isEmpty() && hourlyMeans.rows==5 && hourlyMeans.paths.size()==3
            && lines(hourlyMeans.paths[0]).size()==2 && lines(hourlyMeans.paths[1]).size()==2 && lines(hourlyMeans.paths[2]).size()==1,
            "1 Hz rotates hourly without splitting, losing or fabricating completed second means across gaps");
        RecordSettings irregular;irregular.directory=temporary.path()+"/irregular";irregular.frequencyHz=1;
        const auto irregularRun=record(irregular,{{epoch+300,.3,1,2,3,4,true},{epoch+500,.5,2,2,6,8,true},
            {epoch+999,.999,6,2,9,12,true},{epoch+1000,1,10,5,20,30,true},
            {epoch+1800,1.8,14,5,22,32,true},{epoch+2200,2.2,20,5,40,50,true},
            {epoch+4100,4.1,40,5,60,70,true},{epoch+5000,5,50,5,80,90,true}});
        const auto irregularRows=lines(irregularRun.paths[0]);
        require(irregularRun.error.isEmpty() && irregularRows.size()==4
            && irregularRows[0]==beijingTime(epoch+1000,"yy/MM/dd HH:mm:ss.zzz")+" 3.000000000 5.000000000 6.000000000 8.000000000"
            && irregularRows[1]==beijingTime(epoch+2000,"yy/MM/dd HH:mm:ss.zzz")+" 12.000000000 5.000000000 21.000000000 31.000000000"
            && irregularRows[3].startsWith(beijingTime(epoch+5000,"yy/MM/dd HH:mm:ss.zzz")),
            "1 Hz uses the actual sample count, exact boundaries, no empty-window fill and the target at each curve timestamp");
        RecordSettings shortRun;shortRun.directory=temporary.path()+"/short";shortRun.frequencyHz=1;
        const auto incomplete=record(shortRun,{{epoch+100,.1,1,2,3,4,true},{epoch+900,.9,2,2,4,5,true}});
        require(incomplete.error.isEmpty() && incomplete.rows==0 && incomplete.paths.isEmpty(),
            "Stopping before the first complete window does not save a partial mean absent from the curve");
        RecordSettings simulated;simulated.directory=temporary.path()+"/sim";simulated.frequencyHz=10;simulated.simulated=true;
        const auto currentOnly=record(simulated,{{epoch,0,999,999,-1.1,-1,false}});
        require(currentOnly.error.isEmpty() && currentOnly.paths[0].contains("IVI仿真")
            && lines(currentOnly.paths[0]).size()==1
            && lines(currentOnly.paths[0])[0].endsWith(" NA NA -1.100000000 -1.000000000"),
            "CH2-only rows mark unavailable probe/target as NA; simulation is visibly isolated");
        const QString blocked=temporary.path()+"/not-a-directory";
        {QFile file(blocked);require(file.open(QIODevice::WriteOnly),"Create invalid destination fixture");}
        RecordSettings invalid;invalid.directory=blocked;invalid.frequencyHz=10;
        const auto failed=record(invalid,samples);
        require(!failed.ready && !failed.error.isEmpty(),"Invalid storage destination fails before ready");
        invalid.directory=temporary.path()+"/invalid-sample";
        const auto badSample=record(invalid,{{epoch,0,1,2,.8,std::numeric_limits<double>::quiet_NaN(),true}});
        require(!badSample.error.isEmpty(),"Nonfinite measurement reports recording failure instead of corrupting TXT");
        const auto badOutput=record(invalid,{{epoch,0,1,2,std::numeric_limits<double>::quiet_NaN(),.75,true}});
        require(!badOutput.error.isEmpty(),"Nonfinite output command reports recording failure instead of corrupting TXT");
        for(double frequency:{0.,.1,2.,20.,50.,100.,101.,200.,500.,std::numeric_limits<double>::quiet_NaN()}) {
            invalid.frequencyHz=frequency;
            const auto badFrequency=record(invalid,{});
            require(!badFrequency.ready && !badFrequency.error.isEmpty(),"Unsupported storage frequency is rejected");
        }

        // Every UI connection below is explicitly IVI driver simulation, never physical I/O.
        Window window(Window::Backend::DriverSimulationForTest);
        auto* saveData=window.findChild<QCheckBox*>("saveData");
        auto* address=window.findChild<QLineEdit*>("visaAddress");
        auto* directory=window.findChild<QLineEdit*>("storageDirectory");directory->setText(temporary.path()+"/ui");
        auto* frequency=window.findChild<QComboBox*>("storageFrequency");
        auto* defaultKp=window.findChild<QDoubleSpinBox*>("proportionalGain");
        auto* defaultTi=window.findChild<QDoubleSpinBox*>("integralTime");
        auto* defaultNplc=window.findChild<QDoubleSpinBox*>("measurementNplc");
        QStringList frequencyLabels;for(int i=0;i<frequency->count();++i)frequencyLabels<<frequency->itemText(i);
        require(saveData->isChecked() && !frequency->isEditable() && frequency->currentData().toInt()==1
            && defaultKp && defaultTi && std::abs(defaultKp->value()-.8)<1e-12 && std::abs(defaultTi->value()-5.2)<1e-12
            && defaultNplc && std::abs(defaultNplc->value()-1.0)<1e-12
            && frequencyLabels==QStringList({"1 Hz","10 Hz"}),
            "Defaults are enabled 1 Hz storage, Kp 0.8 mA/V, Ti 5.2 s and 1.0 NPLC");
        frequency->setCurrentIndex(frequency->findData(10));
        auto* target=window.findChild<QDoubleSpinBox*>("targetVoltage");target->setValue(4);
        defaultKp->setValue(.2);defaultTi->setValue(.1);defaultNplc->setValue(.1);
        auto* start=window.findChild<QPushButton*>("startControlButton");
        auto* apply=window.findChild<QPushButton*>("applyTargetButton");
        auto* lamp=window.findChild<QLabel*>("loopLamp");
        auto* clock=window.findChild<QLabel*>("beijingClock");
        auto* status=window.findChild<QLabel*>("storageStatus");
        auto* probe=window.findChild<TracePlot*>("probePlot");auto* current=window.findChild<TracePlot*>("currentPlot");
        const QString initialClock=clock->text();
        require(lamp->property("state")=="idle" && initialClock.startsWith("北京时间"),"Idle lamp is gray and Beijing clock is present without a connection");
        window.show();QCoreApplication::processEvents();
        auto* monitorTabs=window.findChild<QTabWidget*>("monitorTabs");
        auto* controlPanel=window.findChild<QFrame*>("controlPanel");
        require(monitorTabs && monitorTabs->count()==2 && monitorTabs->currentIndex()==0
            && monitorTabs->tabText(0)==QStringLiteral("实时曲线") && monitorTabs->tabText(1)==QStringLiteral("运行日志")
            && monitorTabs->widget(0)->objectName()==QStringLiteral("curvesPage")
            && monitorTabs->widget(1)->objectName()==QStringLiteral("logPage")
            && controlPanel && controlPanel->width()<=345,
            "Curves and log share two tabs, curves are selected by default, and the control panel leaves more plot width");
        auto* curveSplitter=window.findChild<QSplitter*>("curveSplitter");
        require(curveSplitter && curveSplitter->orientation()==Qt::Vertical
            && probe->geometry().top()<current->geometry().top()
            && probe->width()>=curveSplitter->width()-20 && current->width()>=curveSplitter->width()-20
            && probe->height()>=150 && current->height()>=150,
            "Voltage and current curves use two full-width rows with equal usable plot height");
        window.testConnection();waitUntil([&]{return start->isEnabled();});
        QMap<double,QPair<double,double>> currents;
        QObject::connect(window.findChild<SessionThread*>(),&ControlThread::sample,&window,
            [&](double time,double,double command,double measured,double,double){currents.insert(time,qMakePair(command,measured));});
        window.start();require(lamp->property("state")=="starting","PID lamp shows starting before first valid sample");
        waitUntil([&]{return window.sampleCount>=4 && !current->graph(0)->data()->isEmpty();});
        require(lamp->property("state")=="running" && !frequency->isEnabled() && !directory->isEnabled()
            && current->isVisible() && current->graph(0)->data()->size()==probe->graph(0)->data()->size(),
            "PID enables green lamp and both voltage/current plots, locking storage configuration");
        target->setValue(6);const int edited=window.sampleCount;
        waitUntil([&]{return window.sampleCount>=edited+3;});
        require(!recordedRows(directory->text()).join('\n').contains(" 6.000000000 "),"Unapplied target edit is not stored as an active target");
        apply->click();waitUntil([&]{return apply->isEnabled() && (probe->graph(1)->data()->constEnd()-1)->value==6
            && recordedRows(directory->text()).join('\n').contains(" 6.000000000 ");});
        require(clock->text()!=initialClock,"Beijing clock keeps updating while control runs");
        window.grab().save(QDir(output).filePath("meeting-gui-running.png"));
        const int beforeClear=recordedRows(directory->text()).size();
        window.findChild<QPushButton*>("clearCurveButton")->click();
        require(probe->graph(0)->data()->isEmpty() && current->graph(0)->data()->isEmpty()
            && current->graph(1)->data()->isEmpty(),"Clear curves clears voltage and both current histories");
        waitUntil([&]{return recordedRows(directory->text()).size()>beforeClear && current->graph(0)->data()->size()>=2;});
        window.stop();waitUntil([&]{return start->isEnabled();});
        require(lamp->property("state")=="idle" && frequency->isEnabled() && status->text().contains("数据已保存"),
            "Stop drains recording, resets lamp and restores settings without disconnecting");
        require(recordedRows(directory->text()).size()==window.sampleCount,"10 Hz saves exactly one row per acquired sample, including samples before curve clear");
        require(current->graphCount()==2 && current->legend->itemCount()==2
            && current->graph(0)->name()=="输出电流" && current->graph(1)->name()=="实测电流",
            "UI current chart names the two curves output current and measured current");
        require(!current->graph(0)->data()->isEmpty(),"Output-current curve continues after clearing history");
        bool curvesMatch=true;
        for(int curve=0;curve<2;++curve) {
            for(auto point=current->graph(curve)->data()->constBegin();point!=current->graph(curve)->data()->constEnd();++point) {
                double sum=0;int count=0;
                for(auto raw=currents.lowerBound(point->key-1);raw!=currents.constEnd() && raw.key()<point->key;++raw) {
                    sum+=curve==0 ? raw.value().first : raw.value().second;++count;
                }
                curvesMatch=curvesMatch && count>0 && std::abs(point->value-sum/count)<1e-12;
            }
        }
        require(curvesMatch,"Current curves independently match exact one-second means of raw worker commands and measurements after clear");
        const auto stored=recordedRows(directory->text());
        require(stored.size()==currents.size(),"Recorder and UI receive the same sample count");
        int row=0;bool recordsMatch=true;
        for(auto it=currents.constBegin();it!=currents.constEnd();++it) {
            const auto columns=stored[row++].split(' ');
            recordsMatch=recordsMatch && std::abs(columns[4].toDouble()-it.value().first)<1e-8
                && std::abs(columns[5].toDouble()-it.value().second)<1e-8;
        }
        require(recordsMatch,"TXT retains every raw output and measured current in order, without curve averaging");
        window.grab().save(QDir(output).filePath("meeting-gui-stopped.png"));

        directory->setText(blocked);window.start();waitUntil([&]{return start->isEnabled();});
        require(window.sampleCount==0 && status->text().contains("已取消启动") && lamp->property("state")=="idle",
            "Storage preflight failure cancels output startup and preserves connection");
        directory->setText(temporary.path()+"/runtime-failure");window.start();waitUntil([&]{return window.sampleCount>=3;});
        DataRecorder* live=nullptr;for(auto* writer:window.findChildren<DataRecorder*>())if(writer->isRunning())live=writer;
        require(live!=nullptr,"Recorder is active on an independent thread");
        live->submit({epoch,-1,0,0,0,0,true});waitUntil([&]{return status->text().contains("数据存储失败");});
        const int beforeFailure=window.sampleCount;waitUntil([&]{return window.sampleCount>=beforeFailure+3;});
        require(lamp->property("state")=="running" && !window.hadError,"Runtime recording failure is visible and does not silently stop PID control");
        window.stop();waitUntil([&]{return start->isEnabled();});

        directory->setText(temporary.path()+"/manual");
        window.findChild<QDoubleSpinBox*>("manualCurrent")->setValue(-.4);
        window.start(true);waitUntil([&]{return window.sampleCount>=3 && !current->graph(0)->data()->isEmpty();});
        require(lamp->property("state")=="idle" && current->isVisible(),"Manual current output leaves closed-loop lamp gray");
        window.stop();waitUntil([&]{return start->isEnabled();});
        require(std::abs(current->graph(0)->data()->constBegin()->value+.4)<1e-12
            && recordedRows(directory->text()).first().split(' ')[4]=="-0.400000000",
            "Manual mode plots and records the signed manual command as output current");
        window.start();window.stop();waitUntil([&]{return start->isEnabled();});
        require(window.sampleCount==0,"Immediate stop during storage preparation prevents control startup");
        window.start();waitUntil([&]{return window.sampleCount>=3;});
        // Inject a UI fault only into the simulated session, then close it normally.
        auto* session=window.findChild<SessionThread*>();session->failed("Offline test fault");session->disconnectDevice();
        waitUntil([&]{return address->isEnabled();});
        require(lamp->property("state")=="fault" && !start->isEnabled(),"Control fault shows red lamp and disables start");
        window.close();waitUntil([&]{return !window.isVisible();});

        Window closing(Window::Backend::DriverSimulationForTest);
        closing.findChild<QCheckBox*>("saveData")->setChecked(true);
        closing.findChild<QLineEdit*>("storageDirectory")->setText(temporary.path()+"/close-active");
        auto* closingFrequency=closing.findChild<QComboBox*>("storageFrequency");
        closingFrequency->setCurrentIndex(closingFrequency->findData(10));
        closing.show();closing.testConnection();waitUntil([&]{return closing.findChild<QPushButton*>("startControlButton")->isEnabled();});
        closing.start();waitUntil([&]{return closing.sampleCount>=3;});closing.close();
        waitUntil([&]{return !closing.isVisible();});
        require(recordedRows(temporary.path()+"/close-active").size()==closing.sampleCount,
            "Closing an active window waits for final sample and recorder drain before exiting");
    } catch(const std::exception& error) {checks<<"FAIL "+QString::fromUtf8(error.what());result=1;}
    QFile file(QDir(output).filePath("meeting-test.txt"));
    if(file.open(QIODevice::WriteOnly)){QTextStream stream(&file);stream.setCodec("UTF-8");stream<<checks.join('\n');}
    return result;
}

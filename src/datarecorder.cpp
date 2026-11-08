#include "datarecorder.h"
#include "secondaverage.h"
#include <QDir>
#include <QCoreApplication>
#include <QLocale>
#include <algorithm>
#include <cmath>
#include <stdexcept>

QString beijingTime(qint64 utcMs,const QString& format) {
    return QDateTime::fromMSecsSinceEpoch(utcMs,Qt::UTC).toOffsetFromUtc(8*3600).toString(format);
}
QString defaultDataDirectory() {
    return QDir(QCoreApplication::applicationDirPath()).filePath("HisData");
}
void RecordSettings::validate() const {
    if(directory.trimmed().isEmpty()) throw std::runtime_error("请选择数据存储目录");
    if(std::find(supportedFrequenciesHz.begin(),supportedFrequenciesHz.end(),frequencyHz)==supportedFrequenciesHz.end())
        throw std::runtime_error("存储频率请选择 1 或 10 Hz");
}
DataRecorder::DataRecorder(RecordSettings options,QObject* parent):QThread(parent),settings(options) {}
DataRecorder::~DataRecorder() {finish();wait();}
void DataRecorder::finish() {
    finishing.store(true);
    QMutexLocker lock(&mutex);closing=true;accepting=false;wake.wakeAll();
}
void DataRecorder::submit(const RecordSample& sample) {
    QMutexLocker lock(&mutex);
    if(!accepting || closing) return;
    if(!std::isfinite(sample.seconds) || sample.seconds<0 || sample.seconds<lastSample
        || !std::isfinite(sample.outputMA) || !std::isfinite(sample.measuredMA)
        || (sample.hasProbe && (!std::isfinite(sample.probeV) || !std::isfinite(sample.targetV)))) {
        queueError="采样数据无效，已停止存储";accepting=false;wake.wakeAll();return;
    }
    lastSample=sample.seconds;
    if(pending.size()>=4096) {
        queueError="写入速度不足，数据队列已满，已停止存储";accepting=false;wake.wakeAll();return;
    }
    pending.enqueue(sample);
    wake.wakeOne();
}
void DataRecorder::run() {
    quint64 rows=0;
    QFile file;
    QString fileHour;
    auto write=[&](const QByteArray& bytes){
        if(file.write(bytes)!=bytes.size()) throw std::runtime_error(("TXT 写入失败："+file.errorString()).toUtf8().constData());
    };
    auto flush=[&]{
        if(file.isOpen() && !file.flush()) throw std::runtime_error(("TXT 保存失败："+file.errorString()).toUtf8().constData());
    };
    auto openFile=[&](qint64 utcMs){
        flush();file.close();
        const QString directory=settings.simulated ? QDir(settings.directory).filePath("IVI仿真") : settings.directory;
        if(!QDir().mkpath(directory)) throw std::runtime_error("无法创建数据存储目录");
        fileHour=beijingTime(utcMs,"yyMMdd_HH");
        file.setFileName(QDir(directory).filePath("FB_"+fileHour+".txt"));
        // A calendar hour keeps one file across stops and application restarts.
        // Append never truncates the measurements already recorded in that hour.
        if(!file.open(QIODevice::WriteOnly|QIODevice::Append))
            throw std::runtime_error(("无法追加 TXT："+file.errorString()).toUtf8().constData());
        // Headerless UTF-8: the first byte belongs to the first data timestamp.
        flush();emit fileChanged(file.fileName());
    };
    try {
        settings.validate();
        if(finishing.load()) return;
        // Verify the initial file before the UI starts instrument output.
        openFile(QDateTime::currentMSecsSinceEpoch());
        {QMutexLocker lock(&mutex);accepting=!closing;}
        if(!finishing.load()) emit ready();
        auto saveRow=[&](const RecordSample& sample) {
            // Use the row timestamp, not elapsed run time or writer arrival time.
            // For 1 Hz this is still the plotted averaging-window end timestamp.
            if(beijingTime(sample.utcMs,"yyMMdd_HH")!=fileHour) openFile(sample.utcMs);
            const auto number=[](double value){return QLocale::c().toString(value,'f',9);};
            write(QString("%1 %2 %3 %4 %5\r\n")
                .arg(beijingTime(sample.utcMs,"yy/MM/dd HH:mm:ss.zzz"),sample.hasProbe ? number(sample.probeV) : "NA",
                     sample.hasProbe ? number(sample.targetV) : "NA",number(sample.outputMA),number(sample.measuredMA)).toUtf8());
            flush();++rows;
        };
        SecondAverage<3> averaging;
        RecordSample previous;
        bool haveSample=false;
        qint64 timeOriginUtcMs=0;
        while(true) {
            RecordSample sample;
            {
                QMutexLocker lock(&mutex);
                while(pending.isEmpty() && !closing && queueError.isEmpty()) wake.wait(&mutex);
                if(!queueError.isEmpty()) throw std::runtime_error(queueError.toUtf8().constData());
                if(pending.isEmpty() && closing) break;
                sample=pending.dequeue();
            }
            if(settings.frequencyHz==10) {
                // "10 Hz" means every acquired sample, even if actual cadence varies.
                saveRow(sample);
                continue;
            }
            if(!haveSample)timeOriginUtcMs=sample.utcMs-qRound64(sample.seconds*1000);
            else if(sample.hasProbe!=previous.hasProbe)
                throw std::runtime_error("同次存储的测量通道发生变化，已停止存储");
            SecondAverage<3>::Point completed;
            if(averaging.append(sample.seconds,{{sample.hasProbe ? sample.probeV : 0,sample.outputMA,sample.measuredMA}},completed)) {
                RecordSample mean=previous;
                mean.seconds=completed.end;
                mean.utcMs=timeOriginUtcMs+qRound64(completed.end*1000);
                mean.probeV=completed.values[0];mean.outputMA=completed.values[1];mean.measuredMA=completed.values[2];
                // Match the unsmoothed target line at the plotted window-end time.
                if(sample.seconds==completed.end)mean.targetV=sample.targetV;
                saveRow(mean);
            }
            previous=sample;haveSample=true;
        }
        // 1 Hz only saves closed windows, exactly as the curve does; an unfinished
        // tail is not emitted as a second mean. 10 Hz has already saved every row.
        flush();emit saved(rows);
    } catch(const std::exception& error) {emit failed(QString::fromUtf8(error.what()));}
    file.close();
    QMutexLocker lock(&mutex);accepting=false;pending.clear();
}

#pragma once
#include <QtCore>
#include <atomic>
#include <functional>
inline void sdkDiagnostic(const QString& text) {
    static QMutex mutex;
    QMutexLocker guard(&mutex);
    const QString directory=QCoreApplication::applicationDirPath()+"/Diagnostics";
    QDir().mkpath(directory);
    QFile file(directory+"/sdk_"+QDateTime::currentDateTime().toString("yyyyMMdd_HH")+".log");
    if(file.size()>20*1024*1024) {
        const QString rotated=file.fileName()+".1";
        QFile::remove(rotated);file.rename(rotated);file.setFileName(rotated.left(rotated.size()-2));
    }
    if(file.open(QIODevice::Append)) {
        file.write((QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")+" "+text+"\n").toUtf8());
        file.flush();
    }
}
inline bool diagnosticImportant(const QString& name) {
    return name.contains("InitWithOptions") || name.contains("Tracing")
        || name.contains("RequestLock") || name.contains("ReleaseLock")
        || name.contains("ClearIO") || name.endsWith("_reset") || name.endsWith("_close");
}
inline void diagnosticCall(const QString& line,bool persist,bool fault=false) {
    static QMutex mutex;
    static QStringList recent;
    static QElapsedTimer minute;
    static quint64 calls=0;
    static double maximumMs=0;
    QMutexLocker guard(&mutex);
    if(!minute.isValid()) minute.start();
    recent.append(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")+" "+line);
    while(recent.size()>2000) recent.removeFirst();
    if(line.startsWith("END")) {
        ++calls;
        const auto match=QRegularExpression("elapsed_ms=([0-9.]+)").match(line);
        if(match.hasMatch()) maximumMs=std::max(maximumMs,match.captured(1).toDouble());
    }
    if(fault) {
        sdkDiagnostic("RECENT_CALLS_BEGIN\n"+recent.join('\n')+"\nRECENT_CALLS_END");
        recent.clear();
    } else if(persist) sdkDiagnostic(line);
    if(minute.elapsed()>=60000) {
        sdkDiagnostic(QString("SDK_SUMMARY completed_calls=%1 max_elapsed_ms=%2 interval_ms=%3")
            .arg(calls).arg(maximumMs).arg(minute.elapsed()));
        calls=0;maximumMs=0;minute.restart();
        // Bound retained hourly SDK logs to 48 files, plus bounded rotations.
        QDir dir(QCoreApplication::applicationDirPath()+"/Diagnostics");
        const auto files=dir.entryList({"sdk_*.log","sdk_*.log.1"},QDir::Files,QDir::Name);
        for(int i=0;i<files.size()-48;++i) dir.remove(files[i]);
    }
}
template<class R,class... A> std::function<R(A...)> diagnosticSdk(R (*function)(A...),const char* name) {
    return [function,name](A... args)->R {
        static std::atomic<quint64> sequence{0};
        const quint64 id=++sequence;
        const bool important=diagnosticImportant(QString::fromLatin1(name));
        diagnosticCall(QString("BEGIN id=%1 SDK=%2").arg(id).arg(name),important);
        QElapsedTimer timer;timer.start();
        const R result=function(args...);
        diagnosticCall(QString("END id=%1 SDK=%2 elapsed_ms=%3 status=%4 hex=0x%5")
            .arg(id).arg(name).arg(timer.nsecsElapsed()/1e6,0,'f',3)
            .arg(qint64(result)).arg(quint32(result),8,16,QChar('0')),important,
            result<0 || timer.elapsed()>=1000);
        return result;
    };
}
template<class T> struct DiagnosticFunction;
template<class R,class... A> struct DiagnosticFunction<R (*)(A...)> {using Type=std::function<R(A...)>;};

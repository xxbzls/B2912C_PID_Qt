#pragma once
#include <QDateTime>
#include <QFile>
#include <QMutex>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>
#include <atomic>
#include <array>

QString beijingTime(qint64 utcMs, const QString& format = "yyyy-MM-dd HH:mm:ss.zzz");
QString defaultDataDirectory();
struct RecordSample {
    qint64 utcMs = 0;
    double seconds = 0, probeV = 0, targetV = 0, outputMA = 0, measuredMA = 0;
    bool hasProbe = true;
};
struct RecordSettings {
    static constexpr std::array<int,2> supportedFrequenciesHz{{1,10}};
    QString directory;
    double frequencyHz = 1;
    bool simulated = false;
    void validate() const;
};
// submit only enqueues data; all file operations run on this separate thread.
class DataRecorder : public QThread {
    Q_OBJECT
public:
    explicit DataRecorder(RecordSettings settings, QObject* parent=nullptr);
    ~DataRecorder() override;
    void submit(const RecordSample& sample);
    void finish();
signals:
    void ready();
    void fileChanged(QString path);
    void failed(QString message);
    void saved(quint64 rows);
protected:
    void run() override;
private:
    RecordSettings settings;
    QMutex mutex;
    QWaitCondition wake;
    QQueue<RecordSample> pending;
    bool closing=false, accepting=false;
    QString queueError;
    double lastSample=-1;
    std::atomic_bool finishing{false};
};

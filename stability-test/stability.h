#pragma once
#include "datarecorder.h"
#include <QWidget>
#include <QElapsedTimer>
#include <atomic>
#include <memory>

class TracePlot;
class QLabel;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;

qint64 monotonicNs();
struct TestOptions {
    int periodMs=100;
    double targetV=0;
    double hours=24;
    bool draw=true, save=true;
    int storageHz=10;
    QString directory;
    // Used only by automated offline checks, never exposed in the operator UI.
    int injectAfterSamples=0, injectDelayMs=0;
};
struct TestMetrics {
    std::atomic<quint64> produced{0}, delivered{0}, overruns{0};
    std::atomic<qint64> heartbeatNs{0};
    std::atomic<double> lastGapMs{0}, maxGapMs{0}, maxDispatchMs{0}, maxPaintMs{0};
};
struct TestFrame {
    RecordSample sample;
    quint64 sequence=0;
    qint64 sentNs=0;
    double gapMs=0;
};
Q_DECLARE_METATYPE(TestFrame)

class TestWorker : public QThread {
    Q_OBJECT
public:
    TestWorker(TestOptions options, std::shared_ptr<TestMetrics> metrics, QObject* parent=nullptr);
signals:
    void frame(TestFrame sample);
    void rawSample(const RecordSample& sample);
    void message(QString text);
    void diagnostics(QString path);
    void failed(QString text);
protected:
    void run() override;
private:
    TestOptions options;
    std::shared_ptr<TestMetrics> metrics;
    void interruptibleWait(qint64 deadlineNs);
};

class StabilityWindow : public QWidget {
    Q_OBJECT
public:
    explicit StabilityWindow(QWidget* parent=nullptr);
    ~StabilityWindow() override;
    void startTest();
    void stopTest();
    bool isTesting() const { return worker || recorder; }
    std::shared_ptr<TestMetrics> testMetrics() const { return metrics; }
    TestOptions optionsForTest; // Only delay injection is read from this by offline self-test.
signals:
    void testFinished();
protected:
    void closeEvent(QCloseEvent* event) override;
private:
    TestWorker* worker=nullptr;
    DataRecorder* recorder=nullptr;
    std::shared_ptr<TestMetrics> metrics=std::make_shared<TestMetrics>();
    TestOptions activeOptions;
    bool stopping=false, closing=false, recorderDone=false, hadFailure=false;
    QString dataPath, diagnosticPath;
    TracePlot *probe=nullptr, *current=nullptr;
    QLabel *state=nullptr, *readings=nullptr, *health=nullptr, *files=nullptr, *clock=nullptr;
    QLabel *probeReading=nullptr, *targetReading=nullptr, *outputReading=nullptr, *measuredReading=nullptr;
    QLabel *voltageLimitStatus=nullptr, *loopLamp=nullptr, *loopState=nullptr;
    QPushButton *start=nullptr, *stop=nullptr, *browse=nullptr;
    QPushButton *connectButton=nullptr, *manualButton=nullptr, *applyTargetButton=nullptr, *openFolder=nullptr;
    QWidget* parameters=nullptr;
    QLineEdit* resource=nullptr;
    QDoubleSpinBox *target=nullptr, *kp=nullptr, *ti=nullptr, *td=nullptr, *limit=nullptr, *manual=nullptr, *nplc=nullptr, *voltageLimit=nullptr;
    QComboBox* direction=nullptr;
    QCheckBox* overrideVoltageLimit=nullptr;
    QSpinBox* period=nullptr;
    QDoubleSpinBox* duration=nullptr;
    QComboBox* frequency=nullptr;
    QCheckBox *save=nullptr, *draw=nullptr;
    QLineEdit* directory=nullptr;
    QPlainTextEdit* log=nullptr;
    QElapsedTimer paintProbe, paintCurrent;
    qint64 startedNs=0;
    void buildUi();
    void launchWorker();
    void receiveFrame(const TestFrame& frame);
    void updateControls();
    void append(const QString& text);
    void finishIfIdle();
    void updateFiles();
    void updateHealth();
};
int stabilitySelfTest();

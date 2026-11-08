#pragma once
#include <QWidget>
#include <QVector>
#include <QPointF>
#include "controller.h"
class QComboBox;
class QLineEdit;
class QDoubleSpinBox;
class QSpinBox;
class QPlainTextEdit;
class QLabel;
class QPushButton;
class QCloseEvent;
class QCheckBox;
class ControlThread;
class TracePlot;
class Window : public QWidget {
    Q_OBJECT
public:
    enum class Backend { RealInstrument, DriverSimulationForTest };
    explicit Window(Backend backend=Backend::RealInstrument);
    ~Window() override;
    void start(bool manual=false);
    void stop();
    void testConnection();
    void applyTarget();
    int sampleCount = 0;
    bool hadError = false;
signals:
    void stopped();
    void connectionChanged(bool connected);
protected:
    void closeEvent(QCloseEvent*) override;
private:
    // Offline automated tests opt in explicitly; the operator UI always uses the real instrument.
    const bool driverSimulation;
    SessionThread* session = nullptr;
    bool sessionReady = false;
    bool active = false;
    bool stopping = false, targetPending = false;
    bool preparingControl = false, notifyStopped = false, storageFailed = false, recorderFinished = false;
    DataRecorder* recorder = nullptr;
    QString identity;
    Settings activeSettings;
    QWidget* parameters;
    QComboBox* direction;
    QLineEdit* resource;
    QDoubleSpinBox *target, *kp, *ti, *td, *limit, *voltageLimit, *nplc, *manual;
    QSpinBox* period;
    QCheckBox* saveData;
    QComboBox* storageFrequency;
    QLineEdit* storageDirectory;
    QPushButton *browseStorage, *openStorage;
    QLabel *storageStatus, *beijingClock, *loopLamp, *loopState;
    QLabel *values, *connectionStatus, *voltageLimitStatus;
    QLabel *probeReading, *targetReading, *outputReading, *measuredReading;
    QCheckBox* overrideVoltageLimit;
    QPlainTextEdit* log;
    QPushButton *startButton, *manualButton, *stopButton, *connectButton, *applyTargetButton;
    TracePlot* trace;
    TracePlot* currentTrace;
    bool closing = false;
    void append(QString message);
    void resetConnectionStatus();
    void showConnectionStatus(const QString& text, const QString& color);
    void refreshControls();
    void attachSignals(ControlThread* thread);
    void beginControl();
    void finishRun();
    void releaseRecorder();
    void refreshLoopState();
    void buildUi();
    void showOutputOff();
    void clearLiveReadings(const QString& reason);
};

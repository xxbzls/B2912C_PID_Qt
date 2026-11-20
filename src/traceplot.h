#pragma once
#include "qcustomplot.h"
#include "secondaverage.h"
#include <deque>
#include <limits>
class QTimer;

class TracePlot : public QCustomPlot {
public:
    explicit TracePlot(QWidget* parent = nullptr);
    void resetData(double target);
    void clearData();
    void appendSample(double seconds, double value);
    void appendSample(double seconds, double value, double target);
    void appendCurrentSample(double seconds, double outputMA, double measuredMA);
    void restoreView();
    void setCurrentMode(bool enabled);
    bool followsLatest() const { return autoFollow; }
protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
private:
    double targetValue = 2;
    QMap<double,double> targetHistory;
    QSharedPointer<QCPGraphDataContainer> liveData[2];
    bool autoFollow = true;
    bool manualYRange = false;
    // Exact user-selected Y range. New samples may advance X, but must never
    // replace this range until the user explicitly restores the full view.
    QCPRange lockedYRange;
    // Zero means show the complete retained history. A positive value keeps
    // the user-selected wheel-zoom span while the right edge follows new data.
    double followWindowSeconds = 0;
    bool currentMode = false;
    static constexpr double historySeconds = 24 * 60 * 60;
    SecondAverage<2> averaging;
    double lastSampleTime = -std::numeric_limits<double>::infinity();
    // Monotonic queues keep full-history autoscaling O(1) per sample.
    std::deque<QPair<double,double>> minimums, maximums;
    QTimer* redrawTimer;
    QPoint dragStartPixel;
    bool dragCandidate = false, dragInProgress = false;
    void fitLatest();
    void accumulateSample(double seconds, double value, double measured = 0);
    void finishSample(double seconds, double lower, double upper);
    double targetAt(double seconds) const;
    double displayedTargetAt(double seconds) const;
    void updateTargetLine();
};

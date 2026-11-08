#pragma once
#include "qcustomplot.h"
#include "secondaverage.h"
#include <deque>
#include <limits>
class QLabel;
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
    void leaveEvent(QEvent* event) override;
private:
    double targetValue = 2;
    QMap<double,double> targetHistory;
    QSharedPointer<QCPGraphDataContainer> liveData[2];
    QMap<double,double> pausedTargetHistory;
    double pausedTargetValue = 2;
    bool autoFollow = true;
    bool manualYRange = false;
    bool hovering = false;
    bool currentMode = false;
    static constexpr double historySeconds = 24 * 60 * 60;
    SecondAverage<2> averaging;
    double lastSampleTime = -std::numeric_limits<double>::infinity();
    // Monotonic queues keep full-history autoscaling O(1) per sample.
    std::deque<QPair<double,double>> minimums, maximums;
    QTimer* redrawTimer;
    QTimer* hoverTimer;
    QPoint hoverPixel;
    QPoint dragStartPixel;
    bool dragCandidate = false;
    double hoverKey = std::numeric_limits<double>::quiet_NaN();
    QLabel* hoverLabel;
    QCPItemTracer* marker;
    QCPItemStraightLine* cursorLine;
    void fitLatest();
    void pauseView();
    void accumulateSample(double seconds, double value, double measured = 0);
    void finishSample(double seconds, double lower, double upper);
    double targetAt(double seconds) const;
    double displayedTargetAt(double seconds) const;
    void updateTargetLine();
    void scheduleHover(QPoint pixel);
    void updateHover(QPoint pixel);
    void hideHover();
};

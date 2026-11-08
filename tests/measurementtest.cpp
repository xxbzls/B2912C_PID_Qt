#include "instrument.h"
#include <QDir>
#include <QFile>
#include <QScopeGuard>
#include <QTextStream>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
double reply = 0;
ViInt32 replyCount = 1;
int failedStage = 0;
bool leaveValueUnwritten = false;
QStringList operations;
ViStatus _VI_FUNC initiate(ViSession, ViConstString channels) {
    operations << "init " + QString::fromLatin1(channels);
    return failedStage == 1 ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
ViStatus _VI_FUNC waitComplete(ViSession, ViInt32) {
    operations << "wait";
    return failedStage == 2 ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
ViStatus _VI_FUNC fetch(ViSession, ViInt32, ViConstString channels,
                        ViInt32 size, ViReal64 values[], ViInt32* actual) {
    operations << "fetch " + QString::fromLatin1(channels);
    if (size != 1) return KTB2900_ERROR_IO_GENERAL;
    if (!leaveValueUnwritten) values[0] = reply;
    *actual = replyCount;
    return failedStage == 3 ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
}

int measurementTest() {
    QStringList results; int result = 0;
    Instrument instrument;
    // Exercise the production acquisition/validation code with injected driver
    // functions. There is no DLL, VISA session, output enable or current write.
    instrument.session = 1;
    instrument.MeasurementInitiate = initiate;
    instrument.SystemWaitForOperationComplete = waitComplete;
    instrument.MeasurementFetchScalarData = fetch;
    const auto cleanup = qScopeGuard([&]{instrument.session = VI_NULL;});
    auto require = [&](bool ok, const QString& text) {
        if (!ok) throw std::runtime_error(text.toStdString());
        results << "PASS " + text;
    };
    auto errorFrom = [](const std::function<double()>& read) {
        try { read(); } catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
        return QString();
    };
    try {
        struct Reading { QString label, unit, channels; std::function<double()> read; };
        const Reading readings[] = {
            {"CH1 电压", "V", "(@1)", [&]{return instrument.readVoltage();}},
            {"CH2 实测电流", "A", "(@2)", [&]{return instrument.readCurrent();}},
            {"CH2 电压", "V", "(@2)", [&]{return instrument.readOutputVoltage();}}
        };
        for (const auto& reading : readings) {
            failedStage = 0; replyCount = 1; leaveValueUnwritten = false;
            for (double valid : {0.0, 0.0001, -0.0001}) {
                reply = valid; operations.clear(); instrument.rawQueryPending = false;
                require(reading.read() == valid && !instrument.rawQueryPending && operations == QStringList{
                    "init " + reading.channels, "wait", "fetch " + reading.channels},
                    reading.label + " accepts signed finite readings, preserves acquire/wait/fetch order and closes the query transaction");
            }
            for (double invalid : {9.91e37, -9.91e37, std::numeric_limits<double>::quiet_NaN(),
                                   std::numeric_limits<double>::infinity()}) {
                reply = invalid;
                const QString error = errorFrom(reading.read);
                require(error.contains(reading.label) && error.contains("返回数量=1")
                    && error.contains("原始值=") && error.contains(" " + reading.unit + "。")
                    && error.contains("拒绝将该读数用于控制"),
                    reading.label + " rejects invalid markers/NaN/infinity with channel, native unit and raw result");
            }
            reply = 0;
            for (ViInt32 count : {0, 2, -1}) {
                replyCount = count;
                const QString error = errorFrom(reading.read);
                require(error.contains(reading.label) && error.contains("返回数据数量不符")
                    && error.contains(QString("返回数量=%1").arg(count)),
                    reading.label + " rejects missing or unexpected sample counts even when buffer contains zero");
            }
            replyCount = 1; leaveValueUnwritten = true;
            require(errorFrom(reading.read).contains("返回非有限值或未写入测量值"),
                reading.label + " does not turn an unwritten buffer into a valid zero reading");
            leaveValueUnwritten = false;
            const QString stages[] = {"启动测量", "等待测量完成", "读取测量结果"};
            for (int stage = 1; stage <= 3; ++stage) {
                failedStage = stage; operations.clear(); instrument.rawQueryPending = false;
                const QString error = errorFrom(reading.read);
                require(error.contains(reading.label) && error.contains(stages[stage-1])
                    && operations.size() == stage && instrument.rawQueryPending == (stage >= 2),
                    reading.label + " identifies failed acquisition stage and marks incomplete query responses for shutdown clearing");
            }
            failedStage = 0; instrument.rawQueryPending = false;
        }
        results << "LIMIT: Offline injected driver results only; no real instrument or electrical fault reproduced.";
    } catch (const std::exception& e) {results << "FAIL " + QString::fromUtf8(e.what()); result = 1;}
    const QString output = qEnvironmentVariable("TEST_OUTPUT_DIR", QDir::currentPath());
    QDir().mkpath(output);
    QFile file(QDir(output).filePath("measurement-test.txt"));
    if (file.open(QIODevice::WriteOnly)) {
        QTextStream stream(&file); stream.setCodec("UTF-8"); stream << results.join('\n');
    }
    return result;
}

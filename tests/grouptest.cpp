#include "instrument.h"
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QScopeGuard>
#include <cstring>
#include <stdexcept>

namespace {
QByteArray reply;
QStringList operations;
bool shortWrite=false, readFailure=false, writeFailure=false, waitFailure=false, clearFailure=false;
bool errorQueryFailure=false;
QList<ViInt32> recoveryErrors;
ViInt64 reportedSize=-1;
ViStatus _VI_FUNC writeBytes(ViSession, ViInt64 size, ViByte bytes[], ViInt64* count) {
    operations << QString::fromLatin1(reinterpret_cast<char*>(bytes), static_cast<int>(size));
    *count=shortWrite ? size-1 : size;
    return writeFailure ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
ViStatus _VI_FUNC readBytes(ViSession, ViInt64 size, ViByte bytes[], ViInt64* count) {
    operations << "read";
    *count=reportedSize<0 ? reply.size() : reportedSize;
    if(reply.size()<size) std::memcpy(bytes,reply.constData(),static_cast<size_t>(reply.size()));
    return readFailure ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
ViStatus _VI_FUNC waitComplete(ViSession, ViInt32) {
    operations << "wait";
    return waitFailure ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
ViStatus _VI_FUNC clearIo(ViSession) {
    operations << "clear";
    return clearFailure ? KTB2900_ERROR_IO_GENERAL : VI_SUCCESS;
}
ViStatus _VI_FUNC queryError(ViSession,ViInt32* code,ViChar message[]) {
    operations << "error?";
    if(errorQueryFailure)return KTB2900_ERROR_IO_GENERAL;
    *code=recoveryErrors.isEmpty() ? 0 : recoveryErrors.takeFirst();
    const QByteArray text=*code==0 ? QByteArray("No error") :
        *code==-241 ? QByteArray("Hardware missing") : QByteArray("Query boundary error");
    std::strcpy(message,text.constData());
    return VI_SUCCESS;
}
class ShutdownAudit final : public Instrument {
public:
    bool outputEnabled(int channel) override {operations<<QString("read%1").arg(channel);return false;}
    void checkCompliance() override {operations<<"compliance";}
protected:
    void disableOutput(int channel) override {operations<<QString("off%1").arg(channel);}
};
}

int groupTest() {
    QStringList results;int result=0;
    ShutdownAudit instrument;
    // Inject only the raw transport. The production grouping methods and strict
    // validation run unchanged; this test cannot address or enable real hardware.
    instrument.session=1;
    instrument.viWrite=writeBytes;
    instrument.SystemReadBytes=readBytes;
    instrument.SystemWaitForOperationComplete=waitComplete;
    instrument.SystemClearIO=clearIo;
    instrument.error_query=queryError;
    const auto cleanup=qScopeGuard([&]{instrument.session=VI_NULL;});
    auto require=[&](bool ok,const QString& text) {
        if(!ok) throw std::runtime_error(text.toStdString());
        results << "PASS "+text;
    };
    auto rejects=[](const std::function<void()>& action) {
        try { action(); } catch(const std::exception&) {return true;} return false;
    };
    try {
        for(const QByteArray& response : {QByteArray("(@1),(@2)"),QByteArray("(@2),(@1)"),QByteArray(" (@1),\t(@2)\r\n")}) {
            reply=response;operations.clear();instrument.verifyIndependentChannels();
            require(operations==QStringList{":SYSTem:GROup:DEFine?","read"} && !instrument.rawQueryPending,
                "Accept complete independent groups without inserting another query: "+response.trimmed());
        }
        for(const QByteArray& response : {QByteArray("(@1"),QByteArray("(@1)"),QByteArray("(@1),(@2"),
                QByteArray("(@1,2)"),QByteArray("(@2,1)"),QByteArray("(@1),(@1)"),QByteArray("(@1),(@2),(@3)"),
                QByteArray("0"),QByteArray(),QByteArray("\"(@1),(@2)\""),QByteArray("(@1),(@2)\0garbage",17)}) {
            reply=response;operations.clear();
            require(rejects([&]{instrument.verifyIndependentChannels();}),
                "Reject incomplete, coupled or unexpected response (hex): "+response.toHex());
        }
        reply="(@1),(@2)";operations.clear();instrument.prepareIndependentChannels();
        require(operations==QStringList{":SYSTem:GROup:DEFine (@1),(@2)","wait",":SYSTem:GROup:DEFine?","read"},
            "Explicitly set independent groups, await completion and verify full readback before caller may enable CH1");
        reply="(@1,2)";
        require(rejects([&]{instrument.prepareIndependentChannels();}),"Refuse setup if grouping command did not take effect");
        reply="(@1),(@2)";
        shortWrite=true;operations.clear();
        require(rejects([&]{instrument.prepareIndependentChannels();}) && operations.size()==1,
            "Partial write aborts configuration without querying or enabling any channel");shortWrite=false;
        writeFailure=true;operations.clear();
        require(rejects([&]{instrument.verifyIndependentChannels();}) && operations.size()==1,
            "Write I/O failure aborts before reading");writeFailure=false;
        waitFailure=true;operations.clear();
        require(rejects([&]{instrument.prepareIndependentChannels();}) && operations.size()==2,
            "Configuration completion failure aborts before verification");waitFailure=false;
        readFailure=true;
        require(rejects([&]{instrument.verifyIndependentChannels();}),"Read I/O failure rejects monitoring");readFailure=false;
        reportedSize=256;
        require(rejects([&]{instrument.verifyIndependentChannels();}),"Full buffer is rejected as potentially truncated");
        reportedSize=999;
        require(rejects([&]{instrument.verifyIndependentChannels();}),"Oversized count rejected before accessing buffer");
        operations.clear();
        require(rejects([&]{instrument.writeScpi(":SYSTem:GROup:DEFine?\n");}) && operations.isEmpty(),
            "Reject caller-supplied terminator; IVI driver appends the single LF");
        reportedSize=-1;readFailure=true;
        require(rejects([&]{instrument.verifyIndependentChannels();}) && instrument.rawQueryPending,
            "Read timeout marks the I/O stream incomplete");readFailure=false;
        instrument.configured=true;operations.clear();
        require(instrument.stopOutputs().isEmpty() && !instrument.rawQueryPending
            && operations==QStringList{"clear","off2","off1","read2","read1"},
            "Timeout recovery clears I/O then shuts down and verifies both outputs without setpoint writes");
        instrument.rawQueryPending=true;instrument.configured=true;clearFailure=true;operations.clear();
        require(!instrument.stopOutputs().isEmpty()
            && operations==QStringList{"clear","off2","off1","read2","read1"},
            "Failed recovery is reported but still attempts every output OFF and verification");
        instrument.rawQueryPending=false;instrument.configured=false;clearFailure=false;
        instrument.simulated=true;
        recoveryErrors={-410,-420,-430,-440,0};operations.clear();
        instrument.recoverCommunication();
        require(operations==QStringList{"clear","error?","error?","error?","error?","error?","compliance"}
            && !instrument.rawQueryPending,
            "Recovery Device Clear drains stale query-boundary errors before checking output state");
        recoveryErrors={-241,0};operations.clear();
        require(rejects([&]{instrument.recoverCommunication();})
            && operations==QStringList{"clear","error?"},
            "Recovery never ignores a non-query-boundary instrument error");
        recoveryErrors={0};errorQueryFailure=true;operations.clear();instrument.rawQueryPending=false;
        require(rejects([&]{instrument.recoverCommunication();}) && instrument.rawQueryPending
            && operations==QStringList{"clear","error?"},
            "Error-queue I/O failure keeps the transaction marked incomplete for the next Device Clear");
        errorQueryFailure=false;instrument.rawQueryPending=false;
        clearFailure=true;operations.clear();
        require(rejects([&]{instrument.recoverCommunication();}) && instrument.rawQueryPending
            && operations==QStringList{"clear"},
            "Failed recovery Device Clear keeps the transaction marked incomplete for shutdown");
        clearFailure=false;instrument.rawQueryPending=false;
        results << "LIMIT: Raw transport injected; see group_driver_probe.py for installed-driver loopback verification.";
    } catch(const std::exception& e) {results<<"FAIL "+QString::fromUtf8(e.what());result=1;}
    const QString output=qEnvironmentVariable("TEST_OUTPUT_DIR",QDir::currentPath());QDir().mkpath(output);
    QFile file(QDir(output).filePath("group-test.txt"));
    if(file.open(QIODevice::WriteOnly)){QTextStream stream(&file);stream.setCodec("UTF-8");stream<<results.join('\n');}
    return result;
}

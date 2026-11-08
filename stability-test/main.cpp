#include "stability.h"
#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QMessageBox>
#include <QLockFile>

int main(int argc,char** argv) {
    QApplication app(argc,argv);
    app.setApplicationDisplayName("探头闭环控制上位机v1.0 · 离线测试");
    app.setApplicationVersion("1.0");
    app.setWindowIcon(QIcon(":/icons/probe-control.ico"));
    app.setFont(QFont("Microsoft YaHei UI",10));
    qRegisterMetaType<TestFrame>("TestFrame");
    if(app.arguments().contains("--self-test")) return stabilitySelfTest();
    for(const QString& sub: {"HisData"}) {
        if(!QDir(QCoreApplication::applicationDirPath()).mkpath(sub)) {
            QMessageBox::critical(nullptr,"无法创建测试目录","请将整个测试文件夹放到可写入的位置后运行。");return 1;
        }
    }
    QLockFile lock(QDir::temp().filePath("B2912C_OfflineStabilityTest.lock"));
    if(!lock.tryLock(0)) {
        QMessageBox::information(nullptr,"测试程序已打开","请使用已经打开的离线测试窗口。");return 1;
    }
    StabilityWindow window;window.show();
    return app.exec();
}

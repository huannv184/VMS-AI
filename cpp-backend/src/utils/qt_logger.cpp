#include "utils/qt_logger.h"
#include "utils/logger.h"
#include <QMessageLogContext>
#include <QString>

namespace vms {
namespace utils {

static void qtMessageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    const QByteArray utf8 = msg.toUtf8();
    std::string text(utf8.constData(), static_cast<size_t>(utf8.size()));
    const char* cat = ctx.category ? ctx.category : "qt";

    switch (type) {
    case QtDebugMsg:
        LOG_DEBUG("[{}] {}", cat, text);
        break;
    case QtInfoMsg:
        LOG_INFO("[{}] {}", cat, text);
        break;
    case QtWarningMsg:
        LOG_WARN("[{}] {}", cat, text);
        break;
    case QtCriticalMsg:
        LOG_ERROR("[{}] {}", cat, text);
        break;
    case QtFatalMsg:
        LOG_CRITICAL("[{}] {}", cat, text);
        break;
    }
}

void installQtLogBridge() {
    qInstallMessageHandler(qtMessageHandler);
}

} // namespace utils
} // namespace vms

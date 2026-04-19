#pragma once

#include <QtGlobal>

namespace vms {
namespace utils {

/**
 * @brief Install a Qt message handler that forwards qDebug/qWarning/qCritical
 *        into the existing spdlog-based LOG_* pipeline.
 *
 * Call once after Logger::init(). After installation, Qt internal diagnostics
 * (e.g. QSqlDatabase warnings, QWebSocket errors) appear in the same rotating
 * log file as application messages.
 */
void installQtLogBridge();

} // namespace utils
} // namespace vms

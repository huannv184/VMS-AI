#include "database/audit_repository.h"
#include "database/db_manager.h"
#include "utils/logger.h"

#include <QSqlQuery>
#include <QSqlDatabase>
#include <QSqlError>
#include <QVariant>

namespace vms {
namespace database {

bool AuditRepository::insertLog(int user_id, const std::string& action, const std::string& details) {
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return false;

    QSqlQuery query(db);
    query.prepare("INSERT INTO audit_logs (user_id, action, details, created_at) VALUES (?, ?, ?, ?)");

    long long created_at = static_cast<long long>(time(nullptr));
    query.bindValue(0, user_id);
    query.bindValue(1, QString::fromStdString(action));
    query.bindValue(2, QString::fromStdString(details));
    query.bindValue(3, created_at);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute insertLog statement: {}", query.lastError().text().toStdString());
        return false;
    }
    return true;
}

std::vector<AuditLog> AuditRepository::getLogs(int limit, int offset) {
    std::vector<AuditLog> logs;
    QSqlDatabase db = DbManager::getInstance().getThreadConnection();
    if (!db.isOpen()) return logs;

    QSqlQuery query(db);
    query.prepare("SELECT id, user_id, action, details, created_at FROM audit_logs ORDER BY created_at DESC LIMIT ? OFFSET ?");
    query.bindValue(0, limit);
    query.bindValue(1, offset);

    if (!query.exec()) {
        LOG_ERROR("Failed to execute getLogs statement: {}", query.lastError().text().toStdString());
        return logs;
    }

    while (query.next()) {
        AuditLog log;
        log.id = query.value(0).toInt();
        log.user_id = query.value(1).toInt();
        log.action = query.value(2).isNull() ? "" : query.value(2).toString().toStdString();
        log.details = query.value(3).isNull() ? "" : query.value(3).toString().toStdString();
        log.created_at = query.value(4).toLongLong();
        logs.push_back(log);
    }
    return logs;
}

} // namespace database
} // namespace vms

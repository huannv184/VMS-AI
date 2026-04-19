#pragma once

#include <QObject>

namespace vms {
namespace utils {

/**
 * @brief Singleton QObject that emits configReloaded() when the YAML
 *        configuration is reparsed at runtime.
 *
 * QThread workers (Phase D) can connect to this signal to pick up
 * configuration changes without polling.
 */
class ConfigNotifier : public QObject {
    Q_OBJECT

public:
    static ConfigNotifier& getInstance();

    /// Call after Config::loadFromFile() succeeds to notify listeners.
    void notifyReload();

Q_SIGNALS:
    void configReloaded();

private:
    ConfigNotifier() = default;
    ~ConfigNotifier() override = default;
    ConfigNotifier(const ConfigNotifier&) = delete;
    ConfigNotifier& operator=(const ConfigNotifier&) = delete;
};

} // namespace utils
} // namespace vms

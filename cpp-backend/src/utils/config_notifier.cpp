#include "utils/config_notifier.h"

namespace vms {
namespace utils {

ConfigNotifier& ConfigNotifier::getInstance() {
    static ConfigNotifier instance;
    return instance;
}

void ConfigNotifier::notifyReload() {
    Q_EMIT configReloaded();
}

} // namespace utils
} // namespace vms

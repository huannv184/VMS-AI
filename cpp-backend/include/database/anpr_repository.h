#pragma once

#include "models.h"
#include <vector>
#include <optional>
#include <string>

namespace vms {
namespace database {

class ANPRRepository {
public:
    ANPRRepository() = default;
    ~ANPRRepository() = default;

    std::vector<LicensePlate> getAllPlates(int limit = 100, int offset = 0);
    int getPlateCount();
    std::vector<LicensePlate> getPlatesByCamera(int camera_id, int limit = 50);
    std::optional<LicensePlate> getPlateById(int id);
    bool insertPlate(const LicensePlate& plate);
    bool deletePlate(int id);
    bool clearAllPlates();
    std::vector<LicensePlate> searchPlates(const std::string& query);
};

} // namespace database
} // namespace vms

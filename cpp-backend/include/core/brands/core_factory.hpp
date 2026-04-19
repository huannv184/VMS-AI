#pragma once
#include "brand_core.hpp"
#include "hanwha_core.hpp"
#include "axis_core.hpp"
#include "bosch_core.hpp"
#include "pelco_core.hpp"
#include "hikvision_core.hpp"
#include "dahua_core.hpp"
#include "uniview_core.hpp"
#include "onvif_core.hpp"
#include "milesight_core.hpp"
#include "reolink_core.hpp"

namespace vms {
namespace core {
namespace brands {

class CoreFactory {
public:
    /**
     * @brief Get the appropriate Core implementation for a brand
     */
    static std::shared_ptr<BrandCore> getCore(CameraDiscovery::Brand brand) {
        // --- Tier-2 Mapping Registry ---
        // Map OEM/Local brands to their Tier-1 Cores
        switch (brand) {
            // Dahua Core Mapping
            case CameraDiscovery::Brand::Dahua:
            case CameraDiscovery::Brand::CPPlus:
            case CameraDiscovery::Brand::Amcrest:
            case CameraDiscovery::Brand::Lorex:
            case CameraDiscovery::Brand::QSee:
            case CameraDiscovery::Brand::ICRealtime:
            case CameraDiscovery::Brand::Honeywell:
            case CameraDiscovery::Brand::Panasonic:
            case CameraDiscovery::Brand::FLIR:
            case CameraDiscovery::Brand::Tyco:
            case CameraDiscovery::Brand::Imou:
            case CameraDiscovery::Brand::Kbvision:
            case CameraDiscovery::Brand::Vantech:
            case CameraDiscovery::Brand::Questek:
                return std::make_shared<DahuaCore>();

            // Hikvision Core Mapping
            case CameraDiscovery::Brand::Hikvision:
            case CameraDiscovery::Brand::Annke:
            case CameraDiscovery::Brand::LTS:
            case CameraDiscovery::Brand::LaView:
            case CameraDiscovery::Brand::Swann:
            case CameraDiscovery::Brand::Trendnet:
            case CameraDiscovery::Brand::Epcom:
            case CameraDiscovery::Brand::GWSecurity:
            case CameraDiscovery::Brand::Hunt:
            case CameraDiscovery::Brand::WBox:
            case CameraDiscovery::Brand::Ezviz:
                return std::make_shared<HikvisionCore>();

            // Uniview Core Mapping
            case CameraDiscovery::Brand::Uniview:
            case CameraDiscovery::Brand::GeoVision:
            case CameraDiscovery::Brand::Luma:
            case CameraDiscovery::Brand::Speco:
                return std::make_shared<UniviewCore>();

            // Tier-1 Independent Cores
            case CameraDiscovery::Brand::Hanwha:   return std::make_shared<HanwhaCore>();
            case CameraDiscovery::Brand::Axis:      return std::make_shared<AxisCore>();
            case CameraDiscovery::Brand::Bosch:     return std::make_shared<BoschCore>();
            case CameraDiscovery::Brand::Pelco:     return std::make_shared<PelcoCore>();
            case CameraDiscovery::Brand::Milesight: return std::make_shared<MilesightCore>();
            case CameraDiscovery::Brand::Reolink:   return std::make_shared<ReolinkCore>();
            
            // Generic Fallback
            case CameraDiscovery::Brand::ONVIF:   return std::make_shared<ONVIFCore>();

            default:
                return nullptr;
        }
    }
};

} // namespace brands
} // namespace core
} // namespace vms

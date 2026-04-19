#pragma once

#include "server/vms_app.h"
#include "middleware/auth_middleware.h"
#include <string>
#include <vector>

namespace vms {
namespace api {

struct MaskRegion {
    int x;
    int y;
    int width;
    int height;
    std::string type;  // "blur" or "pixelate"
    int strength;
};

struct ExportJob {
    std::string jobId;
    std::string recordingId;
    std::vector<MaskRegion> masks;
    std::string format;
    std::string status;  // "processing", "done", "failed"
    std::string outputPath;
    std::string errorMessage;
};

class ExportController {
public:
    static void registerRoutes(vms::server::VmsApp& app, vms::middleware::AuthMiddleware& auth);
    static void shutdown();
    
private:
    static std::string generateJobId();
    static std::string buildFFmpegCommand(
        const std::string& inputPath,
        const std::string& outputPath,
        const std::vector<MaskRegion>& masks
    );
    static void processExportJob(const ExportJob& job);
};

} // namespace api
} // namespace vms

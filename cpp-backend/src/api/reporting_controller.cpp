#include "api/reporting_controller.h"
#include "database/audit_repository.h"
#include "database/db_manager.h"
#include "utils/api_utils.h"
#include "utils/logger.h"
#include <nlohmann/json.hpp>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace vms {
namespace api {

void ReportingController::registerRoutes(vms::server::VmsApp& app) {

    // GET /api/analytics/export
    CROW_ROUTE(app, "/api/analytics/export")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);

        std::string type = req.url_params.get("type") ? req.url_params.get("type") : "traffic";
        int camera_id = req.url_params.get("camera_id") ? std::stoi(req.url_params.get("camera_id")) : -1;

        std::string csv_content;
        std::string filename = "report_" + type + ".csv";

        if (type == "traffic") {
            vms::database::TrafficRepository repo;
            auto data = repo.getCounts(camera_id, 0, 0, 1000);
            csv_content = ReportingController::formatTrafficToCsv(data);
        } else if (type == "events") {
            vms::database::EventRepository repo;
            auto data = repo.getEvents(camera_id, 1000, 0);
            csv_content = ReportingController::formatEventsToCsv(data);
        } else {
            return ApiUtils::createErrorResponse("Invalid export type", 400, origin);
        }

        // Audit: CSV export is a PII bulk-egress action — log who & what.
        if (ctx.user.has_value()) {
            try {
                vms::database::AuditRepository audit;
                audit.insertLog(ctx.user->id, "ANALYTICS_EXPORT_CSV",
                                "type=" + type + " camera_id=" + std::to_string(camera_id)
                                + " bytes=" + std::to_string(csv_content.size()));
            } catch (...) { /* never break the download on audit failure */ }
        }

        auto res = crow::response(csv_content);
        res.add_header("Content-Type", "text/csv; charset=utf-8");
        res.add_header("Content-Disposition", "attachment; filename=" + filename);
        res.add_header("Access-Control-Allow-Origin", origin);
        return res;
    });

    // GET /api/analytics/heatmap/<int>
    CROW_ROUTE(app, "/api/analytics/heatmap/<int>")
    .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Options)
    ([&app](const crow::request& req, int cam_id) {
        std::string origin = ApiUtils::resolveCorsOrigin(req);
        if (req.method == crow::HTTPMethod::Options) return ApiUtils::createResponse(json::object(), 204, origin);
        auto& ctx = app.get_context<vms::middleware::AuthMiddleware>(req);
        if (auto err = ApiUtils::requirePermission(ctx, Permission::ANALYTICS_READ, origin)) return std::move(*err);
        
        try {
            vms::database::EventRepository repo;
            auto events = repo.getEvents(cam_id, 500, 0);
            
            json points = json::array();
            for (const auto& ev : events) {
                try {
                    auto meta = json::parse(ev.metadata_json);
                    // Extract center points from detections if available
                    if (meta.contains("detections") && meta["detections"].is_array()) {
                        for (const auto& det : meta["detections"]) {
                             if (det.contains("x") && det.contains("y")) {
                                 points.push_back({{"x", det["x"]}, {"y", det["y"]}, {"value", 1}});
                             }
                        }
                    } else if (meta.contains("x") && meta.contains("y")) {
                         points.push_back({{"x", meta["x"]}, {"y", meta["y"]}, {"value", 1}});
                    }
                } catch (...) {}
            }
            
            return ApiUtils::createResponse({{"camera_id", cam_id}, {"points", points}}, 200, origin);
        } catch (const std::exception& e) {
            return ApiUtils::createSafeError(e, 500, origin);
        }
    });
}

std::string ReportingController::formatTrafficToCsv(const std::vector<vms::TrafficCount>& data) {
    std::stringstream ss;
    ss << "\xEF\xBB\xBF"; // UTF-8 BOM for Excel
    ss << "ID,Camera ID,Vùng (ROI),Hướng,Loại phương tiện,Số lượng,Bắt đầu,Kết thúc,Ngày tạo\n";
    
    for (const auto& row : data) {
        char start_time[32], end_time[32], created_at[32];
        std::strftime(start_time, sizeof(start_time), "%Y-%m-%d %H:%M:%S", std::localtime(&row.period_start));
        std::strftime(end_time, sizeof(end_time), "%Y-%m-%d %H:%M:%S", std::localtime(&row.period_end));
        std::strftime(created_at, sizeof(created_at), "%Y-%m-%d %H:%M:%S", std::localtime(&row.created_at));

        ss << row.id << "," 
           << row.camera_id << ","
           << row.roi_id << ","
           << row.direction << ","
           << row.vehicle_type << ","
           << row.count << ","
           << start_time << ","
           << end_time << ","
           << created_at << "\n";
    }
    return ss.str();
}

std::string ReportingController::formatEventsToCsv(const std::vector<vms::Event>& data) {
    std::stringstream ss;
    ss << "\xEF\xBB\xBF"; // UTF-8 BOM
    ss << "ID,Camera ID,Loại sự kiện,Mô tả,Thời gian,Đường dẫn video\n";
    
    for (const auto& row : data) {
        char ts[32];
        std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&row.timestamp));

        ss << row.id << "," 
           << row.camera_id << ","
           << row.event_type << ","
           << "\"" << row.description << "\","
           << ts << ","
           << row.video_path << "\n";
    }
    return ss.str();
}

} // namespace api
} // namespace vms

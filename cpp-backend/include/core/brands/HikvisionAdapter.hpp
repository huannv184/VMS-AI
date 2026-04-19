#pragma once
#include "core/brands/camera_adapter_types.hpp"

namespace vms {

class HikvisionAdapter : public BaseCameraAdapter {
public:
    explicit HikvisionAdapter(const CameraConfig& cfg) : BaseCameraAdapter(cfg) {}

protected:
    std::string getProbeEndpoint() const override { return "/ISAPI/System/deviceInfo"; }
    std::string getDeviceInfoEndpoint() const override { return "/ISAPI/System/deviceInfo"; }
    bool useDigestAuth() const override { return true; }
    std::string getBrandName() const override { return "Hikvision"; }
    std::string getModelFieldName() const override { return "model"; }
    std::string getSerialFieldName() const override { return "serialNumber"; }
    std::string getFirmwareFieldName() const override { return "firmwareVersion"; }

public:
    bool ptzControl(const PtzCommand& cmd) override {
        int pan = 0, tilt = 0, zoom = 0;
        int spd = static_cast<int>(cmd.panSpeed * 100);
        if (spd < 1) spd = 10;
        if (spd > 100) spd = 100;

        switch (cmd.move) {
            case PtzCommand::Move::Left:    pan = -spd; break;
            case PtzCommand::Move::Right:   pan = spd; break;
            case PtzCommand::Move::Up:      tilt = spd; break;
            case PtzCommand::Move::Down:    tilt = -spd; break;
            case PtzCommand::Move::ZoomIn:  zoom = spd; break;
            case PtzCommand::Move::ZoomOut: zoom = -spd; break;
            default: break;
        }

        std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<PTZData version=\"2.0\" xmlns=\"http://www.isapi.org/ver20/XMLSchema\">\n"
            "  <pan>" + std::to_string(pan) + "</pan>\n"
            "  <tilt>" + std::to_string(tilt) + "</tilt>\n"
            "  <zoom>" + std::to_string(zoom) + "</zoom>\n"
            "</PTZData>";

        return !rawPut("/ISAPI/PTZCtrl/channels/1/continuous", xml).empty();
    }

    void startEventSubscription(std::function<void(const CameraEvent&)> callback, void* /*ctx*/) override {
        // Hikvision ISAPI Alert Stream: long-poll on /ISAPI/Event/notification/alertStream
        // The camera pushes multipart/form-data chunks; each chunk is an XML event payload.
        std::string buffer;
        rawStreamGet("/ISAPI/Event/notification/alertStream",
            [callback, &buffer](const std::string& chunk) -> bool {
                buffer += chunk;
                // Each ISAPI event is delimited by "--boundary" or empty line after XML
                std::string::size_type pos = 0;
                while ((pos = buffer.find("</EventNotificationAlert>")) != std::string::npos) {
                    std::string xml = buffer.substr(0, pos + 25); // include closing tag
                    buffer.erase(0, pos + 25);

                    CameraEvent ev;
                    // Parse eventType field
                    auto extractTag = [&](const std::string& tag) -> std::string {
                        std::string open = "<" + tag + ">";
                        std::string close = "</" + tag + ">";
                        auto s = xml.find(open);
                        auto e = (s != std::string::npos) ? xml.find(close, s) : std::string::npos;
                        if (s == std::string::npos || e == std::string::npos) return "";
                        return xml.substr(s + open.size(), e - s - open.size());
                    };

                    ev.typeRaw = extractTag("eventType");
                    ev.active  = extractTag("eventState") == "active";
                    try { ev.channel = std::stoi(extractTag("channelID")); } catch (...) { ev.channel = 1; }

                    if      (ev.typeRaw == "VMD")             ev.type = CameraEvent::Type::Motion;
                    else if (ev.typeRaw == "linedetection")   ev.type = CameraEvent::Type::LineCrossing;
                    else if (ev.typeRaw == "fielddetection")  ev.type = CameraEvent::Type::Intrusion;
                    else if (ev.typeRaw == "facedetection")   ev.type = CameraEvent::Type::FaceDetection;
                    else if (ev.typeRaw == "shelteralarm")    ev.type = CameraEvent::Type::TamperDetection;
                    else                                       ev.type = CameraEvent::Type::Unknown;

                    callback(ev);
                }
                return true; // keep streaming
            });
    }
};

} // namespace vms

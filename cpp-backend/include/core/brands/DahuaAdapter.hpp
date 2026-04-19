#pragma once
#include "core/brands/camera_adapter_types.hpp"

namespace vms {

class DahuaAdapter : public BaseCameraAdapter {
public:
    explicit DahuaAdapter(const CameraConfig& cfg) : BaseCameraAdapter(cfg) {}

protected:
    std::string getProbeEndpoint() const override { return "/cgi-bin/magicBox.cgi?action=getDeviceType"; }
    std::string getDeviceInfoEndpoint() const override { return "/cgi-bin/magicBox.cgi?action=getSystemInfo"; }
    std::string getBrandName() const override { return "Dahua"; }
    std::string getModelFieldName() const override { return "deviceType"; }
    std::string getSerialFieldName() const override { return "serialNumber"; }
    std::string getFirmwareFieldName() const override { return "softwareVersion"; }

public:
    bool ptzControl(const PtzCommand& cmd) override {
        std::string action;
        switch (cmd.move) {
            case PtzCommand::Move::Up:      action = "Up"; break;
            case PtzCommand::Move::Down:    action = "Down"; break;
            case PtzCommand::Move::Left:    action = "Left"; break;
            case PtzCommand::Move::Right:   action = "Right"; break;
            case PtzCommand::Move::ZoomIn:  action = "ZoomTele"; break;
            case PtzCommand::Move::ZoomOut: action = "ZoomWide"; break;
            default: action = "stop"; break;
        }

        if (cmd.action == PtzCommand::Action::Stop) {
            return !rawGet("/cgi-bin/ptz.cgi?action=stop&channel=0&code=" + action).empty();
        }

        int spd = static_cast<int>(cmd.panSpeed * 8);
        if (spd < 1) spd = 1;
        if (spd > 8) spd = 8;

        std::string url = "/cgi-bin/ptz.cgi?action=start&channel=0&code=" + action +
                          "&arg1=0&arg2=" + std::to_string(spd) + "&arg3=0";
        return !rawGet(url).empty();
    }

    void startEventSubscription(std::function<void(const CameraEvent&)> callback, void* /*ctx*/) override {
        // Dahua event manager subscription - simplified
        (void)callback;
    }

protected:
    bool useDigestAuth() const override { return true; }
};

} // namespace vms

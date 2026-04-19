#pragma once
#include "core/brands/camera_adapter_types.hpp"

namespace vms {

class HanwhaAdapter : public BaseCameraAdapter {
public:
    explicit HanwhaAdapter(const CameraConfig& cfg) : BaseCameraAdapter(cfg) {}

protected:
    std::string getProbeEndpoint() const override { return "/stw-cgi/system.cgi?msubmenu=deviceinfo&action=view"; }
    std::string getDeviceInfoEndpoint() const override { return "/stw-cgi/system.cgi?msubmenu=deviceinfo&action=view"; }
    bool useDigestAuth() const override { return true; }
    std::string getBrandName() const override { return "Hanwha"; }
    std::string getModelFieldName() const override { return "Model"; }
    std::string getSerialFieldName() const override { return "SerialNumber"; }
    std::string getFirmwareFieldName() const override { return "FirmwareVersion"; }

public:
    bool ptzControl(const PtzCommand& cmd) override {
        std::string action;
        switch (cmd.move) {
            case PtzCommand::Move::Up:      action = "Mode=ContinuousTilt&TiltSpeed=" + std::to_string((int)(cmd.tiltSpeed * 5)); break;
            case PtzCommand::Move::Down:    action = "Mode=ContinuousTilt&TiltSpeed=-" + std::to_string((int)(cmd.tiltSpeed * 5)); break;
            case PtzCommand::Move::Left:    action = "Mode=ContinuousPan&PanSpeed=-" + std::to_string((int)(cmd.panSpeed * 5)); break;
            case PtzCommand::Move::Right:   action = "Mode=ContinuousPan&PanSpeed=" + std::to_string((int)(cmd.panSpeed * 5)); break;
            case PtzCommand::Move::ZoomIn:  action = "Mode=ContinuousZoom&ZoomSpeed=" + std::to_string((int)(cmd.zoomSpeed * 5)); break;
            case PtzCommand::Move::ZoomOut: action = "Mode=ContinuousZoom&ZoomSpeed=-" + std::to_string((int)(cmd.zoomSpeed * 5)); break;
            default: action = "Mode=Stop"; break;
        }

        if (cmd.action == PtzCommand::Action::Stop) action = "Mode=Stop";

        return !rawGet("/stw-cgi/ptzcontrol.cgi?msubmenu=continuous&action=control&" + action).empty();
    }

    void startEventSubscription(std::function<void(const CameraEvent&)> callback, void* /*ctx*/) override {
        (void)callback;
    }
};

} // namespace vms

#pragma once
#include "core/brands/camera_adapter_types.hpp"

namespace vms {

class AxisAdapter : public BaseCameraAdapter {
public:
    explicit AxisAdapter(const CameraConfig& cfg) : BaseCameraAdapter(cfg) {}

protected:
    std::string getProbeEndpoint() const override { return "/axis-cgi/basicdeviceinfo.cgi"; }
    std::string getDeviceInfoEndpoint() const override { return "/axis-cgi/basicdeviceinfo.cgi"; }
    bool useDigestAuth() const override { return true; }
    std::string getBrandName() const override { return "Axis"; }
    std::string getModelFieldName() const override { return "ProdNbr"; }
    std::string getSerialFieldName() const override { return "SerialNumber"; }
    std::string getFirmwareFieldName() const override { return "Version"; }

public:
    bool ptzControl(const PtzCommand& cmd) override {
        std::string params;
        int spd = static_cast<int>(cmd.panSpeed * 100);
        if (spd < 1) spd = 10;
        if (spd > 100) spd = 100;

        switch (cmd.move) {
            case PtzCommand::Move::Up:      params = "move=up&speed=" + std::to_string(spd); break;
            case PtzCommand::Move::Down:    params = "move=down&speed=" + std::to_string(spd); break;
            case PtzCommand::Move::Left:    params = "move=left&speed=" + std::to_string(spd); break;
            case PtzCommand::Move::Right:   params = "move=right&speed=" + std::to_string(spd); break;
            case PtzCommand::Move::ZoomIn:  params = "rzoom=+" + std::to_string(spd); break;
            case PtzCommand::Move::ZoomOut: params = "rzoom=-" + std::to_string(spd); break;
            default: params = "move=stop"; break;
        }

        if (cmd.action == PtzCommand::Action::Stop) params = "move=stop";

        return !rawGet("/axis-cgi/com/ptz.cgi?" + params).empty();
    }

    void startEventSubscription(std::function<void(const CameraEvent&)> callback, void* /*ctx*/) override {
        (void)callback;
    }
};

} // namespace vms

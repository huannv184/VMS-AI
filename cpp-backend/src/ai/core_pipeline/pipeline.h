#pragma once
#include "inference/bounding_box.h"
#include <memory>
#include <vector>
#include <string>

// Forward declaration
namespace inference {
    class TrtEngine;
}

namespace inference {

class Pipeline {
public:
    explicit Pipeline(const std::string& engine_path);
    ~Pipeline() = default;

    std::vector<BoundingBox> run(const std::vector<float>& input);

private:
    std::unique_ptr<TrtEngine> engine_;
};

} // namespace inference
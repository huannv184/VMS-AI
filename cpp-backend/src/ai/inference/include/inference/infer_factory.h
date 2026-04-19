#pragma once
#include <memory>
#include "infer_base.h"
#include "dummy.h"

namespace inference {

std::unique_ptr<InferBase> createInfer(const InferConfig& cfg);

} // namespace inference

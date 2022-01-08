#pragma once

#include "../common/CommandChannel.hpp"
#include "../../base/DataChannel.hpp"

#include <THPP/THPP.h>

#include <memory>

namespace thd { namespace worker {
extern std::unique_ptr<WorkerCommandChannel> workerCommandChannel;
extern std::unordered_map<object_id_type, std::unique_ptr<thpp::Tensor>>
  workerTensors;
extern std::unordered_map<object_id_type, std::unique_ptr<thpp::Storage>>
  workerStorages;
extern std::unordered_map<object_id_type, std::unique_ptr<thpp::Generator>>
  workerGenerators;
}} // namespace worker, thd

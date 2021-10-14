/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright(c) 2021 Open Networking Foundation
 */

#include "double_buffer_flagger.h"

// #include "../core/utils/common.h"

/*----------------------------------------------------------------------------------*/
const Commands DoubleBufferFlagger::cmds = {
    {"set", "DoubleBufferCommandSetNewFlagValueArg",
     MODULE_CMD_FUNC(&DoubleBufferFlagger::CommandSetNewFlagValue),
     Command::THREAD_SAFE},
};
/*----------------------------------------------------------------------------------*/
CommandResponse DoubleBufferFlagger::Init(
    const bess::pb::DoubleBufferFlaggerArg &arg) {
  const std::lock_guard<std::mutex> lock(mutex_);

  if (arg.attr_name().empty()) {
    return CommandFailure(EINVAL, "invalid metadata name");
  }

  if (!arg.size() || arg.size() > kMaxAttributeSize) {
    return CommandFailure(EINVAL, "invalid metadata size");
  }

  using AccessMode = bess::metadata::Attribute::AccessMode;
  flag_attr_id_ =
      AddMetadataAttr(arg.attr_name(), sizeof(uint64_t), AccessMode::kWrite);
  if (flag_attr_id_ < 0)
    return CommandFailure(EINVAL, "invalid metadata declaration");

  current_flag_value_ = arg.value();

  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
void DoubleBufferFlagger::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (int i = 0; i < batch->cnt(); ++i) {
      set_attr<uint64_t>(this, flag_attr_id_, batch->pkts()[i],
                         current_flag_value_);
    }
  }

  RunNextModule(ctx, batch);
}
/*----------------------------------------------------------------------------------*/
// command module flag set DoubleBufferCommandSetNewFlagValueArg {'new_flag': 12}
CommandResponse DoubleBufferFlagger::CommandSetNewFlagValue(
    const bess::pb::DoubleBufferCommandSetNewFlagValueArg &arg) {
  const std::lock_guard<std::mutex> lock(mutex_);
  current_flag_value_ = arg.new_flag();

  return CommandSuccess();
}
/*----------------------------------------------------------------------------------*/
ADD_MODULE(DoubleBufferFlagger, "double_buffer_flag",
           "Sets a flag attribute for double buffering");

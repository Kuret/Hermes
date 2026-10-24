/**
 * @file virtualhid_gamepad.h
 * @brief Gamepad translation for the Linux libvirtualhid backend.
 */
#pragma once

#include "virtualhid_common.h"

namespace platf::gamepad {
  int alloc(input_raw_t *raw, const gamepad_id_t &id, const gamepad_arrival_t &metadata, feedback_queue_t feedback_queue);
  void free(input_raw_t *raw, int index);
  void update(input_raw_t *raw, int index, const gamepad_state_t &state);
  void touch(input_raw_t *raw, const gamepad_touch_t &touch);
  void motion(input_raw_t *raw, const gamepad_motion_t &motion);
  void battery(input_raw_t *raw, const gamepad_battery_t &battery);
  std::vector<supported_gamepad_t> &supported_gamepads(input_t *input);
}  // namespace platf::gamepad

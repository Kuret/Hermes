/**
 * @file virtualhid_pen_touch.h
 * @brief Experimental per-client pen/touch adapter using MrOz59/libvirtualhid.
 *
 * Only built when HERMES_LVH_PEN_TOUCH is enabled. GameStream packet handling
 * remains in Hermes; libvirtualhid owns the local Linux virtual devices.
 */
#pragma once

#include <libvirtualhid/libvirtualhid.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include "src/platform/common.h"

namespace platf::virtualhid {

  struct client_devices_t {
    explicit client_devices_t(const std::string &session_tag);

    std::unique_ptr<lvh::Runtime> runtime;
    std::unique_ptr<lvh::Touchscreen> touch;
    std::unique_ptr<lvh::PenTablet> pen;

    // Both sets/states are per-client, never shared across streams.
    std::unordered_set<std::int32_t> active_contacts;
    bool pen_touching = false;
    std::uint8_t pen_buttons = 0;
  };

  void touch_update(client_devices_t &devices, const touch_port_t &port, const touch_input_t &touch);
  void pen_update(client_devices_t &devices, const touch_port_t &port, const pen_input_t &pen);
}

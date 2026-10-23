/**
 * @file virtualhid_pen_touch.cpp
 * @brief Moonlight pen/touch -> our libvirtualhid fork, Linux uinput.
 */
#include "virtualhid_pen_touch.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "src/logging.h"

namespace platf::virtualhid {
  namespace {
    constexpr std::uint16_t hermes_vendor = 0xBEEF;
    constexpr std::uint16_t hermes_product = 0xDEAD;

    // Until the full-desktop mapping is migrated, preserve the exact normalized
    // coordinate convention used by Hermes' existing inputtino backend.
    float position(float normalized, int offset, int span) {
      if (!std::isfinite(normalized)) {
        return 0.0f;
      }
      const float value = normalized + (span > 0 ? static_cast<float>(offset) / span : 0.0f);
      return std::clamp(value, 0.0f, 1.0f);
    }

    float analog(float value) {
      return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 0.0f;
    }

    int orientation(short rotation) {
      if (rotation == LI_ROT_UNKNOWN) {
        return 0;
      }
      int degrees = static_cast<int>(rotation) % 360;
      if (degrees < 0) degrees += 360;
      if (degrees > 90 && degrees < 270) degrees = 180 - degrees;
      if (degrees > 90) degrees -= 360;
      return std::clamp(degrees, -90, 90);
    }

    void log_failure(const char *operation, const lvh::OperationStatus &result) {
      if (!result.ok()) {
        BOOST_LOG(warning) << "[libvirtualhid] " << operation << ": " << result.message();
      }
    }

    lvh::DeviceProfile with_hermes_identity(lvh::DeviceProfile profile,
                                            const std::string &session_tag,
                                            const char *display_name) {
      profile.vendor_id = hermes_vendor;
      profile.product_id = hermes_product;
      profile.version = 0x111;
      profile.name = session_tag.empty() ? display_name : "Hermes Session Input";
      profile.linux_phys = session_tag;
      return profile;
    }
  }

  client_devices_t::client_devices_t(const std::string &session_tag) {
    lvh::RuntimeOptions options;
    // The libvirtualhid default is the *fake* backend. Never use that for a
    // stream: it appears to accept events without creating Linux devices.
    options.backend = lvh::BackendKind::platform_default;
    options.log_callback = [](lvh::LogLevel level, const std::string &message) {
      if (level == lvh::LogLevel::error || level == lvh::LogLevel::warning) {
        BOOST_LOG(warning) << "[libvirtualhid] " << message;
      } else {
        BOOST_LOG(debug) << "[libvirtualhid] " << message;
      }
    };
    runtime = lvh::Runtime::create(std::move(options));
    if (!runtime) {
      BOOST_LOG(error) << "[libvirtualhid] Failed to create Linux HID runtime";
      return;
    }

    lvh::CreateTouchscreenOptions touch_options;
    touch_options.profile = with_hermes_identity(lvh::profiles::touchscreen(),
                                                 session_tag, "Hermes Virtual Touch");
    touch_options.stable_id = session_tag + "-touch";
    auto created_touch = runtime->create_touchscreen(touch_options);
    if (created_touch) {
      touch = std::move(created_touch.touchscreen);
    } else {
      BOOST_LOG(warning) << "[libvirtualhid] Touchscreen unavailable: "
                         << created_touch.status.message();
    }

    lvh::CreatePenTabletOptions pen_options;
    pen_options.profile = with_hermes_identity(lvh::profiles::pen_tablet(),
                                               session_tag, "Hermes Virtual Pen");
    pen_options.stable_id = session_tag + "-pen";
    auto created_pen = runtime->create_pen_tablet(pen_options);
    if (created_pen) {
      pen = std::move(created_pen.pen_tablet);
    } else {
      BOOST_LOG(warning) << "[libvirtualhid] Pen tablet unavailable: "
                         << created_pen.status.message();
    }
  }

  void touch_update(client_devices_t &devices, const touch_port_t &port, const touch_input_t &event) {
    if (!devices.touch) {
      return;
    }
    const auto id = static_cast<std::int32_t>(event.pointerId);
    switch (event.eventType) {
      case LI_TOUCH_EVENT_HOVER:
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE: {
        lvh::TouchContact contact;
        contact.id = id;
        contact.x = position(event.x, port.offset_x, port.width);
        contact.y = position(event.y, port.offset_y, port.height);
        contact.pressure = analog(event.pressureOrDistance);
        contact.orientation = orientation(event.rotation);
        contact.touching = event.eventType != LI_TOUCH_EVENT_HOVER;
        log_failure("touch update", devices.touch->place_contact(contact));
        devices.active_contacts.insert(id);
        return;
      }
      case LI_TOUCH_EVENT_UP:
        log_failure("touch release", devices.touch->release_contact(id));
        devices.active_contacts.erase(id);
        return;
      case LI_TOUCH_EVENT_CANCEL:
        log_failure("touch cancel", devices.touch->cancel_contact(id));
        devices.active_contacts.erase(id);
        return;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        log_failure("touch leave", devices.touch->leave_contact(id));
        devices.active_contacts.erase(id);
        return;
      case LI_TOUCH_EVENT_CANCEL_ALL: {
        // Inputtino previously had no implementation for CANCEL_ALL.
        for (const auto active : devices.active_contacts) {
          log_failure("touch cancel all", devices.touch->cancel_contact(active));
        }
        devices.active_contacts.clear();
        return;
      }
      default:
        return;
    }
  }

  void pen_update(client_devices_t &devices, const touch_port_t &port, const pen_input_t &event) {
    if (!devices.pen) {
      return;
    }

    // Buttons are independent of contact state, so a barrel-button press
    // cannot synthesize a pen-up while the user is drawing.
    log_failure("pen primary", devices.pen->button(lvh::PenButton::primary,
                      (event.penButtons & LI_PEN_BUTTON_PRIMARY) != 0));
    log_failure("pen secondary", devices.pen->button(lvh::PenButton::secondary,
                      (event.penButtons & LI_PEN_BUTTON_SECONDARY) != 0));
    log_failure("pen tertiary", devices.pen->button(lvh::PenButton::tertiary,
                      (event.penButtons & LI_PEN_BUTTON_TERTIARY) != 0));

    if (event.eventType == LI_TOUCH_EVENT_BUTTON_ONLY) {
      return;
    }

    lvh::PenToolState tool;
    switch (event.toolType) {
      case LI_TOOL_TYPE_PEN: tool.tool = lvh::PenToolType::pen; break;
      case LI_TOOL_TYPE_ERASER: tool.tool = lvh::PenToolType::eraser; break;
      default: tool.tool = lvh::PenToolType::unchanged; break;
    }
    tool.x = position(event.x, port.offset_x, port.width);
    tool.y = position(event.y, port.offset_y, port.height);

    switch (event.eventType) {
      case LI_TOUCH_EVENT_DOWN:
      case LI_TOUCH_EVENT_MOVE:
        tool.transition = lvh::PointerTransition::update;
        tool.pressure = analog(event.pressureOrDistance);
        devices.pen_touching = true;
        break;
      case LI_TOUCH_EVENT_HOVER:
        tool.transition = lvh::PointerTransition::update;
        tool.distance = analog(event.pressureOrDistance);
        devices.pen_touching = false;
        break;
      case LI_TOUCH_EVENT_UP:
        tool.transition = lvh::PointerTransition::release;
        devices.pen_touching = false;
        break;
      case LI_TOUCH_EVENT_CANCEL:
      case LI_TOUCH_EVENT_CANCEL_ALL:
        tool.transition = lvh::PointerTransition::cancel;
        devices.pen_touching = false;
        break;
      case LI_TOUCH_EVENT_HOVER_LEAVE:
        tool.transition = lvh::PointerTransition::leave;
        devices.pen_touching = false;
        break;
      default:
        return;
    }

    if (event.rotation != LI_ROT_UNKNOWN && event.tilt != LI_TILT_UNKNOWN) {
      const float rotation_rad = static_cast<float>(event.rotation) * (std::numbers::pi_v<float> / 180.0f);
      const float tilt_rad = static_cast<float>(event.tilt) * (std::numbers::pi_v<float> / 180.0f);
      const float r = std::sin(tilt_rad);
      const float z = std::cos(tilt_rad);
      tool.tilt_x = std::atan2(-std::sin(rotation_rad) * r, z) * 180.0f / std::numbers::pi_v<float>;
      tool.tilt_y = std::atan2(std::cos(rotation_rad) * r, z) * 180.0f / std::numbers::pi_v<float>;
    }

    log_failure("pen tool update", devices.pen->place_tool(tool));
  }
}

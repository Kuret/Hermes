/**
 * @file src/platform/linux/wayland.cpp
 * @brief Definitions for Wayland capture.
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xf86drm.h>

// local includes
#include "graphics.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/round_robin.h"
#include "src/utility.h"
#include "wayland.h"

extern const wl_interface wl_output_interface;

using namespace std::literals;

// Disable warning for converting incompatible functions
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wpmf-conversions"

namespace wl {
  void logical_size(int mode_width, int mode_height, int transform, wl_fixed_t scale, int &width, int &height) {
    const bool rotated = transform & 1;
    width = rotated ? mode_height : mode_width;
    height = rotated ? mode_width : mode_height;
    // Fixed point is exact to 1/256, so two decimals recover any scale a user
    // is likely to have configured without changing an exact one.
    const double factor = std::round(wl_fixed_to_double(scale) * 100.0) / 100.0;
    if (factor > 0.0) {
      width = (int) std::lround(width / factor);
      height = (int) std::lround(height / factor);
    }
  }

  output_rect_t desktop_envelope(const std::vector<output_rect_t> &outputs) {
    int left = INT_MAX;
    int top = INT_MAX;
    int right = INT_MIN;
    int bottom = INT_MIN;
    for (const auto &output : outputs) {
      if (output.width <= 0 || output.height <= 0) {
        continue;
      }
      left = std::min(left, output.x);
      top = std::min(top, output.y);
      right = std::max(right, output.x + output.width);
      bottom = std::max(bottom, output.y + output.height);
    }
    if (left == INT_MAX) {
      return {0, 0, 0, 0};
    }
    return {left, top, right - left, bottom - top};
  }

  input_geometry_t input_geometry(const output_rect_t &envelope, const output_rect_t &output, int mode_width, int mode_height) {
    const double s_x = output.width > 0 && mode_width > 0 ? (double) mode_width / output.width : 1.0;
    const double s_y = output.height > 0 && mode_height > 0 ? (double) mode_height / output.height : 1.0;
    return {
      (int) std::lround((output.x - envelope.x) * s_x),
      (int) std::lround((output.y - envelope.y) * s_y),
      (int) std::lround(envelope.width * s_x),
      (int) std::lround(envelope.height * s_y),
    };
  }

  // Helper to call C++ method from wayland C callback
  template<class T, class Method, Method m, class... Params>
  static auto classCall(void *data, Params... params) -> decltype(((*reinterpret_cast<T *>(data)).*m)(params...)) {
    return ((*reinterpret_cast<T *>(data)).*m)(params...);
  }

#define CLASS_CALL(c, m) classCall<c, decltype(&c::m), &c::m>

  // Define buffer params listener
  static const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_t::buffer_params_created,
    .failed = dmabuf_t::buffer_params_failed
  };

  int display_t::init(const char *display_name) {
    if (!display_name) {
      display_name = std::getenv("WAYLAND_DISPLAY");
    }

    if (!display_name) {
      BOOST_LOG(error) << "Environment variable WAYLAND_DISPLAY has not been defined"sv;
      return -1;
    }

    display_internal.reset(wl_display_connect(display_name));
    if (!display_internal) {
      BOOST_LOG(error) << "Couldn't connect to Wayland display: "sv << display_name;
      return -1;
    }

    BOOST_LOG(info) << "Found display ["sv << display_name << ']';

    return 0;
  }

  void display_t::roundtrip() {
    wl_display_roundtrip(display_internal.get());
  }

  /**
   * @brief Waits up to the specified timeout to dispatch new events on the wl_display.
   * @param timeout The timeout in milliseconds.
   * @return `true` if new events were dispatched, `false` if the timeout expired
   *         or the connection is dead; connection_error() tells the two apart.
   */
  bool display_t::dispatch(std::chrono::milliseconds timeout) {
    // Check if any events are queued already. If not, flush
    // outgoing events, and prepare to wait for readability.
    if (wl_display_prepare_read(display_internal.get()) == 0) {
      wl_display_flush(display_internal.get());

      // Wait for an event to come in
      struct pollfd pfd = {};
      pfd.fd = wl_display_get_fd(display_internal.get());
      pfd.events = POLLIN;
      // A hang-up or error without POLLIN still has to reach read_events, so
      // the connection error is recorded instead of looking like a timeout.
      if (poll(&pfd, 1, timeout.count()) == 1 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
        // Read the new event(s). -1 only ever means the connection is dead.
        if (wl_display_read_events(display_internal.get()) < 0) {
          connection_error();
          return false;
        }
      } else {
        // We timed out, so unlock the queue now
        wl_display_cancel_read(display_internal.get());
        return false;
      }
    }

    // Dispatch any existing or new pending events. -1 means the connection is dead.
    if (wl_display_dispatch_pending(display_internal.get()) < 0) {
      connection_error();
      return false;
    }
    return true;
  }

  int display_t::connection_error() {
    auto err = wl_display_get_error(display_internal.get());
    if (err && !error_logged) {
      error_logged = true;
      if (err == EPROTO) {
        const wl_interface *iface = nullptr;
        std::uint32_t id = 0;
        auto code = wl_display_get_protocol_error(display_internal.get(), &iface, &id);
        BOOST_LOG(error) << "Wayland connection lost to a protocol error: "sv
                         << (iface ? iface->name : "unknown") << '#' << id << " error "sv << code
                         << ". Capture cannot continue on this connection."sv;
      } else {
        BOOST_LOG(error) << "Wayland connection lost: "sv << std::strerror(err);
      }
    }
    return err;
  }

  wl_registry *display_t::registry() {
    return wl_display_get_registry(display_internal.get());
  }

  namespace {
    struct output_mode_t {
      zwlr_output_mode_v1 *proxy {nullptr};
      int width {0};
      int height {0};
      int refresh_rate {0};
      bool preferred {false};
    };

    struct output_head_t {
      zwlr_output_head_v1 *proxy {nullptr};
      std::string name;
      std::string description;
      bool enabled {false};
      output_mode_t *current_mode {nullptr};
      int x {0};
      int y {0};
      int transform {WL_OUTPUT_TRANSFORM_NORMAL};
      wl_fixed_t scale {wl_fixed_from_int(1)};
      std::vector<std::unique_ptr<output_mode_t>> modes;
    };

    struct saved_output_t {
      bool enabled {false};
      int width {0};
      int height {0};
      int refresh_rate {0};
      int x {0};
      int y {0};
      int transform {WL_OUTPUT_TRANSFORM_NORMAL};
      wl_fixed_t scale {wl_fixed_from_int(1)};
    };

    std::map<std::string, saved_output_t> saved_output_layout;
    bool saved_output_layout_available {false};
    // Guards the saved layout and the held connection below. A restore has to
    // see a configure as having finished, and the other way round.
    std::mutex output_layout_mutex;

    struct output_control_t {
      display_t display;
      zwlr_output_manager_v1 *manager {nullptr};
      std::vector<std::unique_ptr<output_head_t>> heads;
      uint32_t serial {0};
      bool ready {false};
      bool manager_finished {false};
      int apply_result {0};

      ~output_control_t() {
        for (auto &head : heads) {
          for (auto &mode : head->modes) {
            if (mode->proxy) {
              zwlr_output_mode_v1_destroy(mode->proxy);
            }
          }
          if (head->proxy) {
            zwlr_output_head_v1_destroy(head->proxy);
          }
        }
        if (manager && !manager_finished) {
          zwlr_output_manager_v1_destroy(manager);
        }
      }

      bool init() {
        if (display.init()) {
          return false;
        }

        static const wl_registry_listener registry_listener {
          .global = [](void *data, wl_registry *registry, uint32_t id, const char *interface, uint32_t version) {
            auto *control = static_cast<output_control_t *>(data);
            if (std::strcmp(interface, zwlr_output_manager_v1_interface.name) == 0) {
              const auto bind_version = std::min(version, 4u);
              control->manager = static_cast<zwlr_output_manager_v1 *>(
                wl_registry_bind(registry, id, &zwlr_output_manager_v1_interface, bind_version)
              );
              zwlr_output_manager_v1_add_listener(control->manager, &manager_listener, control);
            }
          },
          .global_remove = [](void *, wl_registry *, uint32_t) {},
        };

        wl_registry_add_listener(display.registry(), &registry_listener, this);
        display.roundtrip();
        return manager != nullptr;
      }

      bool collect() {
        if (!manager) {
          return false;
        }
        display.roundtrip();
        // A complete layout can legitimately be empty while the compositor is
        // waiting for a newly hotplugged DRM connector to appear.
        return ready && !manager_finished;
      }

      output_head_t *find_head(const std::string &name) {
        const auto it = std::find_if(heads.begin(), heads.end(), [&](const auto &head) {
          return head->name == name;
        });
        return it == heads.end() ? nullptr : it->get();
      }

      static void mode_size(void *data, zwlr_output_mode_v1 *, int32_t width, int32_t height) {
        auto *mode = static_cast<output_mode_t *>(data);
        mode->width = width;
        mode->height = height;
      }

      static void mode_refresh(void *data, zwlr_output_mode_v1 *, int32_t refresh_rate) {
        static_cast<output_mode_t *>(data)->refresh_rate = refresh_rate;
      }

      static void mode_preferred(void *data, zwlr_output_mode_v1 *) {
        static_cast<output_mode_t *>(data)->preferred = true;
      }

      static void mode_finished(void *, zwlr_output_mode_v1 *) {}

      static void head_name(void *data, zwlr_output_head_v1 *, const char *name) {
        static_cast<output_head_t *>(data)->name = name;
      }

      static void head_description(void *data, zwlr_output_head_v1 *, const char *description) {
        static_cast<output_head_t *>(data)->description = description;
      }

      static void head_physical_size(void *, zwlr_output_head_v1 *, int32_t, int32_t) {}

      static void head_mode(void *data, zwlr_output_head_v1 *, zwlr_output_mode_v1 *proxy) {
        auto *head = static_cast<output_head_t *>(data);
        auto mode = std::make_unique<output_mode_t>();
        mode->proxy = proxy;
        zwlr_output_mode_v1_add_listener(proxy, &mode_listener, mode.get());
        head->modes.emplace_back(std::move(mode));
      }

      static void head_enabled(void *data, zwlr_output_head_v1 *, int32_t enabled) {
        static_cast<output_head_t *>(data)->enabled = enabled != 0;
      }

      static void head_current_mode(void *data, zwlr_output_head_v1 *, zwlr_output_mode_v1 *proxy) {
        auto *head = static_cast<output_head_t *>(data);
        const auto it = std::find_if(head->modes.begin(), head->modes.end(), [&](const auto &mode) {
          return mode->proxy == proxy;
        });
        head->current_mode = it == head->modes.end() ? nullptr : it->get();
      }

      static void head_position(void *data, zwlr_output_head_v1 *, int32_t x, int32_t y) {
        auto *head = static_cast<output_head_t *>(data);
        head->x = x;
        head->y = y;
      }

      static void head_transform(void *data, zwlr_output_head_v1 *, int32_t transform) {
        static_cast<output_head_t *>(data)->transform = transform;
      }

      static void head_scale(void *data, zwlr_output_head_v1 *, wl_fixed_t scale) {
        static_cast<output_head_t *>(data)->scale = scale;
      }

      static void head_finished(void *, zwlr_output_head_v1 *) {}
      static void head_string(void *, zwlr_output_head_v1 *, const char *) {}
      static void head_adaptive_sync(void *, zwlr_output_head_v1 *, uint32_t) {}

      static void manager_head(void *data, zwlr_output_manager_v1 *, zwlr_output_head_v1 *proxy) {
        auto *control = static_cast<output_control_t *>(data);
        auto head = std::make_unique<output_head_t>();
        head->proxy = proxy;
        zwlr_output_head_v1_add_listener(proxy, &head_listener, head.get());
        control->heads.emplace_back(std::move(head));
      }

      static void manager_done(void *data, zwlr_output_manager_v1 *, uint32_t serial) {
        auto *control = static_cast<output_control_t *>(data);
        control->serial = serial;
        control->ready = true;
      }

      static void manager_protocol_finished(void *data, zwlr_output_manager_v1 *) {
        static_cast<output_control_t *>(data)->manager_finished = true;
      }

      static void configuration_succeeded(void *data, zwlr_output_configuration_v1 *) {
        static_cast<output_control_t *>(data)->apply_result = 1;
      }

      static void configuration_failed(void *data, zwlr_output_configuration_v1 *) {
        static_cast<output_control_t *>(data)->apply_result = -1;
      }

      static void configuration_cancelled(void *data, zwlr_output_configuration_v1 *) {
        static_cast<output_control_t *>(data)->apply_result = -1;
      }

      static inline const zwlr_output_mode_v1_listener mode_listener {
        .size = mode_size,
        .refresh = mode_refresh,
        .preferred = mode_preferred,
        .finished = mode_finished,
      };

      static inline const zwlr_output_head_v1_listener head_listener {
        .name = head_name,
        .description = head_description,
        .physical_size = head_physical_size,
        .mode = head_mode,
        .enabled = head_enabled,
        .current_mode = head_current_mode,
        .position = head_position,
        .transform = head_transform,
        .scale = head_scale,
        .finished = head_finished,
        .make = head_string,
        .model = head_string,
        .serial_number = head_string,
        .adaptive_sync = head_adaptive_sync,
      };

      static inline const zwlr_output_manager_v1_listener manager_listener {
        .head = manager_head,
        .done = manager_done,
        .finished = manager_protocol_finished,
      };

      static inline const zwlr_output_configuration_v1_listener configuration_listener {
        .succeeded = configuration_succeeded,
        .failed = configuration_failed,
        .cancelled = configuration_cancelled,
      };
    };

    /**
     * @brief An output-management connection kept open for as long as an
     * exclusive layout is held, and the thread that keeps it serviced.
     *
     * Hyprland keeps what a client configured through output-management as an
     * override of the matching monitor rule, per output name, in that client's
     * manager object, and lays it over the rule on every rule reload: whether the
     * output is enabled always, mode, position, transform and scale for whatever
     * the client committed. It is erased when the manager goes away. An exclusive
     * layout is made of exactly such overrides - the physical outputs disabled -
     * so a connection closed on return, as configure_virtual_output() otherwise
     * does, lets the very next reload turn them back on, and any rule applied to
     * the headless output reloads. Holding the connection is what makes the
     * layout last, and closing it is what ends it: the compositor drops the
     * overrides and the rules govern again, which is also what happens if this
     * process dies without restoring anything.
     *
     * The connection must not go unread. The compositor sends an event for every
     * change to an output and a client whose queue fills up is disconnected,
     * overrides and all. So a thread of its own sleeps in poll() on the connection
     * and a stop pipe and dispatches what arrives. It is the only reader until
     * stop() has joined it, after which the owner may use the connection alone.
     */
    struct held_layout_t {
      std::unique_ptr<output_control_t> control;
      std::string output;
      std::thread thread;
      int stop_pipe[2] {-1, -1};

      held_layout_t(std::unique_ptr<output_control_t> control, std::string output):
          control {std::move(control)},
          output {std::move(output)} {
        if (pipe2(stop_pipe, O_CLOEXEC) != 0) {
          stop_pipe[0] = stop_pipe[1] = -1;
          return;
        }
        thread = std::thread([this]() {
          service();
        });
      }

      ~held_layout_t() {
        stop();
        for (int fd : stop_pipe) {
          if (fd >= 0) {
            close(fd);
          }
        }
      }

      held_layout_t(const held_layout_t &) = delete;
      held_layout_t &operator=(const held_layout_t &) = delete;

      void stop() {
        if (stop_pipe[1] >= 0) {
          const char byte = 0;
          [[maybe_unused]] const auto written = write(stop_pipe[1], &byte, 1);
        }
        if (thread.joinable()) {
          thread.join();
        }
      }

      void service() {
        const int fd = wl_display_get_fd(control->display.get());
        while (true) {
          pollfd fds[2] = {{fd, POLLIN, 0}, {stop_pipe[0], POLLIN, 0}};
          if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
              continue;
            }
            return;
          }
          if (fds[1].revents) {
            return;
          }
          if (!control->display.dispatch(0ms) && control->display.connection_error()) {
            BOOST_LOG(warning) << "The compositor closed the output-management connection holding the exclusive layout."sv;
            return;
          }
        }
      }
    };

    std::unique_ptr<held_layout_t> held_layout;

    output_mode_t *preferred_or_current_mode(output_head_t &head) {
      if (head.current_mode) {
        return head.current_mode;
      }
      const auto it = std::find_if(head.modes.begin(), head.modes.end(), [](const auto &mode) {
        return mode->preferred;
      });
      return it == head.modes.end() ? (head.modes.empty() ? nullptr : head.modes.front().get()) : it->get();
    }

    output_mode_t *matching_mode(output_head_t &head, int width, int height, int refresh_rate) {
      const auto it = std::find_if(head.modes.begin(), head.modes.end(), [&](const auto &mode) {
        return mode->width == width && mode->height == height &&
               (refresh_rate <= 0 || mode->refresh_rate == 0 || std::abs(mode->refresh_rate - refresh_rate) <= 1000);
      });
      return it == head.modes.end() ? nullptr : it->get();
    }

    bool apply_configuration(output_control_t &control, output_head_t &virtual_head, int width, int height, int refresh_rate, bool exclusive, bool restore, bool bare_virtual = false) {
      auto *configuration = zwlr_output_manager_v1_create_configuration(control.manager, control.serial);
      if (!configuration) {
        return false;
      }

      bool valid = true;
      int right_edge = 0;
      for (const auto &head : control.heads) {
        if (head->enabled && head->current_mode && head.get() != &virtual_head) {
          // Positions are logical, so the edge has to be logical too. Adding
          // the mode width instead leaves a gap after a scaled output, which
          // the cursor cannot cross.
          int logical_width = 0;
          int logical_height = 0;
          logical_size(head->current_mode->width, head->current_mode->height, head->transform, head->scale, logical_width, logical_height);
          right_edge = std::max(right_edge, head->x + logical_width);
        }
      }

      for (const auto &head : control.heads) {
        bool enable = head->enabled;
        output_mode_t *mode = preferred_or_current_mode(*head);
        int x = head->x;
        int y = head->y;
        int transform = head->transform;
        wl_fixed_t scale = head->scale;
        bool custom_mode = false;

        if (restore) {
          const auto saved = saved_output_layout.find(head->name);
          if (saved != saved_output_layout.end()) {
            enable = saved->second.enabled;
            mode = matching_mode(*head, saved->second.width, saved->second.height, saved->second.refresh_rate);
            x = saved->second.x;
            y = saved->second.y;
            transform = saved->second.transform;
            scale = saved->second.scale;
          }
        } else if (head.get() == &virtual_head) {
          enable = true;
          mode = matching_mode(*head, width, height, refresh_rate);
          custom_mode = mode == nullptr;
          x = exclusive ? 0 : right_edge;
          y = 0;
          transform = WL_OUTPUT_TRANSFORM_NORMAL;
          scale = wl_fixed_from_int(1);
        } else if (exclusive) {
          enable = false;
        }

        if (!enable) {
          zwlr_output_configuration_v1_disable_head(configuration, head->proxy);
          continue;
        }

        // Enabled and nothing else: a compositor that keeps what a client asked
        // for as an override of the output's rule, and applies it on top of the
        // rule on every rule reload, would otherwise be told this output's mode,
        // position, transform and scale for as long as the client holds on to it.
        // Left unsaid, those stay whatever the rules - Hermes's own pin, the
        // user's profile - say they are.
        if (bare_virtual && head.get() == &virtual_head && !restore) {
          zwlr_output_configuration_v1_enable_head(configuration, head->proxy);
          continue;
        }

        if (!mode && !custom_mode) {
          BOOST_LOG(error) << "Wayland output "sv << head->name << " has no usable mode"sv;
          valid = false;
          break;
        }

        auto *configured_head = zwlr_output_configuration_v1_enable_head(configuration, head->proxy);
        if (custom_mode) {
          zwlr_output_configuration_head_v1_set_custom_mode(configured_head, width, height, refresh_rate);
        } else {
          zwlr_output_configuration_head_v1_set_mode(configured_head, mode->proxy);
        }
        zwlr_output_configuration_head_v1_set_position(configured_head, x, y);
        zwlr_output_configuration_head_v1_set_transform(configured_head, transform);
        zwlr_output_configuration_head_v1_set_scale(configured_head, scale);
      }

      if (!valid) {
        zwlr_output_configuration_v1_destroy(configuration);
        return false;
      }

      control.apply_result = 0;
      zwlr_output_configuration_v1_add_listener(configuration, &output_control_t::configuration_listener, &control);
      zwlr_output_configuration_v1_apply(configuration);
      for (int attempt = 0; attempt < 20 && control.apply_result == 0; ++attempt) {
        control.display.dispatch(100ms);
      }
      zwlr_output_configuration_v1_destroy(configuration);
      return control.apply_result == 1;
    }
  }  // namespace

  inline monitor_t::monitor_t(wl_output *output):
      output {output},
      wl_listener {
        &CLASS_CALL(monitor_t, wl_geometry),
        &CLASS_CALL(monitor_t, wl_mode),
        &CLASS_CALL(monitor_t, wl_done),
        &CLASS_CALL(monitor_t, wl_scale),
      },
      xdg_listener {
        &CLASS_CALL(monitor_t, xdg_position),
        &CLASS_CALL(monitor_t, xdg_size),
        &CLASS_CALL(monitor_t, xdg_done),
        &CLASS_CALL(monitor_t, xdg_name),
        &CLASS_CALL(monitor_t, xdg_description)
      } {
  }

  inline void monitor_t::xdg_name(zxdg_output_v1 *, const char *name) {
    this->name = name;

    BOOST_LOG(info) << "Name: "sv << this->name;
  }

  void monitor_t::xdg_description(zxdg_output_v1 *, const char *description) {
    this->description = description;

    BOOST_LOG(info) << "Found monitor: "sv << this->description;
  }

  void monitor_t::xdg_position(zxdg_output_v1 *, std::int32_t x, std::int32_t y) {
    viewport.offset_x = x;
    viewport.offset_y = y;

    BOOST_LOG(info) << "Offset: "sv << x << 'x' << y;
  }

  void monitor_t::xdg_size(zxdg_output_v1 *, std::int32_t width, std::int32_t height) {
    logical_width = width;
    logical_height = height;

    BOOST_LOG(info) << "Logical size: "sv << width << 'x' << height;
  }

  void monitor_t::wl_mode(
    wl_output *wl_output,
    std::uint32_t flags,
    std::int32_t width,
    std::int32_t height,
    std::int32_t refresh
  ) {
    viewport.width = width;
    viewport.height = height;

    BOOST_LOG(info) << "Resolution: "sv << width << 'x' << height;
  }

  void monitor_t::listen(zxdg_output_manager_v1 *output_manager) {
    auto xdg_output = zxdg_output_manager_v1_get_xdg_output(output_manager, output);
    zxdg_output_v1_add_listener(xdg_output, &xdg_listener, this);
    wl_output_add_listener(output, &wl_listener, this);
  }

  namespace {
    // The protocol carries xy as value * 1,000,000; SS_HDR_METADATA, like the
    // CTA-861 infoframe it mirrors, as value * 50,000.
    constexpr std::int32_t protocol_chromaticity_scale = 1'000'000;
    constexpr std::int32_t ss_chromaticity_scale = 50'000;

    std::uint16_t clamp_u16(std::int64_t value) {
      return static_cast<std::uint16_t>(std::clamp<std::int64_t>(value, 0, UINT16_MAX));
    }

    // xy * 1,000,000 to xy * 50,000, rounded to nearest rather than truncated so
    // that 0.708 (708000) lands on 14160 exactly and 0.3127 on 6254 instead of 6253.
    std::uint16_t chromaticity_to_ss(std::int32_t value) {
      constexpr std::int64_t divisor = protocol_chromaticity_scale / ss_chromaticity_scale;
      return clamp_u16((static_cast<std::int64_t>(value) + divisor / 2) / divisor);
    }

    // ITU-R BT.2020 primaries with a D65 white point, xy * 1,000,000. The encode
    // path applies the BT.2020 RGB to YUV matrix to anything this capture
    // calls HDR, so this is also what the metadata should claim when a
    // compositor says nothing more specific.
    constexpr color_info_t::chromaticities_t bt2020_chromaticities {708000, 292000, 170000, 797000, 131000, 46000, 312700, 329000};
    constexpr color_info_t::chromaticities_t srgb_chromaticities {640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000};
    constexpr color_info_t::chromaticities_t display_p3_chromaticities {680000, 320000, 265000, 690000, 150000, 60000, 312700, 329000};

    std::optional<color_info_t::chromaticities_t> named_chromaticities(std::uint32_t named) {
      switch (named) {
        case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
          return bt2020_chromaticities;
        case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
          return srgb_chromaticities;
        case WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3:
          return display_p3_chromaticities;
        default:
          return std::nullopt;
      }
    }
  }  // namespace

  bool hdr_metadata_from_color_info(const color_info_t &info, SS_HDR_METADATA &metadata) {
    std::memset(&metadata, 0, sizeof(metadata));

    if (!info.pq()) {
      return false;
    }

    // Target volume first, then how the colour is encoded, then whatever the
    // named set means, and BT.2020 last since PQ here is always delivered as that.
    color_info_t::chromaticities_t chromaticities = bt2020_chromaticities;
    if (info.target_primaries) {
      chromaticities = *info.target_primaries;
    } else if (info.primaries) {
      chromaticities = *info.primaries;
    } else if (info.primaries_named) {
      if (auto named = named_chromaticities(*info.primaries_named)) {
        chromaticities = *named;
      }
    }

    for (int i = 0; i < 3; ++i) {
      metadata.displayPrimaries[i].x = chromaticity_to_ss(chromaticities[i * 2]);
      metadata.displayPrimaries[i].y = chromaticity_to_ss(chromaticities[i * 2 + 1]);
    }
    metadata.whitePoint.x = chromaticity_to_ss(chromaticities[6]);
    metadata.whitePoint.y = chromaticity_to_ss(chromaticities[7]);

    metadata.maxDisplayLuminance = clamp_u16(info.target_max_lum.value_or(info.max_lum.value_or(0)));
    metadata.minDisplayLuminance = clamp_u16(info.target_min_lum.value_or(info.min_lum.value_or(0)));
    metadata.maxContentLightLevel = clamp_u16(info.target_max_cll.value_or(0));
    metadata.maxFrameAverageLightLevel = clamp_u16(info.target_max_fall.value_or(0));

    return true;
  }

  /**
   * One description of an output, from the request that asked for it until its
   * information has all arrived. Owns the protocol objects so that dropping a
   * fetch that is still in flight cancels it.
   */
  struct color_output_t::fetch_t {
    color_output_t *owner;
    wp_image_description_v1 *description {nullptr};
    wp_image_description_info_v1 *information {nullptr};
    color_info_t data;

    explicit fetch_t(color_output_t *owner):
        owner {owner} {
    }

    ~fetch_t() {
      if (information) {
        wp_image_description_info_v1_destroy(information);
      }
      if (description) {
        wp_image_description_v1_destroy(description);
      }
    }

    fetch_t(const fetch_t &) = delete;
    fetch_t &operator=(const fetch_t &) = delete;

    void failed(std::uint32_t cause, const char *message) {
      BOOST_LOG(warning) << "Wayland compositor could not describe the output's colour: "sv << message << " (cause "sv << cause << ')';
      data.failed = true;
      data.done = true;
      owner->fetch_done(this);
    }

    void ready() {
      // A description is only allowed to be asked for information once, and
      // both ready events mean the same thing here; the identity they carry is
      // only for comparing descriptions, which same_encoding() does by content.
      if (information) {
        return;
      }
      information = wp_image_description_v1_get_information(description);
      wp_image_description_info_v1_add_listener(information, &info_listener, this);
    }

    void finished() {
      // "done" destroys the object on the compositor's side; the proxy is ours
      // to release.
      wp_image_description_info_v1_destroy(information);
      information = nullptr;
      data.done = true;
      owner->fetch_done(this);
    }

    static const wp_image_description_v1_listener description_listener;
    static const wp_image_description_info_v1_listener info_listener;
  };

  const wp_image_description_v1_listener color_output_t::fetch_t::description_listener = {
    .failed = [](void *data, wp_image_description_v1 *, std::uint32_t cause, const char *msg) {
      static_cast<fetch_t *>(data)->failed(cause, msg);
    },
    .ready = [](void *data, wp_image_description_v1 *, std::uint32_t) {
      static_cast<fetch_t *>(data)->ready();
    },
    .ready2 = [](void *data, wp_image_description_v1 *, std::uint32_t, std::uint32_t) {
      static_cast<fetch_t *>(data)->ready();
    },
  };

  const wp_image_description_info_v1_listener color_output_t::fetch_t::info_listener = {
    .done = [](void *data, wp_image_description_info_v1 *) {
      static_cast<fetch_t *>(data)->finished();
    },
    // An ICC profile arrives as a file descriptor that is ours to close; the
    // parametric values below are all that is read.
    .icc_file = [](void *, wp_image_description_info_v1 *, std::int32_t fd, std::uint32_t) {
      ::close(fd);
    },
    .primaries = [](void *data, wp_image_description_info_v1 *, std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
      static_cast<fetch_t *>(data)->data.primaries = color_info_t::chromaticities_t {r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y};
    },
    .primaries_named = [](void *data, wp_image_description_info_v1 *, std::uint32_t primaries) {
      static_cast<fetch_t *>(data)->data.primaries_named = primaries;
    },
    .tf_power = [](void *, wp_image_description_info_v1 *, std::uint32_t) {},
    .tf_named = [](void *data, wp_image_description_info_v1 *, std::uint32_t tf) {
      static_cast<fetch_t *>(data)->data.transfer_function = tf;
    },
    .luminances = [](void *data, wp_image_description_info_v1 *, std::uint32_t min_lum, std::uint32_t max_lum, std::uint32_t) {
      static_cast<fetch_t *>(data)->data.min_lum = min_lum;
      static_cast<fetch_t *>(data)->data.max_lum = max_lum;
    },
    .target_primaries = [](void *data, wp_image_description_info_v1 *, std::int32_t r_x, std::int32_t r_y, std::int32_t g_x, std::int32_t g_y, std::int32_t b_x, std::int32_t b_y, std::int32_t w_x, std::int32_t w_y) {
      static_cast<fetch_t *>(data)->data.target_primaries = color_info_t::chromaticities_t {r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y};
    },
    .target_luminance = [](void *data, wp_image_description_info_v1 *, std::uint32_t min_lum, std::uint32_t max_lum) {
      static_cast<fetch_t *>(data)->data.target_min_lum = min_lum;
      static_cast<fetch_t *>(data)->data.target_max_lum = max_lum;
    },
    .target_max_cll = [](void *data, wp_image_description_info_v1 *, std::uint32_t max_cll) {
      static_cast<fetch_t *>(data)->data.target_max_cll = max_cll;
    },
    .target_max_fall = [](void *data, wp_image_description_info_v1 *, std::uint32_t max_fall) {
      static_cast<fetch_t *>(data)->data.target_max_fall = max_fall;
    },
  };

  color_output_t::color_output_t() = default;

  color_output_t::~color_output_t() {
    // Fetches release their own objects; the output object is released last so
    // nothing it created outlives it.
    pending.reset();
    current.reset();
    if (output) {
      wp_color_management_output_v1_destroy(output);
    }
  }

  void color_output_t::listen(wp_color_manager_v1 *manager, wl_output *wl_output) {
    this->manager = manager;
    output = wp_color_manager_v1_get_output(manager, wl_output);

    static const wp_color_management_output_v1_listener output_listener = {
      .image_description_changed = [](void *data, wp_color_management_output_v1 *) {
        static_cast<color_output_t *>(data)->description_changed();
      },
    };
    wp_color_management_output_v1_add_listener(output, &output_listener, this);

    start_fetch(current);
  }

  void color_output_t::start_fetch(std::unique_ptr<fetch_t> &slot) {
    slot = std::make_unique<fetch_t>(this);
    slot->description = wp_color_management_output_v1_get_image_description(output);
    wp_image_description_v1_add_listener(slot->description, &fetch_t::description_listener, slot.get());
  }

  bool color_output_t::settled() const {
    return !current || current->data.done;
  }

  const color_info_t &color_output_t::encoding() const {
    static const color_info_t none {};
    return current ? current->data : none;
  }

  void color_output_t::description_changed() {
    BOOST_LOG(debug) << "Wayland output image description changed"sv;
    if (!current || !current->data.done) {
      // Still waiting for the first one; asking again would only race it.
      return;
    }
    start_fetch(pending);
  }

  void color_output_t::fetch_done(fetch_t *fetch) {
    if (fetch != pending.get()) {
      return;
    }
    if (!fetch->data.failed && !fetch->data.same_encoding(current->data)) {
      BOOST_LOG(info) << "Wayland output now encodes differently ("sv
                      << (fetch->data.pq() ? "PQ"sv : "not PQ"sv) << ", was "sv << (current->data.pq() ? "PQ"sv : "not PQ"sv) << ')';
      changed_ = true;
    }
    pending.reset();
  }

  interface_t::interface_t() noexcept
      :
      screencopy_manager {nullptr},
      dmabuf_interface {nullptr},
      output_manager {nullptr},
      image_copy_capture_manager {nullptr},
      image_capture_source_manager {nullptr},
      listener {
        &CLASS_CALL(interface_t, add_interface),
        &CLASS_CALL(interface_t, del_interface)
      } {
  }

  void interface_t::listen(wl_registry *registry) {
    wl_registry_add_listener(registry, &listener, this);
  }

  void interface_t::add_interface(
    wl_registry *registry,
    std::uint32_t id,
    const char *interface,
    std::uint32_t version
  ) {
    BOOST_LOG(debug) << "Available interface: "sv << interface << '(' << id << ") version "sv << version;

    if (!std::strcmp(interface, wl_output_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      monitors.emplace_back(
        std::make_unique<monitor_t>(
          (wl_output *) wl_registry_bind(registry, id, &wl_output_interface, 2)
        )
      );
    } else if (!std::strcmp(interface, zxdg_output_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      output_manager = (zxdg_output_manager_v1 *) wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, version);

      this->interface[XDG_OUTPUT] = true;
    } else if (!std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      screencopy_manager = (zwlr_screencopy_manager_v1 *) wl_registry_bind(registry, id, &zwlr_screencopy_manager_v1_interface, version);

      this->interface[WLR_EXPORT_DMABUF] = true;
    } else if (!std::strcmp(interface, zwp_linux_dmabuf_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      // Never bind a version newer than the generated code knows about.
      dmabuf_interface = (zwp_linux_dmabuf_v1 *) wl_registry_bind(registry, id, &zwp_linux_dmabuf_v1_interface, std::min<std::uint32_t>(version, zwp_linux_dmabuf_v1_interface.version));

      this->interface[LINUX_DMABUF] = true;
    } else if (!std::strcmp(interface, ext_image_copy_capture_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      image_copy_capture_manager = (ext_image_copy_capture_manager_v1 *) wl_registry_bind(registry, id, &ext_image_copy_capture_manager_v1_interface, 1);

      this->interface[IMAGE_COPY_CAPTURE] = true;
    } else if (!std::strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      image_capture_source_manager = (ext_output_image_capture_source_manager_v1 *) wl_registry_bind(registry, id, &ext_output_image_capture_source_manager_v1_interface, 1);

      this->interface[IMAGE_CAPTURE_SOURCE] = true;
    } else if (!std::strcmp(interface, wp_color_manager_v1_interface.name)) {
      BOOST_LOG(info) << "Found interface: "sv << interface << '(' << id << ") version "sv << version;
      // Offered is not the same as wanted. Binding is what tells the compositor
      // this client understands colour, and a compositor that can run an output
      // in HDR answers that by delivering the output's own PQ encoding instead
      // of a copy it tone-maps to sRGB. A capture that cannot use PQ must not
      // ask for it, so the bind waits for the caller to say it is wanted.
      //
      // Never bind a version newer than the generated code knows about. Version
      // 1 is enough for everything used here; 2 and 3 only change which of two
      // events announces a ready description, and color_output_t takes both.
      if (bind_color_manager) {
        color_manager = (wp_color_manager_v1 *) wl_registry_bind(registry, id, &wp_color_manager_v1_interface, std::min<std::uint32_t>(version, wp_color_manager_v1_interface.version));

        this->interface[COLOR_MANAGEMENT] = true;
      }
    }
  }

  void interface_t::del_interface(wl_registry *registry, uint32_t id) {
    BOOST_LOG(info) << "Delete: "sv << id;
  }

  // A capture path that cannot allocate a buffer fails every frame, and every
  // failure is reported by a different object (see dmabuf_capture_exhausted()),
  // so both the giving-up decision and the log throttling have to be kept here.
  static std::atomic<unsigned> gbm_failure_streak {0};
  static std::atomic<std::int64_t> last_gbm_failure_ns {0};

  // Long enough to ride out a compositor that is still bringing an output up,
  // short enough that nobody watches a black screen for a second while we count.
  static constexpr unsigned gbm_failure_limit = 10;

  // A reinit retries within a frame time or two, so a failure arriving after a
  // pause this long is not part of the same run. Without the gap the streak
  // would be a lifetime tally: a user who fixed their configuration and started
  // a new stream would be refused on the first frame, on the strength of a run
  // that ended minutes ago.
  static constexpr std::int64_t gbm_failure_gap_ns = 5'000'000'000;

  static void note_gbm_failure() {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch()
    )
                       .count();
    const auto previous = last_gbm_failure_ns.exchange(now, std::memory_order_relaxed);

    if (previous != 0 && now - previous > gbm_failure_gap_ns) {
      gbm_failure_streak.store(1, std::memory_order_relaxed);
      return;
    }
    gbm_failure_streak.fetch_add(1, std::memory_order_relaxed);
  }

  static void clear_gbm_failures() {
    gbm_failure_streak.store(0, std::memory_order_relaxed);
    last_gbm_failure_ns.store(0, std::memory_order_relaxed);
  }

  // The first failure of a streak carries the diagnosis; the rest are the same
  // sentence at frame rate, which is how the original report ended up with tens
  // of thousands of identical lines and no explanation among them.
  static bool should_log_gbm_failure() {
    return gbm_failure_streak.load(std::memory_order_relaxed) <= 1;
  }

  bool dmabuf_capture_exhausted() {
    return gbm_failure_streak.load(std::memory_order_relaxed) >= gbm_failure_limit;
  }

  /**
   * @brief Open a render node, and prove GBM can actually allocate on it.
   *
   * Opening a render node says nothing about whether buffers can come from it.
   * Mesa loads a driver for whatever the node turns out to be, and when there is
   * none to load - a proprietary-NVIDIA node has no nouveau behind it - it falls
   * back to kms_swrast without complaint. kms_swrast allocates through
   * DRM_IOCTL_MODE_CREATE_DUMB, which render nodes refuse with EACCES, so the
   * device looks healthy right up to the moment the first buffer is asked for.
   * Asking for a throwaway buffer is what separates the two cases.
   */
  static gbm_device *open_usable_gbm_device(const char *node, int &out_fd) {
    const int fd = open(node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(debug) << "GBM: cannot open "sv << node << ": "sv << std::strerror(errno);
      return nullptr;
    }

    gbm_device *device = gbm_create_device(fd);
    if (!device) {
      BOOST_LOG(debug) << "GBM: "sv << node << " has no GBM device"sv;
      close(fd);
      return nullptr;
    }

    // Screencopy buffers are ordinary render targets, so the probe asks for the
    // smallest buffer of the same kind rather than a frame-sized one.
    gbm_bo *probe = gbm_bo_create(device, 64, 64, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING);
    if (!probe) {
      BOOST_LOG(debug) << "GBM: "sv << node << " opened but cannot allocate buffers"sv;
      gbm_device_destroy(device);
      close(fd);
      return nullptr;
    }
    gbm_bo_destroy(probe);

    out_fd = fd;
    return device;
  }

  namespace {
    struct feedback_ctx_t {
      dev_t main_device {};
      bool have_main_device {false};
      bool done {false};
    };

    void feedback_done(void *data, zwp_linux_dmabuf_feedback_v1 *) {
      static_cast<feedback_ctx_t *>(data)->done = true;
    }

    void feedback_format_table(void *, zwp_linux_dmabuf_feedback_v1 *, std::int32_t fd, std::uint32_t) {
      close(fd);
    }

    void feedback_main_device(void *data, zwp_linux_dmabuf_feedback_v1 *, wl_array *device) {
      auto ctx = static_cast<feedback_ctx_t *>(data);
      if (device->size == sizeof(dev_t)) {
        std::memcpy(&ctx->main_device, device->data, sizeof(dev_t));
        ctx->have_main_device = true;
      }
    }

    void feedback_tranche_done(void *, zwp_linux_dmabuf_feedback_v1 *) {}

    void feedback_tranche_target_device(void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {}

    void feedback_tranche_formats(void *, zwp_linux_dmabuf_feedback_v1 *, wl_array *) {}

    void feedback_tranche_flags(void *, zwp_linux_dmabuf_feedback_v1 *, std::uint32_t) {}

    const zwp_linux_dmabuf_feedback_v1_listener feedback_listener {
      .done = feedback_done,
      .format_table = feedback_format_table,
      .main_device = feedback_main_device,
      .tranche_done = feedback_tranche_done,
      .tranche_target_device = feedback_tranche_target_device,
      .tranche_formats = feedback_tranche_formats,
      .tranche_flags = feedback_tranche_flags,
    };

    /**
     * @brief The node to allocate on for the DRM device that owns a device number.
     *
     * A compositor names its GPU by number and may name either its primary or
     * its render node, so the number is resolved through libdrm rather than
     * used as a path. The answer is the render node, or the primary node for a
     * driver that exposes no render node: GBM can allocate there too, and
     * init_gbm() proves whichever node comes back before trusting it.
     * @return The node's path, or an empty string when no DRM device owns the number.
     */
    std::string render_node_for_device(dev_t device_id) {
      drmDevice *drm_device {nullptr};
      if (drmGetDeviceFromDevId(device_id, 0, &drm_device) != 0) {
        BOOST_LOG(warning) << "No DRM device found for device number "sv << major(device_id) << ':' << minor(device_id);
        return {};
      }
      std::string node;
      if (drm_device->available_nodes & (1 << DRM_NODE_RENDER)) {
        node = drm_device->nodes[DRM_NODE_RENDER];
      } else if (drm_device->available_nodes & (1 << DRM_NODE_PRIMARY)) {
        node = drm_device->nodes[DRM_NODE_PRIMARY];
      }
      drmFreeDevice(&drm_device);
      return node;
    }
  }  // namespace

  std::string compositor_render_node(display_t &display, zwp_linux_dmabuf_v1 *dmabuf_interface) {
    if (!display.get() || !dmabuf_interface) {
      return {};
    }
    if (zwp_linux_dmabuf_v1_get_version(dmabuf_interface) < ZWP_LINUX_DMABUF_V1_GET_DEFAULT_FEEDBACK_SINCE_VERSION) {
      BOOST_LOG(info) << "linux-dmabuf v"sv << zwp_linux_dmabuf_v1_get_version(dmabuf_interface)
                      << " has no feedback; cannot learn which GPU the compositor renders on"sv;
      return {};
    }

    feedback_ctx_t ctx;
    auto feedback = zwp_linux_dmabuf_v1_get_default_feedback(dmabuf_interface);
    zwp_linux_dmabuf_feedback_v1_add_listener(feedback, &feedback_listener, &ctx);
    auto roundtrip = wl_display_roundtrip(display.get());
    zwp_linux_dmabuf_feedback_v1_destroy(feedback);

    if (roundtrip < 0) {
      BOOST_LOG(warning) << "Wayland connection failed while reading linux-dmabuf feedback"sv;
      return {};
    }
    if (!ctx.done) {
      BOOST_LOG(warning) << "The compositor did not finish sending linux-dmabuf feedback"sv;
      return {};
    }
    if (!ctx.have_main_device) {
      BOOST_LOG(warning) << "The compositor sent no linux-dmabuf main device"sv;
      return {};
    }

    auto node = render_node_for_device(ctx.main_device);
    if (!node.empty()) {
      BOOST_LOG(info) << "The compositor renders on "sv << node << " (linux-dmabuf feedback)"sv;
    }
    return node;
  }

  void dmabuf_t::set_render_node(std::string node) {
    render_node = std::move(node);
  }

  /**
   * @brief Pick the render node the capture buffers will be allocated on.
   *
   * These buffers are handed to the compositor to copy the screen into, so they
   * have to come from the GPU the compositor renders on. "The first render node
   * that opens" is not that GPU on any machine with two of them: renderD128 and
   * renderD129 are handed out in module load order, so on a hybrid laptop the
   * first node is as likely as not the discrete card the compositor never
   * touches. Opening it succeeds, every frame after it fails, and what reaches
   * the user is a black stream with working audio and input - the encoders all
   * probe green, because an encoder probe never touches this path.
   *
   * The node is therefore taken, in order, from what the user configured, from
   * what the compositor itself said it renders on (linux-dmabuf feedback, see
   * compositor_render_node()), from what the wlroots compositor was told to
   * render on, and finally from a scan that keeps the first device able to
   * produce a buffer. That scan is the last resort for a reason: a buffer the
   * wrong GPU produced perfectly well is still one the compositor cannot
   * import, and on Hyprland that import failure is a fatal protocol error, not
   * a failed frame.
   */
  bool dmabuf_t::init_gbm() {
    if (gbm_device) {
      return true;
    }

    if (!config::video.adapter_name.empty()) {
      const auto &node = config::video.adapter_name;
      gbm_device = open_usable_gbm_device(node.c_str(), drm_fd);
      if (!gbm_device) {
        BOOST_LOG(error) << "Configured adapter_name "sv << node
                         << " cannot allocate capture buffers. Point it at the render node the "sv
                         << "compositor renders on, or clear it to let Hermes search."sv;
        return false;
      }
      BOOST_LOG(info) << "GBM capture device: "sv << node << " (adapter_name)"sv;
      // Compared by device, not by path: adapter_name may be a symlink to the
      // node the compositor named, or that GPU's primary node.
      struct stat st {};
      if (!render_node.empty() && stat(node.c_str(), &st) == 0 && render_node_for_device(st.st_rdev) != render_node) {
        BOOST_LOG(warning) << "adapter_name "sv << node << " is not the GPU the compositor renders on ("sv
                           << render_node << "); the compositor may refuse buffers allocated there."sv;
      }
      return true;
    }

    if (!render_node.empty()) {
      gbm_device = open_usable_gbm_device(render_node.c_str(), drm_fd);
      if (gbm_device) {
        BOOST_LOG(info) << "GBM capture device: "sv << render_node << " (named by the compositor)"sv;
        return true;
      }
      BOOST_LOG(warning) << "The compositor renders on "sv << render_node
                         << ", which cannot allocate capture buffers; searching the other render nodes."sv;
    }

    // A wlroots compositor renders the captured output on this node whenever the
    // session bothered to name one, which makes it the right answer before any
    // guessing - notably in the container image, whose entrypoint points sway at
    // the encode GPU precisely because the first node is the wrong one. It stays
    // a hint: a value that cannot allocate falls through to the scan.
    if (const char *wlr_node = std::getenv("WLR_RENDER_DRM_DEVICE"); wlr_node && wlr_node[0]) {
      gbm_device = open_usable_gbm_device(wlr_node, drm_fd);
      if (gbm_device) {
        BOOST_LOG(info) << "GBM capture device: "sv << wlr_node << " (WLR_RENDER_DRM_DEVICE)"sv;
        return true;
      }
      BOOST_LOG(warning) << "WLR_RENDER_DRM_DEVICE names "sv << wlr_node
                         << ", which cannot allocate capture buffers; searching the other render nodes."sv;
    }

    drmDevice *devices[16];
    const int n = drmGetDevices2(0, devices, 16);
    if (n <= 0) {
      BOOST_LOG(error) << "No DRM devices found"sv;
      return false;
    }

    std::string rejected;
    for (int i = 0; i < n && !gbm_device; i++) {
      if (!(devices[i]->available_nodes & (1 << DRM_NODE_RENDER))) {
        continue;
      }
      const char *node = devices[i]->nodes[DRM_NODE_RENDER];
      if (!node) {
        continue;
      }
      gbm_device = open_usable_gbm_device(node, drm_fd);
      if (gbm_device) {
        BOOST_LOG(info) << "GBM capture device: "sv << node;
      } else {
        rejected += rejected.empty() ? node : ", "s + node;
      }
    }
    drmFreeDevices(devices, n);

    if (!gbm_device) {
      BOOST_LOG(error) << "No render node can allocate capture buffers"sv
                       << (rejected.empty() ? ""s : " (tried "s + rejected + ")"s)
                       << ". Set adapter_name to the render node the compositor renders on."sv;
      return false;
    }

    return true;
  }

  // Cleanup GBM
  void dmabuf_t::cleanup_gbm() {
    if (current_bo) {
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
    }

    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }
  }

  dmabuf_t::dmabuf_t():
      status {READY},
      frames {},
      current_frame {&frames[0]},
      listener {
        &CLASS_CALL(dmabuf_t, buffer),
        &CLASS_CALL(dmabuf_t, flags),
        &CLASS_CALL(dmabuf_t, ready),
        &CLASS_CALL(dmabuf_t, failed),
        &CLASS_CALL(dmabuf_t, damage),
        &CLASS_CALL(dmabuf_t, linux_dmabuf),
        &CLASS_CALL(dmabuf_t, buffer_done),
      } {
  }

  // Start capture
  void dmabuf_t::listen(
    zwlr_screencopy_manager_v1 *screencopy_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface,
    wl_output *output,
    bool blend_cursor
  ) {
    this->dmabuf_interface = dmabuf_interface;
    // Reset state
    shm_info.supported = false;
    dmabuf_info.supported = false;

    // Create new frame
    auto frame = zwlr_screencopy_manager_v1_capture_output(
      screencopy_manager,
      blend_cursor ? 1 : 0,
      output
    );

    // Store frame data pointer for callbacks
    zwlr_screencopy_frame_v1_set_user_data(frame, this);

    // Add listener
    zwlr_screencopy_frame_v1_add_listener(frame, &listener, this);

    status = WAITING;
  }

  dmabuf_t::~dmabuf_t() {
    icc_destroy_session();
    cleanup_gbm();

    for (auto &frame : frames) {
      frame.destroy();
    }

    if (gbm_device) {
      gbm_device_destroy(gbm_device);
      gbm_device = nullptr;
    }

    // gbm_create_device() borrows the fd rather than adopting it, so destroying
    // the device leaves it open. Every reinit built a new dmabuf_t, so a capture
    // path that kept failing leaked one descriptor per retry.
    if (drm_fd >= 0) {
      close(drm_fd);
      drm_fd = -1;
    }
  }

  // Buffer format callback
  void dmabuf_t::buffer(
    zwlr_screencopy_frame_v1 *frame,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t stride
  ) {
    shm_info.supported = true;
    shm_info.format = format;
    shm_info.width = width;
    shm_info.height = height;
    shm_info.stride = stride;

    BOOST_LOG(debug) << "Screencopy supports SHM format: "sv << format;
  }

  // DMA-BUF format callback
  void dmabuf_t::linux_dmabuf(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t format,
    std::uint32_t width,
    std::uint32_t height
  ) {
    dmabuf_info.supported = true;
    dmabuf_info.format = format;
    dmabuf_info.width = width;
    dmabuf_info.height = height;

    BOOST_LOG(debug) << "Screencopy supports DMA-BUF format: "sv << format;
  }

  // Flags callback
  void dmabuf_t::flags(zwlr_screencopy_frame_v1 *frame, std::uint32_t flags) {
    y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
    BOOST_LOG(debug) << "Frame flags: "sv << flags << (y_invert ? " (y_invert)" : "");
  }

  // DMA-BUF creation helper
  void dmabuf_t::create_and_copy_dmabuf(zwlr_screencopy_frame_v1 *frame) {
    if (!init_gbm()) {
      note_gbm_failure();
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // Create GBM buffer
    current_bo = gbm_bo_create(gbm_device, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, GBM_BO_USE_RENDERING);
    if (!current_bo) {
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "Failed to allocate a "sv << dmabuf_info.width << 'x' << dmabuf_info.height
                         << " capture buffer in format "sv << dmabuf_info.format
                         << ". The render node this was allocated on is not the one the compositor "sv
                         << "renders on, or does not support the format it asked for."sv;
      }
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    // Get buffer info
    int fd = gbm_bo_get_fd(current_bo);
    if (fd < 0) {
      BOOST_LOG(error) << "Failed to get buffer FD"sv;
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
      return;
    }

    uint32_t stride = gbm_bo_get_stride(current_bo);
    uint64_t modifier = gbm_bo_get_modifier(current_bo);

    // Store in surface descriptor for later use
    auto next_frame = get_next_frame();
    next_frame->sd.fds[0] = fd;
    next_frame->sd.pitches[0] = stride;
    next_frame->sd.offsets[0] = 0;
    next_frame->sd.modifier = modifier;

    // Create linux-dmabuf buffer
    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride, modifier >> 32, modifier & 0xffffffff);

    // Add listener for buffer creation
    zwp_linux_buffer_params_v1_add_listener(params, &params_listener, frame);

    // Create Wayland buffer (async - callback will handle copy)
    zwp_linux_buffer_params_v1_create(params, dmabuf_info.width, dmabuf_info.height, dmabuf_info.format, 0);
  }

  // Buffer done callback - time to create buffer
  void dmabuf_t::buffer_done(zwlr_screencopy_frame_v1 *frame) {
    auto next_frame = get_next_frame();

    // Prefer DMA-BUF if supported
    if (dmabuf_info.supported && dmabuf_interface) {
      // Store format info first
      next_frame->sd.fourcc = dmabuf_info.format;
      next_frame->sd.width = dmabuf_info.width;
      next_frame->sd.height = dmabuf_info.height;

      // Create and start copy
      create_and_copy_dmabuf(frame);
    } else if (shm_info.supported) {
      // Retrying will not grow a code path that was never written, so this is
      // counted as a failure rather than left to spin: the compositor offered
      // only the shared-memory buffer, and Hermes has no capture for it.
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The compositor offers screencopy only as a shared-memory buffer, "sv
                         << "which this capture backend does not implement. Capture needs "sv
                         << "zwp_linux_dmabuf_v1 on this output."sv;
      }
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    } else {
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The compositor offered no buffer type for this frame."sv;
      }
      zwlr_screencopy_frame_v1_destroy(frame);
      status = REINIT;
    }
  }

  // Buffer params created callback
  void dmabuf_t::buffer_params_created(
    void *data,
    struct zwp_linux_buffer_params_v1 *params,
    struct wl_buffer *buffer
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    // Store for cleanup
    self->current_wl_buffer = buffer;

    // Start the actual copy
    zwlr_screencopy_frame_v1_copy(frame, buffer);
  }

  // Buffer params failed callback
  void dmabuf_t::buffer_params_failed(
    void *data,
    struct zwp_linux_buffer_params_v1 *params
  ) {
    auto frame = static_cast<zwlr_screencopy_frame_v1 *>(data);
    auto self = static_cast<dmabuf_t *>(zwlr_screencopy_frame_v1_get_user_data(frame));

    note_gbm_failure();
    if (should_log_gbm_failure()) {
      BOOST_LOG(error) << "The compositor refused the capture buffer. It was allocated on a device "sv
                       << "the compositor cannot import from."sv;
    }
    self->cleanup_gbm();

    zwlr_screencopy_frame_v1_destroy(frame);
    self->status = REINIT;
  }

  // Ready callback
  void dmabuf_t::ready(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {
    BOOST_LOG(debug) << "Frame ready"sv;

    // A frame arrived, so whatever went wrong before was transient.
    clear_gbm_failures();

    // Frame is ready for use, GBM buffer now contains screen content
    current_frame->destroy();
    current_frame = get_next_frame();

    // Keep the GBM buffer alive but destroy the Wayland objects
    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }

    cleanup_gbm();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = READY;
  }

  // Failed callback
  void dmabuf_t::failed(zwlr_screencopy_frame_v1 *frame) {
    BOOST_LOG(error) << "Frame capture failed"sv;

    // Clean up resources
    cleanup_gbm();
    auto next_frame = get_next_frame();
    next_frame->destroy();

    zwlr_screencopy_frame_v1_destroy(frame);
    status = REINIT;
  }

  void dmabuf_t::damage(
    zwlr_screencopy_frame_v1 *frame,
    std::uint32_t x,
    std::uint32_t y,
    std::uint32_t width,
    std::uint32_t height
  ) {};

  // ---------------------------------------------------------------------------
  // ext-image-copy-capture
  //
  // Adapted from Dregu's draft for upstream Sunshine
  // (LizardByte/Sunshine#4788), which is where the protocol sequence below was
  // worked out. It differs in three places, each noted at the site: the render
  // device the compositor names is validated before it is trusted, the modifier
  // lists are owned rather than malloc'd, and buffer failures feed the same
  // streak counter the screencopy path uses so that a session which can never
  // produce a frame ends instead of retrying at frame rate.
  // ---------------------------------------------------------------------------

  static const struct zwp_linux_buffer_params_v1_listener icc_params_listener = {
    .created = CLASS_CALL(dmabuf_t, icc_buffer_params_created),
    .failed = CLASS_CALL(dmabuf_t, icc_buffer_params_failed),
  };

  static const struct ext_image_copy_capture_session_v1_listener icc_session_listener = {
    .buffer_size = CLASS_CALL(dmabuf_t, icc_session_buffer_size),
    .shm_format = CLASS_CALL(dmabuf_t, icc_session_shm_format),
    .dmabuf_device = CLASS_CALL(dmabuf_t, icc_session_dmabuf_device),
    .dmabuf_format = CLASS_CALL(dmabuf_t, icc_session_dmabuf_format),
    .done = CLASS_CALL(dmabuf_t, icc_session_done),
    .stopped = CLASS_CALL(dmabuf_t, icc_session_stopped),
  };

  static const struct ext_image_copy_capture_frame_v1_listener icc_frame_listener = {
    .transform = CLASS_CALL(dmabuf_t, icc_frame_transform),
    .damage = CLASS_CALL(dmabuf_t, icc_frame_damage),
    .presentation_time = CLASS_CALL(dmabuf_t, icc_frame_presentation_time),
    .ready = CLASS_CALL(dmabuf_t, icc_frame_ready),
    .failed = CLASS_CALL(dmabuf_t, icc_frame_failed),
  };

  void dmabuf_t::icc_destroy_session() {
    if (icc_session.frame) {
      ext_image_copy_capture_frame_v1_destroy(icc_session.frame);
      icc_session.frame = nullptr;
    }
    if (icc_session.session) {
      ext_image_copy_capture_session_v1_destroy(icc_session.session);
      icc_session.session = nullptr;
    }
    icc_session = {};
  }

  bool dmabuf_t::icc_listen(
    ext_image_copy_capture_manager_v1 *manager,
    ext_output_image_capture_source_manager_v1 *source_manager,
    zwp_linux_dmabuf_v1 *dmabuf_interface,
    wl_output *output,
    bool blend_cursor
  ) {
    if (!manager || !source_manager || !dmabuf_interface) {
      return false;
    }

    this->dmabuf_interface = dmabuf_interface;
    icc_destroy_session();
    icc_session.cursor = blend_cursor;

    auto source = ext_output_image_capture_source_manager_v1_create_source(source_manager, output);
    if (!source) {
      BOOST_LOG(error) << "Could not name the Wayland output as an image capture source"sv;
      return false;
    }

    auto session = ext_image_copy_capture_manager_v1_create_session(
      manager,
      source,
      blend_cursor ? EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS : 0
    );

    // The session holds its own reference to the source, so the source object
    // is of no further use here.
    ext_image_capture_source_v1_destroy(source);

    if (!session) {
      BOOST_LOG(error) << "Could not open an ext-image-copy-capture session"sv;
      return false;
    }

    ext_image_copy_capture_session_v1_set_user_data(session, this);
    ext_image_copy_capture_session_v1_add_listener(session, &icc_session_listener, this);
    icc_session.session = session;
    status = WAITING;

    return true;
  }

  void dmabuf_t::icc_capture(bool blend_cursor) {
    if (!icc_session.session || blend_cursor != icc_session.cursor) {
      // A cursor-blending change is a different session, not a different frame.
      status = REINIT;
      return;
    }

    if (!icc_session.done || icc_session.frame) {
      return;
    }

    auto frame = ext_image_copy_capture_session_v1_create_frame(icc_session.session);
    if (!frame) {
      note_gbm_failure();
      status = REINIT;
      return;
    }

    icc_session.frame = frame;
    ext_image_copy_capture_frame_v1_set_user_data(frame, this);
    ext_image_copy_capture_frame_v1_add_listener(frame, &icc_frame_listener, this);
    icc_create_and_copy_dmabuf(frame);

    // Only if a frame is genuinely in flight. give_up() clears icc_session.frame
    // and sets REINIT, and overwriting that with WAITING here hid every
    // allocation failure: snapshot() saw a timeout instead of a reinit, so the
    // capture never ended and the streak counter never got the chance to stop
    // it - which is the permanent black stream this whole mechanism exists to
    // prevent.
    if (icc_session.frame) {
      status = WAITING;
    }
  }

  void dmabuf_t::icc_create_and_copy_dmabuf(ext_image_copy_capture_frame_v1 *frame) {
    auto give_up = [&]() {
      note_gbm_failure();
      ext_image_copy_capture_frame_v1_destroy(frame);
      icc_session.frame = nullptr;
      status = REINIT;
    };

    if (!init_gbm()) {
      give_up();
      return;
    }

    if (!icc_session.width || !icc_session.height || !icc_session.format) {
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The capture session negotiated no usable buffer format"sv;
      }
      give_up();
      return;
    }

    // Modifiers first: a compositor that offers them is telling us the layouts
    // it can write into, and an implicit-modifier buffer is only correct when it
    // offers none.
    if (auto it = icc_session.modifiers.find(icc_session.format);
        it != icc_session.modifiers.end() && !it->second.empty()) {
      current_bo = gbm_bo_create_with_modifiers(
        gbm_device,
        icc_session.width,
        icc_session.height,
        icc_session.format,
        it->second.data(),
        it->second.size()
      );
    }

    if (!current_bo) {
      current_bo = gbm_bo_create(gbm_device, icc_session.width, icc_session.height, icc_session.format, GBM_BO_USE_RENDERING);
    }

    if (!current_bo) {
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "Failed to allocate a "sv << icc_session.width << 'x' << icc_session.height
                         << " capture buffer in format "sv << icc_session.format
                         << ". The render node this was allocated on is not the one the compositor "sv
                         << "renders on, or does not support the format it asked for."sv;
      }
      give_up();
      return;
    }

    // Allocating with modifiers can hand back a buffer of several planes - an
    // AMD compression modifier does exactly that - and a compositor offered
    // only the first plane of one cannot use it. That failure arrives as a bare
    // "unknown reason" on the frame, with nothing to say a plane was missing.
    const std::uint64_t modifier = gbm_bo_get_modifier(current_bo);
    const int planes = gbm_bo_get_plane_count(current_bo);

    if (planes < 1 || planes > 4) {
      BOOST_LOG(error) << "Capture buffer has "sv << planes << " planes, which cannot be shared"sv;
      gbm_bo_destroy(current_bo);
      current_bo = nullptr;
      give_up();
      return;
    }

    auto next_frame = get_next_frame();
    auto params = zwp_linux_dmabuf_v1_create_params(dmabuf_interface);

    for (int plane = 0; plane < planes; ++plane) {
      int fd = gbm_bo_get_fd_for_plane(current_bo, plane);
      if (fd < 0) {
        BOOST_LOG(error) << "Failed to get buffer FD for plane "sv << plane;
        zwp_linux_buffer_params_v1_destroy(params);
        next_frame->destroy();
        gbm_bo_destroy(current_bo);
        current_bo = nullptr;
        give_up();
        return;
      }

      const std::uint32_t stride = gbm_bo_get_stride_for_plane(current_bo, plane);
      const std::uint32_t offset = gbm_bo_get_offset(current_bo, plane);

      next_frame->sd.fds[plane] = fd;
      next_frame->sd.pitches[plane] = stride;
      next_frame->sd.offsets[plane] = offset;

      zwp_linux_buffer_params_v1_add(params, fd, plane, offset, stride, modifier >> 32, modifier & 0xffffffff);
    }

    next_frame->sd.modifier = modifier;
    next_frame->sd.fourcc = icc_session.format;
    next_frame->sd.width = icc_session.width;
    next_frame->sd.height = icc_session.height;

    BOOST_LOG(debug) << "Capture buffer: "sv << icc_session.width << 'x' << icc_session.height
                     << ", "sv << planes << " plane(s), modifier "sv << std::hex << modifier << std::dec;

    zwp_linux_buffer_params_v1_set_user_data(params, this);
    zwp_linux_buffer_params_v1_add_listener(params, &icc_params_listener, this);
    zwp_linux_buffer_params_v1_create(params, icc_session.width, icc_session.height, icc_session.format, 0);
  }

  void dmabuf_t::icc_buffer_params_created(zwp_linux_buffer_params_v1 *params, struct wl_buffer *buffer) {
    current_wl_buffer = buffer;
    zwp_linux_buffer_params_v1_destroy(params);

    if (!icc_session.frame) {
      return;
    }

    ext_image_copy_capture_frame_v1_attach_buffer(icc_session.frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(icc_session.frame, 0, 0, icc_session.width, icc_session.height);
    ext_image_copy_capture_frame_v1_capture(icc_session.frame);
  }

  void dmabuf_t::icc_buffer_params_failed(zwp_linux_buffer_params_v1 *params) {
    note_gbm_failure();
    if (should_log_gbm_failure()) {
      BOOST_LOG(error) << "The compositor refused the capture buffer offered to it"sv;
    }

    zwp_linux_buffer_params_v1_destroy(params);
    cleanup_gbm();

    if (icc_session.frame) {
      ext_image_copy_capture_frame_v1_destroy(icc_session.frame);
      icc_session.frame = nullptr;
    }
    status = REINIT;
  }

  /**
   * @brief Start a fresh round of session constraints.
   *
   * A compositor may renegotiate: it sends the whole set again, then done. The
   * accumulated formats and the latched preferred format are only valid for the
   * round they arrived in, so keeping them across rounds means allocating in a
   * format the compositor has since dropped.
   */
  void dmabuf_t::icc_begin_constraints() {
    if (!icc_session.done) {
      return;
    }
    icc_session.done = false;
    icc_session.format = 0;
    icc_session.dmabuf_supported = false;
    icc_session.modifiers.clear();
    shm_info.supported = false;
  }

  void dmabuf_t::icc_session_buffer_size(ext_image_copy_capture_session_v1 *session, std::uint32_t width, std::uint32_t height) {
    icc_begin_constraints();
    icc_session.width = width;
    icc_session.height = height;
    BOOST_LOG(debug) << "Capture session buffer size: "sv << width << 'x' << height;
  }

  void dmabuf_t::icc_session_shm_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format) {
    icc_begin_constraints();
    // Recorded for the same reason the screencopy path records it: to be able to
    // say the compositor offered only shared memory, which nothing here can use.
    shm_info.supported = true;
    shm_info.format = format;
    BOOST_LOG(debug) << "Capture session SHM format: "sv << format;
  }

  void dmabuf_t::icc_session_dmabuf_device(ext_image_copy_capture_session_v1 *session, struct wl_array *device) {
    icc_begin_constraints();
    // The compositor names the device it renders on, per session, which is the
    // answer init_gbm() otherwise has to guess at. It is trusted the same way
    // as the linux-dmabuf feedback answer: init_gbm() proves the node can
    // allocate and falls through to the search when it cannot.
    if (device->size != sizeof(dev_t)) {
      BOOST_LOG(warning) << "Capture session named a device of "sv << device->size
                         << " bytes, expected "sv << sizeof(dev_t) << "; searching instead."sv;
      return;
    }

    dev_t device_id;
    std::memcpy(&device_id, device->data, sizeof(dev_t));
    set_render_node(render_node_for_device(device_id));
  }

  void dmabuf_t::icc_session_dmabuf_format(ext_image_copy_capture_session_v1 *session, std::uint32_t format, struct wl_array *modifiers) {
    icc_begin_constraints();
    icc_session.dmabuf_supported = true;

    auto &list = icc_session.modifiers[format];
    list.assign(
      (const std::uint64_t *) modifiers->data,
      (const std::uint64_t *) modifiers->data + modifiers->size / sizeof(std::uint64_t)
    );

    // The compositor sends its preferred format first, so the first one that
    // arrives is the one to allocate in.
    if (!icc_session.format) {
      icc_session.format = format;
    }

    char fourcc[5] {};
    std::memcpy(fourcc, &format, 4);
    BOOST_LOG(debug) << "Capture session DMA-BUF format: "sv << fourcc << " with "sv << list.size() << " modifier(s)"sv;
  }

  void dmabuf_t::icc_session_done(ext_image_copy_capture_session_v1 *session) {
    if (!icc_session.session) {
      note_gbm_failure();
      status = REINIT;
      return;
    }

    // Each of these is a session that will negotiate the same way every time,
    // so they have to feed the streak: a reinit alone would rebuild the display
    // forever. The screencopy path counts its equivalent failures for the same
    // reason.
    if (!icc_session.width || !icc_session.height) {
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "Capture session negotiated a zero-sized buffer"sv;
      }
      status = REINIT;
      return;
    }

    if (!icc_session.dmabuf_supported) {
      note_gbm_failure();
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The compositor offers capture only as a shared-memory buffer, "sv
                         << "which this capture backend does not implement."sv;
      }
      status = REINIT;
      return;
    }

    icc_session.done = true;
    BOOST_LOG(debug) << "Capture session ready"sv;
  }

  void dmabuf_t::icc_session_stopped(ext_image_copy_capture_session_v1 *session) {
    BOOST_LOG(warning) << "The compositor stopped the capture session"sv;
    icc_destroy_session();
    status = REINIT;
  }

  void dmabuf_t::icc_frame_transform(ext_image_copy_capture_frame_v1 *frame, std::uint32_t transform) {
    BOOST_LOG(debug) << "Capture frame transform: "sv << transform;
  }

  void dmabuf_t::icc_frame_damage(
    ext_image_copy_capture_frame_v1 *frame,
    std::int32_t x,
    std::int32_t y,
    std::int32_t width,
    std::int32_t height
  ) {}

  void dmabuf_t::icc_frame_presentation_time(
    ext_image_copy_capture_frame_v1 *frame,
    std::uint32_t tv_sec_hi,
    std::uint32_t tv_sec_lo,
    std::uint32_t tv_nsec
  ) {}

  void dmabuf_t::icc_frame_ready(ext_image_copy_capture_frame_v1 *frame) {
    clear_gbm_failures();

    current_frame->destroy();
    current_frame = get_next_frame();

    if (current_wl_buffer) {
      wl_buffer_destroy(current_wl_buffer);
      current_wl_buffer = nullptr;
    }
    cleanup_gbm();

    ext_image_copy_capture_frame_v1_destroy(frame);
    icc_session.frame = nullptr;
    status = READY;
  }

  void dmabuf_t::icc_frame_failed(ext_image_copy_capture_frame_v1 *frame, std::uint32_t reason) {
    note_gbm_failure();

    ext_image_copy_capture_frame_v1_destroy(frame);
    icc_session.frame = nullptr;

    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED) {
      if (should_log_gbm_failure()) {
        BOOST_LOG(error) << "The capture session stopped while a frame was in flight"sv;
      }
      cleanup_gbm();
      get_next_frame()->destroy();
      icc_destroy_session();
      status = REINIT;
      return;
    }

    if (reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS) {
      // The negotiated constraints no longer describe the output - a mode change,
      // most often. The session renegotiates them, so drop what we cached.
      if (should_log_gbm_failure()) {
        BOOST_LOG(warning) << "Capture buffer no longer meets the session's constraints; renegotiating"sv;
      }
      cleanup_gbm();
      get_next_frame()->destroy();
      status = REINIT;
      return;
    }

    if (should_log_gbm_failure()) {
      BOOST_LOG(error) << "The compositor failed the capture frame for an unknown reason ("sv << reason << ')';
    }
    // Every path out of here has to release the buffer the frame was carrying.
    // The plane descriptors are the expensive half: a multi-plane modifier
    // buffer holds one fd per plane, and this is the reason a compositor is
    // most likely to hand back, so leaking here leaks at frame rate.
    cleanup_gbm();
    get_next_frame()->destroy();
    status = REINIT;
  }

  void frame_t::destroy() {
    for (auto x = 0; x < 4; ++x) {
      if (sd.fds[x] >= 0) {
        close(sd.fds[x]);

        sd.fds[x] = -1;
      }
    }
  }

  frame_t::frame_t() {
    // File descriptors aren't open
    std::fill_n(sd.fds, 4, -1);
  };

  std::vector<std::unique_ptr<monitor_t>> monitors(const char *display_name) {
    display_t display;

    if (display.init(display_name)) {
      return {};
    }

    interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[interface_t::XDG_OUTPUT]) {
      BOOST_LOG(error) << "Missing Wayland wire XDG_OUTPUT"sv;
      return {};
    }

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    return std::move(interface.monitors);
  }

  bool render_node_can_allocate(const char *node) {
    if (!node || !node[0]) {
      return false;
    }
    int fd = -1;
    gbm_device *device = open_usable_gbm_device(node, fd);
    if (!device) {
      return false;
    }
    gbm_device_destroy(device);
    // gbm_create_device() borrows the fd rather than adopting it.
    ::close(fd);
    return true;
  }

  bool configure_virtual_output(const std::string &output_name, int width, int height, int refresh_rate, bool exclusive, bool hold) {
    std::lock_guard<std::mutex> layout_lock(output_layout_mutex);
    for (int attempt = 0; attempt < 5; ++attempt) {
      auto owned = std::make_unique<output_control_t>();
      auto &control = *owned;
      if (!control.init()) {
        BOOST_LOG(warning) << "Wayland compositor does not expose wlr-output-management; cannot activate virtual output "sv << output_name;
        return false;
      }

      if (!control.collect()) {
        // Not a verdict on the layout, just a layout we have not been told about
        // yet: collect() only succeeds once the manager has sent its first done
        // event. A compositor sitting at zero outputs — a headless session, or a
        // container that has just come up — can still be assembling that state in
        // the roundtrip we make here. Returning now would abandon the activation
        // in the same millisecond the connector was created, before the compositor
        // could possibly have processed the hotplug, so retry on the same footing
        // as a head that has not appeared yet.
        std::this_thread::sleep_for(100ms);
        continue;
      }

      auto *virtual_head = control.find_head(output_name);
      if (!virtual_head) {
        // Virtual DRM connectors are published asynchronously. Give the compositor
        // a short window to process the hotplug before declaring it unavailable.
        std::this_thread::sleep_for(100ms);
        continue;
      }

      std::map<std::string, saved_output_t> previous_layout;
      if (exclusive) {
        for (const auto &head : control.heads) {
          if (head.get() == virtual_head) {
            continue;
          }
          const auto *mode = preferred_or_current_mode(*head);
          previous_layout[head->name] = {
            .enabled = head->enabled,
            .width = mode ? mode->width : 0,
            .height = mode ? mode->height : 0,
            .refresh_rate = mode ? mode->refresh_rate : 0,
            .x = head->x,
            .y = head->y,
            .transform = head->transform,
            .scale = head->scale,
          };
        }
      }

      // A layout already held is being replaced, and its connection must not be
      // the thing that outlives the new one's overrides; it goes once the new
      // one is in.
      const bool applied = apply_configuration(control, *virtual_head, width, height, refresh_rate, exclusive, false, exclusive && hold);
      if (applied && exclusive) {
        saved_output_layout = std::move(previous_layout);
        saved_output_layout_available = true;
        held_layout.reset();
        if (hold) {
          held_layout = std::make_unique<held_layout_t>(std::move(owned), output_name);
        }
      }
      if (!applied) {
        BOOST_LOG(warning) << "Wayland compositor rejected the virtual output layout for "sv << output_name;
      }
      return applied;
    }

    BOOST_LOG(warning) << "Virtual connector "sv << output_name << " did not appear in the Wayland output layout."sv;
    return false;
  }

  bool restore_virtual_output_layout() {
    std::lock_guard<std::mutex> layout_lock(output_layout_mutex);
    if (!saved_output_layout_available) {
      // Nothing to put back, but a connection may still be held for a layout
      // whose saved state is gone; it has no business outliving it.
      held_layout.reset();
      return true;
    }

    // The restore goes through the connection that applied the layout when there
    // is one, so what it says replaces what that connection said; a fresh one
    // would only add to it. The thread servicing it is stopped first, because
    // the apply below reads the connection itself.
    std::unique_ptr<output_control_t> fresh;
    output_control_t *control = nullptr;
    if (held_layout) {
      held_layout->stop();
      if (held_layout->control->collect()) {
        control = held_layout->control.get();
      } else {
        BOOST_LOG(warning) << "The connection holding the exclusive layout is gone; restoring through a new one."sv;
        held_layout.reset();
      }
    }
    if (!control) {
      fresh = std::make_unique<output_control_t>();
      if (!fresh->init() || !fresh->collect()) {
        BOOST_LOG(warning) << "Cannot restore the Wayland physical output layout."sv;
        return false;
      }
      control = fresh.get();
    }
    if (control->heads.empty()) {
      BOOST_LOG(warning) << "Cannot restore the Wayland physical output layout."sv;
      held_layout.reset();
      return false;
    }

    const bool restored = apply_configuration(*control, *control->heads.front(), 0, 0, 0, false, true);
    // Whatever the outcome, the connection goes now: its overrides are what the
    // compositor keeps applying on top of the rules, and a restore that was
    // refused is better left to the rules than to a stale override.
    held_layout.reset();
    if (restored) {
      saved_output_layout.clear();
      saved_output_layout_available = false;
      BOOST_LOG(info) << "Restored Wayland physical output layout."sv;
    } else {
      BOOST_LOG(warning) << "Wayland compositor rejected restoration of the physical output layout."sv;
    }
    return restored;
  }

  bool restore_virtual_output_layout_for(const std::string &output_name) {
    {
      std::lock_guard<std::mutex> layout_lock(output_layout_mutex);
      if (!held_layout || held_layout->output != output_name) {
        return true;
      }
    }
    return restore_virtual_output_layout();
  }

  bool output_management_supported() {
    output_control_t control;
    return control.init() && control.collect();
  }

  session_protocols_t probe_protocols() {
    session_protocols_t protocols {};

    display_t display;
    if (display.init() != 0) {
      // Not a failure worth logging as an error: an X11 session reaching the
      // diagnostics path is the ordinary case, and `connected` says so.
      return protocols;
    }
    protocols.connected = true;

    // A probe must not require bindings for protocols Hermes does not
    // implement, so the two ext-* globals are matched by name. They are staging
    // protocols whose names are part of their wire identity and cannot change
    // without a new global, which is exactly what makes the string safe here.
    static constexpr auto image_copy_capture_name = "ext_image_copy_capture_manager_v1"sv;
    static constexpr auto output_capture_source_name = "ext_output_image_capture_source_manager_v1"sv;

    struct probe_state_t {
      session_protocols_t *protocols;
      wl_registry_listener listener;
    };

    probe_state_t state {
      .protocols = &protocols,
      .listener = {
        .global = [](void *data, wl_registry *, std::uint32_t, const char *interface, std::uint32_t) {
          auto *found = static_cast<probe_state_t *>(data)->protocols;
          const std::string_view name {interface};
          if (name == zwlr_output_manager_v1_interface.name) {
            found->output_management = true;
          } else if (name == zwlr_screencopy_manager_v1_interface.name) {
            found->screencopy = true;
          } else if (name == zwp_linux_dmabuf_v1_interface.name) {
            found->linux_dmabuf = true;
          } else if (name == zxdg_output_manager_v1_interface.name) {
            found->xdg_output = true;
          } else if (name == image_copy_capture_name) {
            found->image_copy_capture = true;
          } else if (name == output_capture_source_name) {
            found->output_capture_source = true;
          }
        },
        .global_remove = [](void *, wl_registry *, std::uint32_t) {},
      },
    };

    wl_registry_add_listener(display.registry(), &state.listener, &state);
    display.roundtrip();

    return protocols;
  }

  static bool validate() {
    display_t display;

    return display.init() == 0;
  }

  int init() {
    static bool validated = validate();

    return !validated;
  }

}  // namespace wl

#pragma GCC diagnostic pop

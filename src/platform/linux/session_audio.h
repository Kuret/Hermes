/**
 * @file src/platform/linux/session_audio.h
 * @brief An isolated session's own PipeWire server.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "src/platform/common.h"

namespace platf::session_audio {

  /**
   * @brief PipeWire and WirePlumber running in a session's runtime directory.
   *
   * The server knows no hardware: its only device is the session's sink. Applications started with the session's
   * XDG_RUNTIME_DIR find it on their own, through PulseAudio, PipeWire, ALSA or
   * JACK, and the host's server never sees them. Stopped on destruction.
   */
  class server_t {
  public:
    virtual ~server_t() = default;

    /// The PULSE_SERVER address of the server.
    virtual std::string pulse_server() const = 0;

    /// The PIPEWIRE_REMOTE address of the server.
    virtual std::string pipewire_remote() const = 0;
  };

  /**
   * @brief Start a server in runtime_dir with sink as its only device.
   * @return nullptr where PipeWire or WirePlumber is missing or fails to start.
   */
  std::unique_ptr<server_t> start(const std::string &runtime_dir, const std::string &sink, int channels);

  /// Record sink's monitor on the server at pulse_server.
  std::unique_ptr<mic_t> microphone(const std::string &pulse_server, const std::string &sink, const std::uint8_t *mapping, int channels, std::uint32_t sample_rate, std::uint32_t frame_size);
}  // namespace platf::session_audio

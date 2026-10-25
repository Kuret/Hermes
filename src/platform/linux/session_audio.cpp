/**
 * @file src/platform/linux/session_audio.cpp
 * @brief An isolated session's own PipeWire server.
 */
#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

// platform includes
#include <fcntl.h>
#include <unistd.h>

// lib includes
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/search_path.hpp>

// local includes
#include "session_audio.h"
#include "src/logging.h"

namespace bp = boost::process::v1;
namespace fs = std::filesystem;
using namespace std::literals;

namespace platf::session_audio {
  namespace {
    std::string positions(int channels) {
      switch (channels) {
        case 6:
          return "FL FR FC LFE RL RR";
        case 8:
          return "FL FR FC LFE RL RR SL SR";
        default:
          return "FL FR";
      }
    }

    // No ALSA, Bluetooth or camera monitors: the sink and the mic below are
    // the only devices. The pulse server takes no D-Bus name, which the
    // host's already owns.
    std::string pipewire_conf(const std::string &sink, int channels, const std::string &mic) {
      std::string conf = R"(context.properties = {
    core.daemon = true
    core.name = pipewire-0
    link.max-buffers = 16
    default.clock.rate = 48000
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    video.convert.* = videoconvert/libspa-videoconvert
    support.* = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-rt args = { nice.level = -11 rt.prio = 88 rtportal.enabled = false } flags = [ ifexists nofail ] }
    { name = libpipewire-module-protocol-native args = { sockets = [ { name = pipewire-0 } { name = pipewire-0-manager } ] } }
    { name = libpipewire-module-metadata }
    { name = libpipewire-module-spa-node-factory }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-access }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-link-factory }
    { name = libpipewire-module-protocol-pulse }
)";
      if (!mic.empty()) {
        conf += R"(    { name = libpipewire-module-pipe-tunnel args = {
        tunnel.mode = source
        pipe.filename = ")" + session_mic_fifo(mic) + R"("
        audio.format = S16LE
        audio.rate = 48000
        audio.channels = 1
        audio.position = [ MONO ]
        stream.props = { node.name = )" + mic + R"( node.description = "Client microphone" }
    } }
)";
      }
      conf += R"(]
pulse.properties = {
    server.address = [ "unix:native" ]
    server.dbus-name = ""
}
context.objects = [
    { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Dummy-Driver node.group = pipewire.dummy priority.driver = 20000 } }
    { factory = spa-node-factory args = { factory.name = support.node.driver node.name = Freewheel-Driver node.group = pipewire.freewheel priority.driver = 19000 node.freewheel = true } }
    { factory = adapter args = {
        factory.name = support.null-audio-sink
        node.name = )" + sink + R"(
        node.description = "Hermes session"
        media.class = Audio/Sink
        audio.position = [ )" + positions(channels) + R"( ]
        monitor.channel-volumes = true
        object.linger = true
    } }
]
)";
      return conf;
    }

    // Linking and default nodes only; nothing is remembered, and nothing
    // talks to the host's session bus (MPRIS pausing would reach its players).
    constexpr auto wireplumber_conf = R"(override.context.modules = [
  { name = libpipewire-module-rt args = { nice.level = -11 rtportal.enabled = false } flags = [ ifexists nofail ] }
  { name = libpipewire-module-protocol-native }
  { name = libpipewire-module-metadata }
]
wireplumber.profiles = {
  hermes-session = {
    inherits = [ policy, mixin.systemwide-session, mixin.stateless ]
    hardware.audio = disabled
    hardware.bluetooth = disabled
    hardware.video-capture = disabled
    support.dbus = disabled
    api.notifications = disabled
    support.mpris = disabled
    hooks.linking.pause-playback = disabled
  }
}
)";

    bool write_file(const fs::path &path, const std::string &contents) {
      std::error_code ec;
      fs::create_directories(path.parent_path(), ec);
      std::ofstream file {path};
      file << contents;
      return file.good();
    }

    void stop(bp::child &child) {
      if (!child.valid() || !child.running()) {
        return;
      }
      ::kill(child.id(), SIGTERM);
      for (int i = 0; i < 100 && child.running(); ++i) {
        std::this_thread::sleep_for(20ms);
      }
      if (child.running()) {
        child.terminate();
      }
    }

    class pipewire_t: public server_t {
    public:
      pipewire_t(std::string runtime_dir, std::string mic):
          runtime_dir {std::move(runtime_dir)},
          mic {std::move(mic)} {
      }

      ~pipewire_t() override {
        stop(wireplumber);
        stop(pipewire);
        if (!mic.empty()) {
          std::error_code ec;
          fs::remove(session_mic_fifo(mic), ec);
        }
      }

      std::string pulse_server() const override {
        return "unix:" + runtime_dir + "/pulse/native";
      }

      std::string pipewire_remote() const override {
        return runtime_dir + "/pipewire-0";
      }

      std::string runtime_dir;
      std::string mic;
      bp::child pipewire;
      bp::child wireplumber;
    };
  }  // namespace

  std::string session_mic_fifo(const std::string &mic) {
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    return (fs::path {runtime && *runtime ? runtime : "/tmp"} / "hermes" / (mic + ".fifo")).string();
  }

  std::unique_ptr<server_t> start(const std::string &runtime_dir, const std::string &sink, int channels, const std::string &mic) {
    const auto pipewire = bp::search_path("pipewire");
    const auto wireplumber = bp::search_path("wireplumber");
    if (pipewire.empty() || wireplumber.empty()) {
      BOOST_LOG(warning) << "PipeWire or WirePlumber is not installed; no private audio server"sv;
      return nullptr;
    }

    const fs::path dir = fs::path {runtime_dir} / "audio";
    // Not named pipewire.conf, so no system or user fragments apply.
    const auto conf = dir / "hermes-session.conf";
    if (!write_file(conf, pipewire_conf(sink, channels, mic)) ||
        !write_file(dir / "config/wireplumber/wireplumber.conf.d/hermes-session.conf", wireplumber_conf)) {
      BOOST_LOG(warning) << "Couldn't write the private audio server's config to "sv << dir.string();
      return nullptr;
    }
    std::error_code ec;
    if (!mic.empty()) {
      fs::create_directories(fs::path {session_mic_fifo(mic)}.parent_path(), ec);
      fs::remove(session_mic_fifo(mic), ec);
    }

    // The user's own PipeWire config and WirePlumber state stay out of it.
    auto env = bp::environment {boost::this_process::environment()};
    for (const auto *name : {"PIPEWIRE_REMOTE", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_CONFIG_DIR", "PULSE_SERVER", "PULSE_RUNTIME_PATH", "WIREPLUMBER_CONFIG_DIR", "DBUS_SESSION_BUS_ADDRESS", "DISPLAY", "WAYLAND_DISPLAY"}) {
      env.erase(name);
    }
    env["XDG_RUNTIME_DIR"] = runtime_dir;
    env["XDG_CONFIG_HOME"] = (dir / "config").string();
    env["XDG_STATE_HOME"] = (dir / "state").string();

    auto server = std::make_unique<pipewire_t>(runtime_dir, mic);
    server->pipewire = bp::child(pipewire, "-c", conf.string(), env, bp::std_in < bp::null, bp::limit_handles, ec);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't start PipeWire: "sv << ec.message();
      return nullptr;
    }

    // Applications start once the pulse socket exists; it is created last.
    const auto socket = fs::path {runtime_dir} / "pulse" / "native";
    for (auto deadline = std::chrono::steady_clock::now() + 5s; !fs::exists(socket, ec);) {
      if (!server->pipewire.running() || std::chrono::steady_clock::now() > deadline) {
        BOOST_LOG(warning) << "The private PipeWire server didn't come up in "sv << runtime_dir;
        return nullptr;
      }
      std::this_thread::sleep_for(20ms);
    }

    server->wireplumber = bp::child(wireplumber, "-p", "hermes-session", env, bp::std_in < bp::null, bp::limit_handles, ec);
    if (ec) {
      BOOST_LOG(warning) << "Couldn't start WirePlumber: "sv << ec.message();
      return nullptr;
    }

    BOOST_LOG(info) << "Started a private PipeWire server in "sv << runtime_dir << " with sink ["sv << sink << ']'
                    << (mic.empty() ? ""s : " and mic ["s + mic + ']');
    return server;
  }

  namespace {
    class fifo_mic_writer_t: public mic_writer_t {
    public:
      fifo_mic_writer_t(int fd, std::string mic):
          fd {fd},
          mic {std::move(mic)} {
      }

      ~fifo_mic_writer_t() override {
        close(fd);
      }

      // The caller blocks SIGPIPE: the FIFO loses its reader with its server.
      // While nobody records, the full FIFO drops writes.
      void write(const std::int16_t *samples, int count) override {
        // Writes up to PIPE_BUF are all or nothing, so a full FIFO never
        // splits a sample.
        constexpr int chunk = PIPE_BUF / sizeof(std::int16_t);
        for (int offset = 0; offset < count; offset += chunk) {
          const auto bytes = std::min(chunk, count - offset) * sizeof(std::int16_t);
          if (::write(fd, samples + offset, bytes) < 0 && errno == EPIPE && !broken) {
            broken = true;
            BOOST_LOG(warning) << "Session mic ["sv << mic << "] is gone; dropping the client's microphone"sv;
          }
        }
      }

    private:
      int fd;
      std::string mic;
      bool broken {};
    };
  }  // namespace

  std::unique_ptr<mic_writer_t> open_mic(const std::string &mic) {
    const auto fifo = session_mic_fifo(mic);
    const int fd = open(fifo.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
      BOOST_LOG(warning) << "Couldn't open session mic ["sv << fifo << "]: "sv << std::strerror(errno);
      return nullptr;
    }
    return std::make_unique<fifo_mic_writer_t>(fd, mic);
  }
}  // namespace platf::session_audio

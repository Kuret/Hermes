/**
 * @file tests/unit/test_network.cpp
 * @brief Test src/network.*
 */
#include <gtest/gtest.h>
#include <src/network.h>
#include <src/thread_safe.h>

#ifndef _WIN32
  #include <fcntl.h>
#endif

#ifdef __linux__
  #include <csignal>
  #include <spawn.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {
  std::uint16_t bound_port(ENetHost &host) {
    ENetAddress address {};
    if (enet_socket_get_address(host.socket, &address) != 0) {
      return 0;
    }
    if (address.address.ss_family == AF_INET) {
      return ntohs(reinterpret_cast<const sockaddr_in *>(&address.address)->sin_port);
    }
    return ntohs(reinterpret_cast<const sockaddr_in6 *>(&address.address)->sin6_port);
  }
}  // namespace

struct MdnsInstanceNameTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(MdnsInstanceNameTest, Run) {
  auto [input, expected] = GetParam();
  ASSERT_EQ(net::mdns_instance_name(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  MdnsInstanceNameTests,
  MdnsInstanceNameTest,
  testing::Values(
    std::make_tuple("shortname-123", "shortname-123"),
    std::make_tuple("space 123", "space-123"),
    std::make_tuple("hostname.domain.test", "hostname"),
    std::make_tuple("&", "Hermes"),
    std::make_tuple("", "Hermes"),
    std::make_tuple("😁", "Hermes"),
    std::make_tuple(std::string(128, 'a'), std::string(63, 'a'))
  )
);

/**
 * A host that cannot be created must come back null, not crash.
 *
 * enet returns null when it cannot create or bind the socket, and the port
 * already being taken is the ordinary way that happens. `host_create` used to
 * set a socket option on the result before checking it, so a bind failure at
 * session start was a segfault rather than the "couldn't bind" the caller is
 * written to report — the crash in #33.
 */
struct HostCreateTest: testing::TestWithParam<net::af_e> {};

TEST_P(HostCreateTest, SurvivesAPortThatIsAlreadyTaken) {
  const auto af = GetParam();
  ENetAddress first_addr {};
  ENetAddress second_addr {};

  // A port in the ephemeral range, so this does not collide with a real
  // Hermes on the machine running the tests.
  constexpr std::uint16_t port = 47989 + 10 + 1000;

  auto first = net::host_create(af, first_addr, port);
  if (!first) {
    GTEST_SKIP() << "could not bind the probe port; nothing to contend with";
  }

  // The second bind fails, and that has to be survivable.
  auto second = net::host_create(af, second_addr, port);
  EXPECT_FALSE(static_cast<bool>(second));
}

INSTANTIATE_TEST_SUITE_P(
  HostCreateTests,
  HostCreateTest,
  testing::Values(net::IPV4, net::BOTH),
  [](const auto &info) {
    return info.param == net::IPV4 ? "ipv4" : "both";
  }
);

/**
 * The port probe the Web UI asks, so a conflict is visible before anyone tries
 * to play rather than as a stream that fails for no stated reason.
 */
struct UdpPortAvailableTest: testing::TestWithParam<net::af_e> {};

TEST_P(UdpPortAvailableTest, ReportsAPortThatSomethingElseHolds) {
  const auto af = GetParam();
  constexpr std::uint16_t port = 47989 + 9 + 1000;

  ENetAddress addr {};
  auto holder = net::host_create(af, addr, port);
  if (!holder) {
    GTEST_SKIP() << "could not bind the probe port; nothing to contend with";
  }
  EXPECT_FALSE(net::udp_port_available(af, port));

  holder.reset();
  EXPECT_TRUE(net::udp_port_available(af, port));
}

INSTANTIATE_TEST_SUITE_P(
  UdpPortAvailableTests,
  UdpPortAvailableTest,
  testing::Values(net::IPV4, net::BOTH),
  [](const auto &info) {
    return info.param == net::IPV4 ? "ipv4" : "both";
  }
);

/**
 * The wildcard address must be built, not resolved.
 *
 * enet resolves names through getaddrinfo with AI_ADDRCONFIG, which refuses to
 * return "::" on a host whose only IPv6 address is on loopback — and reports
 * that through a return value the caller never read, leaving the address as it
 * found it. The bind then went out with a length of zero and failed, on a host
 * where binding the wildcard would have worked perfectly (#33).
 *
 * Poisoning the address first is the point: it has to be fully overwritten,
 * which a failed resolution would not do.
 */
TEST_P(HostCreateTest, BuildsTheWildcardAddressWithoutResolvingIt) {
  const auto af = GetParam();
  constexpr std::uint16_t port = 47989 + 10 + 1001;

  ENetAddress addr {};
  std::memset(&addr, 0xAB, sizeof(addr));

  auto host = net::host_create(af, addr, port);
  if (!host) {
    GTEST_SKIP() << "could not bind the probe port";
  }

  const auto expected_family = af == net::IPV4 ? AF_INET : AF_INET6;
  EXPECT_EQ(addr.address.ss_family, expected_family);
  EXPECT_EQ(addr.addressLength, af == net::IPV4 ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));

  if (af == net::IPV4) {
    const auto *sin = reinterpret_cast<const sockaddr_in *>(&addr.address);
    EXPECT_EQ(ntohs(sin->sin_port), port);
    EXPECT_EQ(sin->sin_addr.s_addr, htonl(INADDR_ANY));
  } else {
    const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(&addr.address);
    EXPECT_EQ(ntohs(sin6->sin6_port), port);
    EXPECT_EQ(std::memcmp(&sin6->sin6_addr, &in6addr_any, sizeof(in6addr_any)), 0);
  }
}

TEST_P(HostCreateTest, ReportsTheSocketErrorAndClearsItOnRetry) {
  ENetAddress address {};
  boost::system::error_code error;
  auto holder = net::host_create(GetParam(), address, 0, &error);
  if (!holder) {
    GTEST_SKIP() << "address family unavailable: " << error.message();
  }
  const auto port = bound_port(*holder);
  ASSERT_NE(port, 0);
  auto conflict = net::host_create(GetParam(), address, port, &error);
  EXPECT_FALSE(static_cast<bool>(conflict));
  EXPECT_EQ(error, boost::asio::error::address_in_use);

  holder.reset();
  auto retry = net::host_create(GetParam(), address, port, &error);
  ASSERT_TRUE(static_cast<bool>(retry)) << error.message();
  EXPECT_FALSE(static_cast<bool>(error));
}

TEST_P(HostCreateTest, RepeatedlyReleasesAndRebindsTheSamePort) {
  ENetAddress address {};
  boost::system::error_code error;
  auto host = net::host_create(GetParam(), address, 0, &error);
  if (!host) {
    GTEST_SKIP() << "address family unavailable: " << error.message();
  }
  const auto port = bound_port(*host);
  ASSERT_NE(port, 0);
  for (int attempt = 0; attempt < 20; ++attempt) {
    host.reset();
    host = net::host_create(GetParam(), address, port, &error);
    ASSERT_TRUE(static_cast<bool>(host)) << "attempt " << attempt << ": " << error.message();
  }
}

#ifndef _WIN32
TEST_P(HostCreateTest, MarksTheControlSocketCloseOnExec) {
  ENetAddress address {};
  auto host = net::host_create(GetParam(), address, 0);
  if (!host) {
    GTEST_SKIP() << "address family unavailable";
  }
  const int flags = fcntl(host->socket, F_GETFD);
  ASSERT_NE(flags, -1);
  EXPECT_NE(flags & FD_CLOEXEC, 0);
}
#endif

#ifdef __linux__
TEST_P(HostCreateTest, AnExecutedHelperDoesNotKeepTheControlPortAlive) {
  ENetAddress address {};
  auto host = net::host_create(GetParam(), address, 0);
  if (!host) {
    GTEST_SKIP() << "address family unavailable";
  }
  const auto port = bound_port(*host);
  ASSERT_NE(port, 0);

  pid_t child;
  char program[] = "/bin/sleep";
  char duration[] = "30";
  char *arguments[] = {program, duration, nullptr};
  ASSERT_EQ(posix_spawn(&child, program, nullptr, nullptr, arguments, environ), 0);
  auto cleanup = util::fail_guard([child]() {
    kill(child, SIGTERM);
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
  });

  // The helper is still alive, but releasing Hermes' host must free the port.
  host.reset();
  boost::system::error_code error;
  auto retry = net::host_create(GetParam(), address, port, &error);
  EXPECT_TRUE(static_cast<bool>(retry)) << error.message();
}
#endif

TEST(NetworkStartupRollbackTest, FailedSharedInitializationReleasesAnAcquiredPort) {
  struct context_t {
    net::host_t host;
  };

  ENetAddress address {};
  std::uint16_t port = 0;
  bool acquired = false;
  bool fail = true;
  int shutdowns = 0;
  auto shared = safe::make_shared<context_t>([&](context_t &context) {
    context.host = net::host_create(net::IPV4, address, port);
    if (!context.host) {
      return -1;
    }
    acquired = true;
    port = bound_port(*context.host);
    return fail ? -1 : 0;
  },
                                             [&](context_t &) {
                                               ++shutdowns;
                                             });

  auto failed = shared.ref();
  ASSERT_TRUE(acquired);
  ASSERT_NE(port, 0);
  EXPECT_FALSE(static_cast<bool>(failed));
  EXPECT_EQ(shutdowns, 0);  // Full shutdown requires a completed startup.
  EXPECT_TRUE(net::udp_port_available(net::IPV4, port));

  fail = false;
  auto retry = shared.ref();
  ASSERT_TRUE(static_cast<bool>(retry));
  auto second_reference = retry;
  retry.release();
  EXPECT_FALSE(net::udp_port_available(net::IPV4, port));
  EXPECT_EQ(shutdowns, 0);
  second_reference.release();
  EXPECT_TRUE(net::udp_port_available(net::IPV4, port));
  EXPECT_EQ(shutdowns, 1);
}

/**
 * @file tests/unit/platform/test_virtual_display_hyprland.cpp
 * @brief Test the monitor rule Hermes pins on a Hyprland headless output.
 *
 * Hyprland matches a rule against the output by name and then by mode, so a
 * rule with a mode it does not recognise is a rule that never applies. The
 * string is checked here, without a compositor, because that is where a
 * locale once turned "@120.000" into "@120,000".
 */
#include "../../tests_common.h"

#include <src/platform/linux/virtual_display.h>

#include <clocale>
#include <string>

namespace {
  /** Switches the numeric locale for one test and restores it afterwards. */
  class numeric_locale_guard {
  public:
    explicit numeric_locale_guard(const char *name) {
      const char *previous = std::setlocale(LC_NUMERIC, nullptr);
      previous_ = previous ? previous : "C";
      active_ = std::setlocale(LC_NUMERIC, name) != nullptr;
    }

    ~numeric_locale_guard() {
      std::setlocale(LC_NUMERIC, previous_.c_str());
    }

    bool active() const {
      return active_;
    }

  private:
    std::string previous_;
    bool active_ {false};
  };
}  // namespace

TEST(HyprlandModeString, CarriesTheRefreshInMillihertz) {
  EXPECT_EQ(VDISPLAY::hyprlandModeString(2448, 1848, 120000), "2448x1848@120.000");
  EXPECT_EQ(VDISPLAY::hyprlandModeString(1920, 1080, 59940), "1920x1080@59.940");
}

TEST(HyprlandModeString, PadsTheFractionToThreeDigits) {
  EXPECT_EQ(VDISPLAY::hyprlandModeString(1920, 1080, 60001), "1920x1080@60.001");
  EXPECT_EQ(VDISPLAY::hyprlandModeString(1920, 1080, 60050), "1920x1080@60.050");
}

TEST(HyprlandModeString, LeavesOutAnUnknownRefresh) {
  EXPECT_EQ(VDISPLAY::hyprlandModeString(1280, 720, 0), "1280x720");
  EXPECT_EQ(VDISPLAY::hyprlandModeString(1280, 720, -1), "1280x720");
}

TEST(HyprlandModeString, IgnoresTheNumericLocale) {
  // Any locale whose decimal separator is a comma will do; skip where none is installed.
  for (const char *name : {"de_DE.UTF-8", "nl_NL.UTF-8", "fr_FR.UTF-8", "de_DE", "nl_NL"}) {
    numeric_locale_guard guard {name};
    if (guard.active()) {
      EXPECT_EQ(VDISPLAY::hyprlandModeString(2448, 1848, 120000), "2448x1848@120.000") << name;
      return;
    }
  }
  GTEST_SKIP() << "no locale with a decimal comma is installed";
}

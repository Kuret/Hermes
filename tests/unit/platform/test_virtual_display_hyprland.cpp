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

TEST(HyprlandMonitorRule, SdrIsTenBitWithoutAColorManagementPreset) {
  EXPECT_EQ(
    VDISPLAY::hyprlandMonitorRule("HEADLESS-2", 2448, 1848, 120000, false),
    R"(hl.monitor({ output = "HEADLESS-2", mode = "2448x1848@120.000", position = "auto", scale = 1, bitdepth = 10 }))"
  );
}

TEST(HyprlandMonitorRule, HdrAddsTheHdrPresetAndTheCapabilitiesAHeadlessOutputLacks) {
  EXPECT_EQ(
    VDISPLAY::hyprlandMonitorRule("HEADLESS-2", 2448, 1848, 120000, true),
    R"(hl.monitor({ output = "HEADLESS-2", mode = "2448x1848@120.000", position = "auto", scale = 1, bitdepth = 10, cm = "hdr", supports_hdr = 1, supports_wide_color = 1 }))"
  );
}

TEST(HyprlandMonitorRule, TheRuleIsLocaleIndependent) {
  numeric_locale_guard guard {"nl_NL.UTF-8"};
  if (!guard.active()) {
    GTEST_SKIP() << "nl_NL.UTF-8 is not installed";
  }
  EXPECT_NE(VDISPLAY::hyprlandMonitorRule("HEADLESS-2", 2448, 1848, 120000, true).find("@120.000"), std::string::npos);
  EXPECT_NE(VDISPLAY::hyprlandKeywordMonitorRule("HEADLESS-2", 2448, 1848, 120000, true).find("@120.000"), std::string::npos);
}

TEST(HyprlandMonitorRule, KeywordFormUsesTheLegacySyntax) {
  EXPECT_EQ(
    VDISPLAY::hyprlandKeywordMonitorRule("HEADLESS-2", 1920, 1080, 60000, false),
    "HEADLESS-2,1920x1080@60.000,auto,1,bitdepth,10"
  );
  EXPECT_EQ(
    VDISPLAY::hyprlandKeywordMonitorRule("HEADLESS-2", 1920, 1080, 60000, true),
    "HEADLESS-2,1920x1080@60.000,auto,1,bitdepth,10,cm,hdr,supports_hdr,1,supports_wide_color,1"
  );
}

TEST(HyprlandMonitorState, ReadsTheNamedOutput) {
  const std::string monitors = R"([
    {"name": "DP-3", "width": 3440, "height": 1440, "refreshRate": 144.00000, "colorManagementPreset": "srgb"},
    {"name": "HEADLESS-2", "width": 2448, "height": 1848, "refreshRate": 119.99900, "colorManagementPreset": "hdr"}
  ])";

  const auto state = VDISPLAY::hyprlandMonitorState(monitors, "HEADLESS-2");
  ASSERT_TRUE(state);
  EXPECT_EQ(state->width, 2448);
  EXPECT_EQ(state->height, 1848);
  EXPECT_EQ(state->refresh_mhz, 119999);
  EXPECT_EQ(state->color_preset, "hdr");

  const auto physical = VDISPLAY::hyprlandMonitorState(monitors, "DP-3");
  ASSERT_TRUE(physical);
  EXPECT_EQ(physical->refresh_mhz, 144000);
  EXPECT_EQ(physical->color_preset, "srgb");
}

TEST(HyprlandMonitorState, AnUnknownOutputOrBadTextIsNothing) {
  EXPECT_FALSE(VDISPLAY::hyprlandMonitorState(R"([{"name": "DP-3"}])", "HEADLESS-2"));
  EXPECT_FALSE(VDISPLAY::hyprlandMonitorState("not json", "HEADLESS-2"));
  EXPECT_FALSE(VDISPLAY::hyprlandMonitorState(R"({"name": "HEADLESS-2"})", "HEADLESS-2"));
}

TEST(HyprlandMonitorState, ABuildWithoutColorManagementHasNoPreset) {
  const auto state = VDISPLAY::hyprlandMonitorState(R"([{"name": "HEADLESS-2", "width": 800, "height": 600}])", "HEADLESS-2");
  ASSERT_TRUE(state);
  EXPECT_TRUE(state->color_preset.empty());
  EXPECT_EQ(state->refresh_mhz, 0);
}

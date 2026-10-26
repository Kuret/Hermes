/**
 * @file tests/unit/platform/test_wayland_color.cpp
 * @brief Test the image description to HDR metadata conversion in src/platform/linux/wayland.cpp.
 *
 * No compositor is needed. These pin the units: the color-management protocol
 * carries xy chromaticities times 1,000,000 and minimum luminance times
 * 10,000, and SS_HDR_METADATA, like the KMS capture that fills it from a DRM
 * infoframe, wants xy times 50,000 and the same 1/10000 cd/m^2 minimum.
 */
#include "../../tests_common.h"

#ifdef SUNSHINE_BUILD_WAYLAND

  #include <cstring>

  #include <src/platform/linux/wayland.h>

namespace {
  wl::color_info_t pq_description() {
    wl::color_info_t info;
    info.transfer_function = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
    info.done = true;
    return info;
  }
}  // namespace

TEST(WaylandColor, SdrHasNoHdrMetadata) {
  wl::color_info_t info;
  info.transfer_function = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
  info.target_max_lum = 1000;

  SS_HDR_METADATA metadata;
  std::memset(&metadata, 0xff, sizeof(metadata));
  EXPECT_FALSE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 0);
  EXPECT_EQ(metadata.displayPrimaries[0].x, 0);
  EXPECT_FALSE(info.pq());
}

TEST(WaylandColor, AnUndescribedOutputIsNotHdr) {
  EXPECT_FALSE(wl::color_info_t {}.pq());
}

TEST(WaylandColor, PqWithoutPrimariesFallsBackToBt2020) {
  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(pq_description(), metadata));

  // 0.708, 0.292 / 0.170, 0.797 / 0.131, 0.046, white 0.3127, 0.3290, at 50,000.
  EXPECT_EQ(metadata.displayPrimaries[0].x, 35400);
  EXPECT_EQ(metadata.displayPrimaries[0].y, 14600);
  EXPECT_EQ(metadata.displayPrimaries[1].x, 8500);
  EXPECT_EQ(metadata.displayPrimaries[1].y, 39850);
  EXPECT_EQ(metadata.displayPrimaries[2].x, 6550);
  EXPECT_EQ(metadata.displayPrimaries[2].y, 2300);
  EXPECT_EQ(metadata.whitePoint.x, 15635);
  EXPECT_EQ(metadata.whitePoint.y, 16450);
}

TEST(WaylandColor, TargetPrimariesWinOverEncodedPrimaries) {
  auto info = pq_description();
  info.primaries = wl::color_info_t::chromaticities_t {640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000};
  info.target_primaries = wl::color_info_t::chromaticities_t {680000, 320000, 265000, 690000, 150000, 60000, 314000, 351000};

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 34000);
  EXPECT_EQ(metadata.whitePoint.x, 15700);
  EXPECT_EQ(metadata.whitePoint.y, 17550);

  info.target_primaries.reset();
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 32000);
}

TEST(WaylandColor, ANamedSetStandsInWhenNothingIsExplicit) {
  auto info = pq_description();
  info.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3;

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 34000);
  EXPECT_EQ(metadata.displayPrimaries[1].y, 34500);
}

TEST(WaylandColor, ChromaticitiesRoundToNearest) {
  auto info = pq_description();
  // 9 / 20 is below a half step and 10 / 20 is exactly one.
  info.target_primaries = wl::color_info_t::chromaticities_t {9, 10, 312710, 312709, 0, 0, 0, 0};

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.displayPrimaries[0].x, 0);
  EXPECT_EQ(metadata.displayPrimaries[0].y, 1);
  EXPECT_EQ(metadata.displayPrimaries[1].x, 15636);
  EXPECT_EQ(metadata.displayPrimaries[1].y, 15635);
}

TEST(WaylandColor, LuminanceKeepsTheProtocolUnits) {
  auto info = pq_description();
  info.target_min_lum = 50;  // 0.005 cd/m^2
  info.target_max_lum = 1000;
  info.target_max_cll = 800;
  info.target_max_fall = 400;

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 1000);
  EXPECT_EQ(metadata.minDisplayLuminance, 50);
  EXPECT_EQ(metadata.maxContentLightLevel, 800);
  EXPECT_EQ(metadata.maxFrameAverageLightLevel, 400);
}

TEST(WaylandColor, TargetLuminanceWinsOverTheEncodedRange) {
  auto info = pq_description();
  info.min_lum = 10;
  info.max_lum = 10000;

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 10000);
  EXPECT_EQ(metadata.minDisplayLuminance, 10);
  EXPECT_EQ(metadata.maxContentLightLevel, 0);

  info.target_min_lum = 0;
  info.target_max_lum = 600;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 600);
  EXPECT_EQ(metadata.minDisplayLuminance, 0);
}

TEST(WaylandColor, OversizedLuminanceClampsInsteadOfWrapping) {
  auto info = pq_description();
  info.target_max_lum = 100000;
  info.target_min_lum = 70000;

  SS_HDR_METADATA metadata;
  ASSERT_TRUE(wl::hdr_metadata_from_color_info(info, metadata));
  EXPECT_EQ(metadata.maxDisplayLuminance, 65535);
  EXPECT_EQ(metadata.minDisplayLuminance, 65535);
}

TEST(WaylandColor, OnlyAnEncodingDifferenceCountsAsAChange) {
  auto first = pq_description();
  first.target_max_lum = 1000;

  auto same = first;
  same.done = false;
  EXPECT_TRUE(first.same_encoding(same));

  auto sdr = first;
  sdr.transfer_function = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB;
  EXPECT_FALSE(first.same_encoding(sdr));

  auto dimmer = first;
  dimmer.target_max_lum = 600;
  EXPECT_FALSE(first.same_encoding(dimmer));
}

#endif

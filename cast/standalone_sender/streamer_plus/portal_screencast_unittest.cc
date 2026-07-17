// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streamer_plus/portal_screencast.h"

#include <optional>

#include <glib.h>

#include "gtest/gtest.h"

namespace openscreen::cast::streamer_plus {
namespace {

GVariant* Streams(std::initializer_list<std::pair<guint32, GVariant*>> values) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(ua{sv})"));
  for (const auto& [node, properties] : values) {
    g_variant_builder_add(&builder, "(u@a{sv})", node, properties);
  }
  return g_variant_ref_sink(g_variant_builder_end(&builder));
}

GVariant* Properties(std::optional<guint64> serial) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
  if (serial) {
    g_variant_builder_add(&builder, "{sv}", "pipewire-serial",
                          g_variant_new_uint64(*serial));
  }
  return g_variant_builder_end(&builder);
}

TEST(PortalScreencastTest, PrefersValidPipeWireSerial) {
  GVariant* streams = Streams({{7, Properties(9999999999)}});
  PortalStreamTarget target;
  std::string error;
  EXPECT_TRUE(ParsePortalStreamTarget(streams, &target, &error));
  EXPECT_EQ(target.target_object, "9999999999");
  EXPECT_EQ(target.kind, PortalTargetKind::kPipeWireSerial);
  EXPECT_FALSE(target.used_node_id_fallback);
  g_variant_unref(streams);
}

TEST(PortalScreencastTest, FallsBackOnlyWhenSerialIsAbsent) {
  GVariant* streams = Streams({{7, Properties(std::nullopt)}});
  PortalStreamTarget target;
  std::string error;
  ASSERT_TRUE(ParsePortalStreamTarget(streams, &target, &error));
  EXPECT_EQ(target.target_object, "7");
  EXPECT_TRUE(target.used_node_id_fallback);
  g_variant_unref(streams);
}

TEST(PortalScreencastTest, RejectsMalformedStreamLists) {
  PortalStreamTarget target;
  std::string error;
  GVariant* empty = Streams({});
  EXPECT_FALSE(ParsePortalStreamTarget(empty, &target, &error));
  g_variant_unref(empty);
  GVariant* multiple = Streams({{1, Properties(1)}, {2, Properties(2)}});
  EXPECT_FALSE(ParsePortalStreamTarget(multiple, &target, &error));
  g_variant_unref(multiple);
  GVariant* zero_node = Streams({{0, Properties(std::nullopt)}});
  EXPECT_FALSE(ParsePortalStreamTarget(zero_node, &target, &error));
  g_variant_unref(zero_node);
  GVariant* wrong_type = g_variant_new_string("not streams");
  EXPECT_FALSE(ParsePortalStreamTarget(wrong_type, &target, &error));
}

TEST(PortalScreencastTest, RejectsInvalidSerialValuesAndTypes) {
  GVariantBuilder properties;
  g_variant_builder_init(&properties, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&properties, "{sv}", "pipewire-serial",
                        g_variant_new_string("bad"));
  GVariant* wrong_type = Streams({{1, g_variant_builder_end(&properties)}});
  PortalStreamTarget target;
  std::string error;
  EXPECT_FALSE(ParsePortalStreamTarget(wrong_type, &target, &error));
  g_variant_unref(wrong_type);
  GVariant* zero = Streams({{1, Properties(0)}});
  EXPECT_FALSE(ParsePortalStreamTarget(zero, &target, &error));
  g_variant_unref(zero);
}

}  // namespace
}  // namespace openscreen::cast::streamer_plus

// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/device_runner/cobalt/cobalt.h"

#include <fuchsia/cobalt/cpp/fidl.h>
#include <lib/cobalt/cpp/cobalt_logger.h>
#include <lib/fit/function.h>

namespace modular {
namespace {
constexpr char kConfigBinProtoPath[] = "/pkg/data/cobalt_config.binproto";

cobalt::CobaltLogger* g_cobalt_logger = nullptr;

}  // namespace

fxl::AutoCall<fit::closure> InitializeCobalt(
    async_dispatcher_t* dispatcher, component::StartupContext* context) {
  FXL_DCHECK(!g_cobalt_logger) << "Cobalt has already been initialized.";

  std::unique_ptr<cobalt::CobaltLogger> cobalt_logger =
      cobalt::NewCobaltLogger(dispatcher, context, kConfigBinProtoPath);

  g_cobalt_logger = cobalt_logger.get();
  return fxl::MakeAutoCall<fit::closure>(
      [cobalt_logger = std::move(cobalt_logger)] {
        g_cobalt_logger = nullptr;
      });
}

void ReportEvent(ModularEvent event) {
  if (g_cobalt_logger) {
    g_cobalt_logger->LogEvent(
        static_cast<uint32_t>(CobaltMetric::MODULAR_EVENTS),
        static_cast<uint32_t>(event));
  }
}

void ReportModuleLaunchTime(std::string module_url, zx::duration time) {
  if (g_cobalt_logger) {
    g_cobalt_logger->LogElapsedTime(
        static_cast<uint32_t>(CobaltMetric::MODULE_LAUNCH_LATENCY), 0,
        module_url, time);
  }
}

void ReportStoryLaunchTime(zx::duration time) {
  if (g_cobalt_logger) {
    g_cobalt_logger->LogElapsedTime(
        static_cast<uint32_t>(CobaltMetric::STORY_LAUNCH_LATENCY), 0, "", time);
  }
}

}  // namespace modular

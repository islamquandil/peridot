// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURES_DEAD_STORY_RANKING_FEATURE_H_
#define PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURES_DEAD_STORY_RANKING_FEATURE_H_

#include <fuchsia/modular/cpp/fidl.h>

#include "peridot/bin/suggestion_engine/ranking_feature.h"

namespace fuchsia {
namespace modular {

class DeadStoryRankingFeature : public RankingFeature {
 public:
  DeadStoryRankingFeature();
  ~DeadStoryRankingFeature() override;

 private:
  double ComputeFeatureInternal(
      const UserInput& query, const RankedSuggestion& suggestion) override;

  ContextSelectorPtr CreateContextSelectorInternal() override;
};

}  // namespace modular
}  // namespace fuchsia

#endif  // PERIDOT_BIN_SUGGESTION_ENGINE_RANKING_FEATURES_DEAD_STORY_RANKING_FEATURE_H_
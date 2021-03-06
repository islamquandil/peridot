// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/ledger/testing/blocking_callback_waiter.h"

#include <memory>

#include <lib/fit/function.h>

#include "gtest/gtest.h"

namespace ledger {
namespace {

class FakeLoopController : public LoopController {
 public:
  FakeLoopController(fit::function<void()> on_run,
                     fit::function<void()> on_stop)
      : on_run_(std::move(on_run)), on_stop_(std::move(on_stop)) {}
  FakeLoopController(const FakeLoopController&) = delete;
  FakeLoopController& operator=(const FakeLoopController&) = delete;

  void RunLoop() override { on_run_(); };

  void StopLoop() override { on_stop_(); };

  std::unique_ptr<SubLoop> StartNewLoop() override {
    FXL_NOTREACHED();
    return nullptr;
  }

  std::unique_ptr<CallbackWaiter> NewWaiter() override {
    return std::make_unique<BlockingCallbackWaiter>(this);
  }

  async_dispatcher_t* dispatcher() override {
    FXL_NOTREACHED();
    return nullptr;
  }

  fit::closure QuitLoopClosure() override {
    FXL_NOTREACHED();
    return []() {};
  }

  bool RunLoopUntil(fit::function<bool()> /* condition */) override {
    FXL_NOTREACHED();
    return false;
  }

  void RunLoopFor(zx::duration /* duration */) override { FXL_NOTREACHED(); }

 private:
  fit::function<void()> on_run_;
  fit::function<void()> on_stop_;
};

TEST(BlockingCallbackWaiterTest, PreCall) {
  size_t nb_run = 0;
  size_t nb_stop = 0;
  FakeLoopController loop_controller([&] { ++nb_run; }, [&] { ++nb_stop; });

  auto waiter = loop_controller.NewWaiter();
  auto callback = waiter->GetCallback();

  callback();
  ASSERT_TRUE(waiter->RunUntilCalled());

  EXPECT_EQ(0u, nb_run);
  EXPECT_EQ(0u, nb_stop);
}

TEST(BlockingCallbackWaiterTest, MultipleGetCallback) {
  size_t nb_run = 0;
  size_t nb_stop = 0;
  FakeLoopController loop_controller([&] { ++nb_run; }, [&] { ++nb_stop; });

  auto waiter = loop_controller.NewWaiter();

  waiter->GetCallback()();
  waiter->GetCallback()();

  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_TRUE(waiter->RunUntilCalled());

  EXPECT_EQ(0u, nb_run);
  EXPECT_EQ(0u, nb_stop);
}

TEST(BlockingCallbackWaiterTest, PostCall) {
  size_t nb_run = 0;
  size_t nb_stop = 0;
  std::unique_ptr<fit::closure> callback;
  FakeLoopController loop_controller(
      [&] {
        ++nb_run;
        if (callback) {
          (*callback)();
        }
      },
      [&] { ++nb_stop; });

  auto waiter = loop_controller.NewWaiter();
  callback = std::make_unique<fit::closure>(waiter->GetCallback());

  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_EQ(1u, nb_run);
  EXPECT_EQ(1u, nb_stop);
}

TEST(BlockingCallbackWaiterTest, MultipleRunUntilCalled) {
  size_t nb_run = 0;
  size_t nb_stop = 0;
  std::unique_ptr<fit::closure> callback;
  FakeLoopController loop_controller(
      [&] {
        ++nb_run;
        if (callback) {
          (*callback)();
        }
      },
      [&] { ++nb_stop; });

  auto waiter = loop_controller.NewWaiter();
  callback = std::make_unique<fit::closure>(waiter->GetCallback());

  ASSERT_TRUE(waiter->RunUntilCalled());
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_EQ(2u, nb_run);
  EXPECT_EQ(2u, nb_stop);
}

TEST(BlockingCallbackWaiterTest, InterleaveRunUntilCalledAndCall) {
  size_t nb_run = 0;
  size_t nb_stop = 0;
  std::unique_ptr<fit::closure> callback;
  FakeLoopController loop_controller(
      [&] {
        ++nb_run;
        if (callback) {
          (*callback)();
        }
      },
      [&] { ++nb_stop; });

  auto waiter = loop_controller.NewWaiter();
  callback = std::make_unique<fit::closure>(waiter->GetCallback());

  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_EQ(1u, nb_run);
  EXPECT_EQ(1u, nb_stop);

  (*callback)();
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_EQ(1u, nb_run);
  EXPECT_EQ(1u, nb_stop);
}

TEST(BlockingCallbackWaiterTest, NotCalledYet) {
  FakeLoopController loop_controller([] {}, [] {});
  auto waiter = loop_controller.NewWaiter();
  auto callback = waiter->GetCallback();

  EXPECT_TRUE(waiter->NotCalledYet());

  callback();
  EXPECT_FALSE(waiter->NotCalledYet());
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_TRUE(waiter->NotCalledYet());

  callback();
  callback();
  EXPECT_FALSE(waiter->NotCalledYet());
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_FALSE(waiter->NotCalledYet());
  ASSERT_TRUE(waiter->RunUntilCalled());
  EXPECT_TRUE(waiter->NotCalledYet());
}

}  // namespace
}  // namespace ledger

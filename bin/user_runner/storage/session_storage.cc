// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/user_runner/storage/session_storage.h"

#include <lib/fidl/cpp/clone.h>
#include <lib/fxl/functional/make_copyable.h>
#include <lib/fxl/random/uuid.h>
#include <unordered_set>

#include "peridot/bin/user_runner/storage/constants_and_utils.h"
#include "peridot/bin/user_runner/storage/session_storage_xdr.h"
#include "peridot/lib/fidl/clone.h"
#include "peridot/lib/ledger_client/operations.h"

namespace modular {

SessionStorage::SessionStorage(LedgerClient* ledger_client,
                               LedgerPageId page_id)
    : PageClient("SessionStorage", ledger_client, page_id, kStoryKeyPrefix),
      ledger_client_(ledger_client) {
  FXL_DCHECK(ledger_client_ != nullptr);
}

namespace {

fidl::StringPtr StoryNameToLedgerKey(fidl::StringPtr story_name) {
  // Not escaped, because only one component after the prefix.
  return kStoryKeyPrefix + story_name.get();
}

fidl::StringPtr StoryNameFromLedgerKey(fidl::StringPtr key) {
  return key->substr(sizeof(kStoryKeyPrefix) - 1);
}

OperationBase* MakeGetStoryDataCall(
    fuchsia::ledger::Page* const page, fidl::StringPtr story_name,
    std::function<void(fuchsia::modular::internal::StoryDataPtr)> result_call) {
  return new ReadDataCall<fuchsia::modular::internal::StoryData>(
      page, StoryNameToLedgerKey(story_name), true /* not_found_is_ok */,
      XdrStoryData, std::move(result_call));
};

OperationBase* MakeWriteStoryDataCall(
    fuchsia::ledger::Page* const page,
    fuchsia::modular::internal::StoryDataPtr story_data,
    std::function<void()> result_call) {
  return new WriteDataCall<fuchsia::modular::internal::StoryData>(
      page, StoryNameToLedgerKey(story_data->story_info.id), XdrStoryData,
      std::move(story_data), std::move(result_call));
};

class CreateStoryCall
    : public LedgerOperation<fidl::StringPtr, fuchsia::ledger::PageId> {
 public:
  CreateStoryCall(
      fuchsia::ledger::Ledger* const ledger,
      fuchsia::ledger::Page* const root_page, fidl::StringPtr story_name,
      fidl::VectorPtr<fuchsia::modular::StoryInfoExtraEntry> extra_info,
      fuchsia::modular::StoryOptions story_options, ResultCall result_call)
      : LedgerOperation("SessionStorage::CreateStoryCall", ledger, root_page,
                        std::move(result_call)),
        story_name_(std::move(story_name)),
        extra_info_(std::move(extra_info)),
        story_options_(std::move(story_options)) {}

 private:
  void Run() override {
    FlowToken flow{this, &story_name_, &story_page_id_};
    ledger()->GetPage(nullptr, story_page_.NewRequest(),
                      Protect([this, flow](fuchsia::ledger::Status status) {
                        if (status != fuchsia::ledger::Status::OK) {
                          FXL_LOG(ERROR) << trace_name() << " "
                                         << "Ledger.GetPage() "
                                         << fidl::ToUnderlying(status);
                        }
                      }));
    story_page_->GetId([this, flow](fuchsia::ledger::PageId id) {
      story_page_id_ = std::move(id);
      Cont(flow);
    });
  }

  void Cont(FlowToken flow) {
    // TODO(security), cf. FW-174. This ID is exposed in public services
    // such as fuchsia::modular::StoryProvider.PreviousStories(),
    // fuchsia::modular::StoryController.GetInfo(),
    // fuchsia::modular::ModuleContext.GetStoryName(). We need to ensure this
    // doesn't expose internal information by being a page ID.
    // TODO(thatguy): Generate a GUID instead.
    if (!story_name_ || story_name_->empty()) {
      story_name_ = fxl::GenerateUUID();
    }

    story_data_ = fuchsia::modular::internal::StoryData::New();
    story_data_->story_name = story_name_;
    story_data_->story_options = std::move(story_options_);
    story_data_->story_page_id = CloneOptional(story_page_id_);
    story_data_->story_info.id = story_name_;
    story_data_->story_info.last_focus_time = 0;
    story_data_->story_info.extra = std::move(extra_info_);

    operation_queue_.Add(MakeWriteStoryDataCall(page(), std::move(story_data_),
                                                [this, flow] {}));
  }

  fidl::StringPtr story_name_;
  fidl::VectorPtr<fuchsia::modular::StoryInfoExtraEntry> extra_info_;
  fuchsia::modular::StoryOptions story_options_;

  fuchsia::ledger::PagePtr story_page_;
  fuchsia::modular::internal::StoryDataPtr story_data_;

  fuchsia::ledger::PageId story_page_id_;

  // Sub operations run in this queue.
  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(CreateStoryCall);
};

}  // namespace

FuturePtr<fidl::StringPtr, fuchsia::ledger::PageId> SessionStorage::CreateStory(
    fidl::StringPtr story_name,
    fidl::VectorPtr<fuchsia::modular::StoryInfoExtraEntry> extra_info,
    fuchsia::modular::StoryOptions story_options) {
  auto ret = Future<fidl::StringPtr, fuchsia::ledger::PageId>::Create(
      "SessionStorage.CreateStory.ret");
  operation_queue_.Add(new CreateStoryCall(
      ledger_client_->ledger(), page(), std::move(story_name),
      std::move(extra_info), std::move(story_options), ret->Completer()));
  return ret;
}

FuturePtr<fidl::StringPtr, fuchsia::ledger::PageId> SessionStorage::CreateStory(
    fidl::VectorPtr<fuchsia::modular::StoryInfoExtraEntry> extra_info,
    fuchsia::modular::StoryOptions story_options) {
  return CreateStory(nullptr /* story_name */, std::move(extra_info),
                     std::move(story_options));
}

namespace {
class DeleteStoryCall : public Operation<> {
 public:
  DeleteStoryCall(fuchsia::ledger::Ledger* const ledger,
                  fuchsia::ledger::Page* const session_page,
                  fidl::StringPtr story_name, ResultCall result_call)
      : Operation("SessionStorage::DeleteStoryCall", std::move(result_call)),
        ledger_(ledger),
        session_page_(session_page),
        story_name_(story_name) {}

 private:
  void Run() override {
    FlowToken flow{this};
    operation_queue_.Add(MakeGetStoryDataCall(
        session_page_, story_name_, [this, flow](auto story_data) {
          if (!story_data)
            return;
          story_data_ = std::move(*story_data);
          Cont1(flow);
        }));
  }

  void Cont1(FlowToken flow) {
    // Get the story page so we can remove its contents.
    ledger_->GetPage(
        std::move(story_data_.story_page_id), story_page_.NewRequest(),
        [this, flow, story_name = story_data_.story_info.id](
            fuchsia::ledger::Status status) {
          if (status != fuchsia::ledger::Status::OK) {
            FXL_LOG(ERROR) << "Ledger.GetPage() for story " << story_name
                           << ": " << fidl::ToUnderlying(status);
            return;
          }
          Cont2(flow);
        });
  }

  void Cont2(FlowToken flow) {
    story_page_->Clear([this, flow, story_name = story_data_.story_info.id](
                           fuchsia::ledger::Status status) {
      if (status != fuchsia::ledger::Status::OK) {
        FXL_LOG(ERROR) << "Page.Clear() for story " << story_name << ": "
                       << fidl::ToUnderlying(status);
        return;
      }
      Cont3(flow);
    });
  }

  void Cont3(FlowToken flow) {
    // Finally remove the entry in the session page.
    session_page_->Delete(
        to_array(StoryNameToLedgerKey(story_data_.story_info.id)),
        [this, flow](fuchsia::ledger::Status status) {
          // Deleting a key that doesn't exist is OK, not
          // KEY_NOT_FOUND.
          if (status != fuchsia::ledger::Status::OK) {
            FXL_LOG(ERROR) << "SessionStorage: Page.Delete() "
                           << fidl::ToUnderlying(status);
          }
        });
  }

  fuchsia::ledger::Ledger* const ledger_;      // not owned
  fuchsia::ledger::Page* const session_page_;  // not owned
  const fidl::StringPtr story_name_;

  // Intermediate state.
  OperationQueue operation_queue_;
  fuchsia::modular::internal::StoryData story_data_;
  fuchsia::ledger::PagePtr story_page_;

  FXL_DISALLOW_COPY_AND_ASSIGN(DeleteStoryCall);
};
}  // namespace

FuturePtr<> SessionStorage::DeleteStory(fidl::StringPtr story_name) {
  auto ret = Future<>::Create("SessionStorage.DeleteStory.ret");
  operation_queue_.Add(new DeleteStoryCall(ledger_client_->ledger(), page(),
                                           story_name, ret->Completer()));
  return ret;
}

namespace {
class MutateStoryDataCall : public Operation<> {
 public:
  MutateStoryDataCall(
      fuchsia::ledger::Page* const page, fidl::StringPtr story_name,
      std::function<bool(fuchsia::modular::internal::StoryData* story_data)>
          mutate,
      ResultCall result_call)
      : Operation("SessionStorage::MutateStoryDataCall",
                  std::move(result_call)),
        page_(page),
        story_name_(story_name),
        mutate_(std::move(mutate)) {}

 private:
  void Run() override {
    FlowToken flow{this};

    operation_queue_.Add(MakeGetStoryDataCall(
        page_, story_name_,
        [this, flow](fuchsia::modular::internal::StoryDataPtr story_data) {
          if (!story_data) {
            // If the story doesn't exist, it was deleted.
            return;
          }
          if (!mutate_(story_data.get())) {
            // If no mutation happened, we're done.
            return;
          }

          operation_queue_.Add(
              MakeWriteStoryDataCall(page_, std::move(story_data), [flow] {}));
        }));
  }

  fuchsia::ledger::Page* const page_;  // not owned
  const fidl::StringPtr story_name_;
  std::function<bool(fuchsia::modular::internal::StoryData* story_data)>
      mutate_;

  OperationQueue operation_queue_;

  FXL_DISALLOW_COPY_AND_ASSIGN(MutateStoryDataCall);
};

}  // namespace

FuturePtr<> SessionStorage::UpdateLastFocusedTimestamp(
    fidl::StringPtr story_name, const int64_t ts) {
  auto mutate = [ts](fuchsia::modular::internal::StoryData* const story_data) {
    if (story_data->story_info.last_focus_time == ts) {
      return false;
    }
    story_data->story_info.last_focus_time = ts;
    return true;
  };

  auto ret = Future<>::Create("SessionStorage.UpdateLastFocusedTimestamp.ret");
  operation_queue_.Add(
      new MutateStoryDataCall(page(), story_name, mutate, ret->Completer()));
  return ret;
}

FuturePtr<fuchsia::modular::internal::StoryDataPtr>
SessionStorage::GetStoryData(fidl::StringPtr story_name) {
  auto ret = Future<fuchsia::modular::internal::StoryDataPtr>::Create(
      "SessionStorage.GetStoryData.ret");
  operation_queue_.Add(
      MakeGetStoryDataCall(page(), story_name, ret->Completer()));
  return ret;
}

// Returns a Future vector of StoryData for all stories in this session.
FuturePtr<fidl::VectorPtr<fuchsia::modular::internal::StoryData>>
SessionStorage::GetAllStoryData() {
  auto ret =
      Future<fidl::VectorPtr<fuchsia::modular::internal::StoryData>>::Create(
          "SessionStorage.GetAllStoryData.ret");
  operation_queue_.Add(
      new ReadAllDataCall<fuchsia::modular::internal::StoryData>(
          page(), kStoryKeyPrefix, XdrStoryData, ret->Completer()));
  return ret;
}

FuturePtr<> SessionStorage::UpdateStoryOptions(
    fidl::StringPtr story_name, fuchsia::modular::StoryOptions story_options) {
  auto ret = Future<>::Create("SessionStorage.SetOptions.ret");
  auto mutate = fxl::MakeCopyable(
      [story_options = std::move(story_options)](
          fuchsia::modular::internal::StoryData* story_data) mutable {
        if (story_data->story_options != story_options) {
          story_data->story_options = std::move(story_options);
          return true;
        }
        return false;
      });
  operation_queue_.Add(new MutateStoryDataCall(
      page(), story_name, std::move(mutate), ret->Completer()));
  return ret;
}

FuturePtr<std::unique_ptr<StoryStorage>> SessionStorage::GetStoryStorage(
    fidl::StringPtr story_name) {
  auto returned_future = Future<std::unique_ptr<StoryStorage>>::Create(
      "SessionStorage.GetStoryStorage.returned_future");

  operation_queue_.Add(MakeGetStoryDataCall(
      page(), story_name,
      [this, returned_future,
       story_name](fuchsia::modular::internal::StoryDataPtr story_data) {
        if (story_data) {
          auto story_storage = std::make_unique<StoryStorage>(
              ledger_client_, *story_data->story_page_id);
          returned_future->Complete(std::move(story_storage));
        } else {
          returned_future->Complete(nullptr);
        }
      }));

  return returned_future;
}

void SessionStorage::OnPageChange(const std::string& key,
                                  const std::string& value) {
  auto story_data = fuchsia::modular::internal::StoryData::New();
  if (!XdrRead(value, &story_data, XdrStoryData)) {
    FXL_LOG(ERROR) << "SessionStorage::OnPageChange : could not decode ledger "
                      "value for key "
                   << key << "\nvalue:\n"
                   << value;
    return;
  }

  auto story_name = StoryNameFromLedgerKey(key);
  if (on_story_updated_) {
    on_story_updated_(std::move(story_name), std::move(*story_data));
  }
}

void SessionStorage::OnPageDelete(const std::string& key) {
  if (on_story_deleted_) {
    // Call to StoryNameFromLedgerKey() needed because a deleted story is
    // modelled by deleting the key, and then the value is not available.
    // TODO(thatguy,mesch): Change PageClient to supply values of deleted keys
    // and/or change modeling of deleted stories.
    on_story_deleted_(StoryNameFromLedgerKey(key));
  }
}

}  // namespace modular

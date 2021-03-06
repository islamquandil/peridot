// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/bin/maxwell/user_intelligence_provider_impl.h"

#include <fuchsia/bluetooth/le/cpp/fidl.h>
#include <fuchsia/cobalt/cpp/fidl.h>
#include <fuchsia/maxwell/internal/cpp/fidl.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/component/cpp/connect.h>
#include <lib/fxl/files/file.h>
#include <lib/fxl/functional/make_copyable.h>

#include "peridot/bin/maxwell/intelligence_services_impl.h"

namespace maxwell {

namespace {

constexpr char kUsageLogUrl[] = "usage_log";
constexpr char kKronkUrl[] = "kronk";
constexpr char kStoryInfoAgentUrl[] = "story_info";
static constexpr modular::RateLimitedRetry::Threshold kSessionAgentRetryLimit =
    {3, zx::sec(45)};

// Calls Duplicate() on an InterfacePtr<> and returns the newly bound
// InterfaceHandle<>.
template <class T>
fidl::InterfaceHandle<T> Duplicate(const fidl::InterfacePtr<T>& ptr) {
  fidl::InterfaceHandle<T> handle;
  ptr->Duplicate(handle.NewRequest());
  return handle;
}

fuchsia::modular::AgentControllerPtr StartStoryInfoAgent(
    fuchsia::modular::ComponentContext* component_context,
    fidl::InterfaceHandle<fuchsia::modular::StoryProvider> story_provider,
    fidl::InterfaceHandle<fuchsia::modular::FocusProvider> focus_provider,
    fidl::InterfaceHandle<fuchsia::modular::VisibleStoriesProvider>
        visible_stories_provider) {
  fuchsia::sys::ServiceProviderPtr agent_services;
  fuchsia::modular::AgentControllerPtr controller;
  component_context->ConnectToAgent(
      kStoryInfoAgentUrl, agent_services.NewRequest(), controller.NewRequest());

  using fuchsia::maxwell::internal::StoryInfoInitializer;
  auto initializer =
      component::ConnectToService<StoryInfoInitializer>(agent_services.get());
  initializer->Initialize(std::move(story_provider), std::move(focus_provider),
                          std::move(visible_stories_provider));

  return controller;
}

fuchsia::modular::ComponentScope CloneScope(
    const fuchsia::modular::ComponentScope& scope) {
  fuchsia::modular::ComponentScope result;
  fidl::Clone(scope, &result);
  return result;
}

}  // namespace

template <class Interface>
UserIntelligenceProviderImpl::SessionAgentData::DeferredInterfaceRequest::
    DeferredInterfaceRequest(fidl::InterfaceRequest<Interface> request)
    : name(Interface::Name_), channel(request.TakeChannel()) {}

UserIntelligenceProviderImpl::SessionAgentData::SessionAgentData()
    : restart(kSessionAgentRetryLimit) {}

template <class Interface>
void UserIntelligenceProviderImpl::SessionAgentData::
    ConnectOrQueueServiceRequest(fidl::InterfaceRequest<Interface> request) {
  if (services) {
    component::ConnectToService(services.get(), std::move(request));
  } else {
    pending_service_requests.emplace_back(std::move(request));
  }
}

UserIntelligenceProviderImpl::UserIntelligenceProviderImpl(
    component::StartupContext* context, const Config& config,
    fidl::InterfaceHandle<fuchsia::modular::ContextEngine>
        context_engine_handle,
    fidl::InterfaceHandle<fuchsia::modular::StoryProvider>
        story_provider_handle,
    fidl::InterfaceHandle<fuchsia::modular::FocusProvider>
        focus_provider_handle,
    fidl::InterfaceHandle<fuchsia::modular::VisibleStoriesProvider>
        visible_stories_provider_handle,
    fidl::InterfaceHandle<fuchsia::modular::PuppetMaster> puppet_master_handle)
    : context_(context), config_(config) {
  context_engine_.Bind(std::move(context_engine_handle));
  story_provider_.Bind(std::move(story_provider_handle));
  focus_provider_.Bind(std::move(focus_provider_handle));
  puppet_master_.Bind(std::move(puppet_master_handle));
  visible_stories_provider_.Bind(std::move(visible_stories_provider_handle));

  // Start dependent processes. We get some component-scope services from
  // these processes.
  suggestion_services_ = StartTrustedApp("suggestion_engine");
  suggestion_engine_ =
      suggestion_services_
          .ConnectToService<fuchsia::modular::SuggestionEngine>();

  // Generate a fuchsia::modular::ContextWriter and
  // fuchsia::modular::ContextReader to pass to the
  // fuchsia::modular::SuggestionEngine.
  fidl::InterfaceHandle<fuchsia::modular::ContextReader> context_reader;
  fidl::InterfaceHandle<fuchsia::modular::ContextWriter> context_writer;
  fuchsia::modular::ComponentScope scope1;
  scope1.set_global_scope(fuchsia::modular::GlobalScope());
  fuchsia::modular::ComponentScope scope2;
  fidl::Clone(scope1, &scope2);
  context_engine_->GetWriter(std::move(scope1), context_writer.NewRequest());
  context_engine_->GetReader(std::move(scope2), context_reader.NewRequest());

  suggestion_engine_->Initialize(std::move(context_writer),
                                 std::move(context_reader),
                                 Duplicate(puppet_master_));
}

void UserIntelligenceProviderImpl::GetComponentIntelligenceServices(
    fuchsia::modular::ComponentScope scope,
    fidl::InterfaceRequest<fuchsia::modular::IntelligenceServices> request) {
  intelligence_services_bindings_.AddBinding(
      std::make_unique<IntelligenceServicesImpl>(
          std::move(scope), context_engine_.get(), suggestion_engine_.get()),
      std::move(request));
}

void UserIntelligenceProviderImpl::GetSuggestionProvider(
    fidl::InterfaceRequest<fuchsia::modular::SuggestionProvider> request) {
  suggestion_services_.ConnectToService(std::move(request));
}

void UserIntelligenceProviderImpl::GetSpeechToText(
    fidl::InterfaceRequest<fuchsia::speech::SpeechToText> request) {
  auto it = session_agents_.find(kKronkUrl);
  if (it != session_agents_.end()) {
    it->second.ConnectOrQueueServiceRequest(std::move(request));
  } else {
    FXL_LOG(WARNING) << "No speech-to-text agent loaded";
  }
}

void UserIntelligenceProviderImpl::StartAgents(
    fidl::InterfaceHandle<fuchsia::modular::ComponentContext>
        component_context_handle) {
  component_context_.Bind(std::move(component_context_handle));

  for (const auto& agent : config_.session_agents) {
    StartSessionAgent(agent);
  }

  for (const auto& agent : config_.startup_agents) {
    StartAgent(agent);
  }

  auto controller = StartStoryInfoAgent(
      component_context_.get(), Duplicate(story_provider_),
      Duplicate(focus_provider_), Duplicate(visible_stories_provider_));
  agent_controllers_.push_back(std::move(controller));
}

void UserIntelligenceProviderImpl::GetServicesForAgent(
    fidl::StringPtr url, GetServicesForAgentCallback callback) {
  fuchsia::sys::ServiceList service_list;
  agent_namespaces_.emplace_back(service_list.provider.NewRequest());
  service_list.names = AddStandardServices(url, &agent_namespaces_.back());
  callback(std::move(service_list));
}

component::Services UserIntelligenceProviderImpl::StartTrustedApp(
    const std::string& url) {
  component::Services services;
  fuchsia::sys::LaunchInfo launch_info;
  launch_info.url = url;
  launch_info.directory_request = services.NewRequest();
  context_->launcher()->CreateComponent(std::move(launch_info), nullptr);
  return services;
}

void UserIntelligenceProviderImpl::StartAgent(const std::string& url) {
  fuchsia::modular::AgentControllerPtr controller;
  fuchsia::sys::ServiceProviderPtr services;
  component_context_->ConnectToAgent(url, services.NewRequest(),
                                     controller.NewRequest());
  agent_controllers_.push_back(std::move(controller));
}

void UserIntelligenceProviderImpl::StartSessionAgent(const std::string& url) {
  SessionAgentData* const agent_data = &session_agents_[url];

  component_context_->ConnectToAgent(url, agent_data->services.NewRequest(),
                                     agent_data->controller.NewRequest());

  fuchsia::modular::SessionAgentInitializerPtr initializer;
  component::ConnectToService(agent_data->services.get(),
                              initializer.NewRequest());
  initializer->Initialize(Duplicate(focus_provider_),
                          Duplicate(puppet_master_));

  // complete any pending connection requests
  for (auto& request : agent_data->pending_service_requests) {
    agent_data->services->ConnectToService(request.name,
                                           std::move(request.channel));
  }
  agent_data->pending_service_requests.clear();

  // fuchsia::modular::Agent runner closes the agent controller connection when
  // the agent terminates. We restart the agent (up to a limit) when we notice
  // this.
  //
  // NOTE(rosswang,mesch): Although the interface we're actually interested in
  // is |data[url].services|, we still need to put the restart handler on the
  // controller. When the agent crashes, |data[url].services| often gets closed
  // quite a bit earlier (~1 second) than the agent runner notices via the
  // application controller (which it must use as opposed to any interface on
  // the agent itself since the agent is not required to implement any
  // interfaces itself, even though it is recommended that it does). If we try
  // to restart the agent at that time, the agent runner would attempt to simply
  // send the connection request to the crashed agent instance and not relaunch
  // the agent.
  //
  // It is also because of this delay that we must queue any pending service
  // connection requests until we can restart.
  agent_data->controller.set_error_handler([this, url] {
    auto it = session_agents_.find(url);
    FXL_DCHECK(it != session_agents_.end())
        << "Controller and services not registered for " << url;
    auto& agent_data = it->second;
    agent_data.services.Unbind();
    agent_data.controller.Unbind();

    if (agent_data.restart.ShouldRetry()) {
      FXL_LOG(INFO) << "Restarting " << url << "...";
      StartSessionAgent(url);
    } else {
      FXL_LOG(WARNING) << url << " failed to restart more than "
                       << kSessionAgentRetryLimit.count << " times in "
                       << kSessionAgentRetryLimit.period.to_secs()
                       << " seconds.";
      // Erase so that incoming connection requests fail fast rather than
      // enqueue forever.
      session_agents_.erase(it);
    }
  });
}

fidl::VectorPtr<fidl::StringPtr>
UserIntelligenceProviderImpl::AddStandardServices(
    const std::string& url, component::ServiceNamespace* agent_host) {
  fuchsia::modular::ComponentScope agent_info;
  fuchsia::modular::AgentScope agent_scope;
  agent_scope.url = url;
  agent_info.set_agent_scope(std::move(agent_scope));
  fidl::VectorPtr<fidl::StringPtr> service_names;

  service_names.push_back(fuchsia::modular::ContextWriter::Name_);
  agent_host->AddService<fuchsia::modular::ContextWriter>(fxl::MakeCopyable(
      [this, client_info = CloneScope(agent_info),
       url](fidl::InterfaceRequest<fuchsia::modular::ContextWriter> request) {
        context_engine_->GetWriter(CloneScope(client_info), std::move(request));
      }));

  service_names.push_back(fuchsia::modular::ContextReader::Name_);
  agent_host->AddService<fuchsia::modular::ContextReader>(fxl::MakeCopyable(
      [this, client_info = CloneScope(agent_info),
       url](fidl::InterfaceRequest<fuchsia::modular::ContextReader> request) {
        context_engine_->GetReader(CloneScope(client_info), std::move(request));
      }));

  service_names.push_back(fuchsia::modular::IntelligenceServices::Name_);
  agent_host->AddService<fuchsia::modular::IntelligenceServices>(
      fxl::MakeCopyable(
          [this, client_info = CloneScope(agent_info),
           url](fidl::InterfaceRequest<fuchsia::modular::IntelligenceServices>
                    request) {
            this->GetComponentIntelligenceServices(CloneScope(client_info),
                                                   std::move(request));
          }));

  service_names.push_back(fuchsia::modular::ProposalPublisher::Name_);
  agent_host->AddService<fuchsia::modular::ProposalPublisher>(
      [this, url](
          fidl::InterfaceRequest<fuchsia::modular::ProposalPublisher> request) {
        suggestion_engine_->RegisterProposalPublisher(url, std::move(request));
      });

  service_names.push_back(fuchsia::modular::VisibleStoriesProvider::Name_);
  agent_host->AddService<fuchsia::modular::VisibleStoriesProvider>(
      [this](fidl::InterfaceRequest<fuchsia::modular::VisibleStoriesProvider>
                 request) {
        visible_stories_provider_->Duplicate(std::move(request));
      });

  if (url == kUsageLogUrl) {
    service_names.push_back(fuchsia::modular::ContextDebug::Name_);
    agent_host->AddService<fuchsia::modular::ContextDebug>(
        [this](fidl::InterfaceRequest<fuchsia::modular::ContextDebug> request) {
          context_engine_->GetContextDebug(std::move(request));
        });

    service_names.push_back(fuchsia::modular::SuggestionDebug::Name_);
    agent_host->AddService<fuchsia::modular::SuggestionDebug>(
        [this](
            fidl::InterfaceRequest<fuchsia::modular::SuggestionDebug> request) {
          suggestion_services_.ConnectToService(std::move(request));
        });
  }

  return service_names;
}

//////////////////////////////////////////////////////////////////////////////

UserIntelligenceProviderFactoryImpl::UserIntelligenceProviderFactoryImpl(
    component::StartupContext* context, const Config& config)
    : context_(context), config_(config) {}

void UserIntelligenceProviderFactoryImpl::GetUserIntelligenceProvider(
    fidl::InterfaceHandle<fuchsia::modular::ContextEngine> context_engine,
    fidl::InterfaceHandle<fuchsia::modular::StoryProvider> story_provider,
    fidl::InterfaceHandle<fuchsia::modular::FocusProvider> focus_provider,
    fidl::InterfaceHandle<fuchsia::modular::VisibleStoriesProvider>
        visible_stories_provider,
    fidl::InterfaceHandle<fuchsia::modular::PuppetMaster> puppet_master,
    fidl::InterfaceRequest<fuchsia::modular::UserIntelligenceProvider>
        user_intelligence_provider_request) {
  // Fail if someone has already used this Factory to create an instance of
  // fuchsia::modular::UserIntelligenceProvider.
  FXL_CHECK(!impl_);
  impl_.reset(new UserIntelligenceProviderImpl(
      context_, config_, std::move(context_engine), std::move(story_provider),
      std::move(focus_provider), std::move(visible_stories_provider),
      std::move(puppet_master)));
  binding_.reset(new fidl::Binding<fuchsia::modular::UserIntelligenceProvider>(
      impl_.get()));
  binding_->Bind(std::move(user_intelligence_provider_request));
}

}  // namespace maxwell

// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/local_router_link.h"

#include <sstream>
#include <utility>

#include "ipcz/link_side.h"
#include "ipcz/link_type.h"
#include "ipcz/router.h"
#include "ipcz/router_link_state.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/ref_counted.h"

namespace ipcz {

class LocalRouterLink::SharedState : public RefCounted {
 public:
  SharedState(LinkType type,
              LocalRouterLink::InitialState initial_state,
              Ref<Router> router_a,
              Ref<Router> router_b)
      : type_(type),
        router_a_(std::move(router_a)),
        router_b_(std::move(router_b)) {
    if (initial_state == LocalRouterLink::kStable) {
      link_state_.status = RouterLinkState::kStable;
    }
  }

  LinkType type() const { return type_; }

  RouterLinkState& link_state() { return link_state_; }

  Ref<Router> GetRouter(LinkSide side) {
    absl::MutexLock lock(&mutex_);
    switch (side.value()) {
      case LinkSide::kA:
        return router_a_;

      case LinkSide::kB:
        return router_b_;
    }
  }

  void Deactivate(LinkSide side) {
    absl::MutexLock lock(&mutex_);
    switch (side.value()) {
      case LinkSide::kA:
        router_a_.reset();
        break;

      case LinkSide::kB:
        router_b_.reset();
        break;
    }
  }

 private:
  ~SharedState() override = default;

  const LinkType type_;

  absl::Mutex mutex_;
  RouterLinkState link_state_;
  Ref<Router> router_a_ ABSL_GUARDED_BY(mutex_);
  Ref<Router> router_b_ ABSL_GUARDED_BY(mutex_);
};

// static
RouterLink::Pair LocalRouterLink::CreatePair(LinkType type,
                                             const Router::Pair& routers,
                                             InitialState initial_state) {
  ABSL_ASSERT(type == LinkType::kCentral || type == LinkType::kBridge);
  auto state = MakeRefCounted<SharedState>(type, initial_state, routers.first,
                                           routers.second);
  auto a = AdoptRef(new LocalRouterLink(LinkSide::kA, state));
  auto b = AdoptRef(new LocalRouterLink(LinkSide::kB, state));
  return {a, b};
}

LocalRouterLink::LocalRouterLink(LinkSide side, Ref<SharedState> state)
    : side_(side), state_(std::move(state)) {}

LocalRouterLink::~LocalRouterLink() = default;

LinkType LocalRouterLink::GetType() const {
  return state_->type();
}

RouterLinkState* LocalRouterLink::GetLinkState() const {
  return &state_->link_state();
}

Ref<Router> LocalRouterLink::GetLocalPeer() {
  return state_->GetRouter(side_.opposite());
}

RemoteRouterLink* LocalRouterLink::AsRemoteRouterLink() {
  return nullptr;
}

void LocalRouterLink::AcceptParcel(Parcel& parcel) {
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    receiver->AcceptInboundParcel(parcel);
  }
}

void LocalRouterLink::AcceptRouteClosure(SequenceNumber sequence_length) {
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    receiver->AcceptRouteClosureFrom(state_->type(), sequence_length);
  }
}

void LocalRouterLink::AcceptRouteDisconnected() {
  if (Ref<Router> receiver = state_->GetRouter(side_.opposite())) {
    receiver->AcceptRouteDisconnectedFrom(state_->type());
  }
}

void LocalRouterLink::MarkSideStable() {
  state_->link_state().SetSideStable(side_);
}

bool LocalRouterLink::TryLockForBypass(const NodeName& bypass_request_source) {
  if (!state_->link_state().TryLock(side_)) {
    return false;
  }

  state_->link_state().allowed_bypass_request_source = bypass_request_source;

  // Balanced by an acquire in CanNodeRequestBypass().
  std::atomic_thread_fence(std::memory_order_release);
  return true;
}

bool LocalRouterLink::TryLockForClosure() {
  return state_->link_state().TryLock(side_);
}

void LocalRouterLink::Unlock() {
  state_->link_state().Unlock(side_);
}

bool LocalRouterLink::FlushOtherSideIfWaiting() {
  const LinkSide other_side = side_.opposite();
  if (state_->link_state().ResetWaitingBit(other_side)) {
    state_->GetRouter(other_side)->Flush(Router::kForceProxyBypassAttempt);
    return true;
  }
  return false;
}

bool LocalRouterLink::CanNodeRequestBypass(
    const NodeName& bypass_request_source) {
  // Balanced by a release in TryLockForBypass().
  std::atomic_thread_fence(std::memory_order_acquire);
  return state_->link_state().is_locked_by(side_.opposite()) &&
         state_->link_state().allowed_bypass_request_source ==
             bypass_request_source;
}

void LocalRouterLink::BypassPeer(const NodeName& bypass_target_node,
                                 SublinkId bypass_target_sublink) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxying(SequenceNumber inbound_sequence_length,
                                   SequenceNumber outbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::ProxyWillStop(SequenceNumber inbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::BypassPeerWithLink(
    SublinkId new_sublink,
    FragmentRef<RouterLinkState> new_link_state,
    SequenceNumber inbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::StopProxyingToLocalPeer(
    SequenceNumber outbound_sequence_length) {
  // Not implemented, and never called on local links.
  ABSL_ASSERT(false);
}

void LocalRouterLink::Deactivate() {
  state_->Deactivate(side_);
}

std::string LocalRouterLink::Describe() const {
  std::stringstream ss;
  ss << side_.ToString() << "-side link to local peer "
     << state_->GetRouter(side_.opposite()).get() << " on "
     << side_.opposite().ToString() << " side";
  return ss.str();
}

}  // namespace ipcz

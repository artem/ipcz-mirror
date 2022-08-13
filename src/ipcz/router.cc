// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/router.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "ipcz/ipcz.h"
#include "ipcz/local_router_link.h"
#include "ipcz/node_link.h"
#include "ipcz/remote_router_link.h"
#include "ipcz/sequence_number.h"
#include "ipcz/trap_event_dispatcher.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/log.h"
#include "util/multi_mutex_lock.h"
#include "util/safe_math.h"

namespace ipcz {

namespace {

// Helper structure used to accumulate individual parcel flushing operations
// within Router::Flush(), via CollectParcelsToFlush() below.
struct ParcelToFlush {
  // The link over which to flush this parcel.
  RouterLink* link;

  // The parcel to be flushed.
  Parcel parcel;
};

using ParcelsToFlush = absl::InlinedVector<ParcelToFlush, 8>;

// Helper which attempts to pop elements from `queue` for transmission along
// `edge`. This terminates either when `queue` is exhausted, or the next parcel
// in `queue` is to be transmitted over a link that is not yet known to `edge`.
// Any successfully popped elements are accumulated at the end of `parcels`.
void CollectParcelsToFlush(ParcelQueue& queue,
                           const RouteEdge& edge,
                           ParcelsToFlush& parcels) {
  RouterLink* decaying_link = edge.decaying_link().get();
  RouterLink* primary_link = edge.primary_link().get();
  while (queue.HasNextElement()) {
    const SequenceNumber n = queue.current_sequence_number();
    RouterLink* link = nullptr;
    if (decaying_link && edge.ShouldTransmitOnDecayingLink(n)) {
      link = decaying_link;
    } else if (primary_link && !edge.ShouldTransmitOnDecayingLink(n)) {
      link = primary_link;
    } else {
      return;
    }

    ParcelToFlush& parcel = parcels.emplace_back(ParcelToFlush{.link = link});
    const bool popped = queue.Pop(parcel.parcel);
    ABSL_ASSERT(popped);
  }
}

}  // namespace

Router::Router() = default;

Router::~Router() {
  // A Router MUST be serialized or closed before it can be destroyed. Both
  // operations clear `traps_` and imply that no further traps should be added.
  absl::MutexLock lock(&mutex_);
  ABSL_ASSERT(traps_.empty());
}

bool Router::IsPeerClosed() {
  absl::MutexLock lock(&mutex_);
  return (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) != 0;
}

bool Router::IsRouteDead() {
  absl::MutexLock lock(&mutex_);
  return (status_.flags & IPCZ_PORTAL_STATUS_DEAD) != 0;
}

bool Router::IsOnCentralRemoteLink() {
  absl::MutexLock lock(&mutex_);
  // This may only be called on terminal Routers.
  ABSL_ASSERT(!inward_edge_);
  return outward_edge_.primary_link() && outward_edge_.is_stable() &&
         outward_edge_.primary_link()->GetType().is_central() &&
         !outward_edge_.primary_link()->GetLocalPeer();
}

void Router::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  const size_t size = std::min(status.size, status_.size);
  memcpy(&status, &status_, size);
  status.size = size;
}

bool Router::HasLocalPeer(Router& router) {
  absl::MutexLock lock(&mutex_);
  return outward_edge_.GetLocalPeer() == &router;
}

IpczResult Router::AllocateOutboundParcel(size_t num_bytes,
                                          bool allow_partial,
                                          Parcel& parcel) {
  Ref<RouterLink> outward_link;
  {
    absl::MutexLock lock(&mutex_);
    outward_link = outward_edge_.primary_link();
  }

  if (outward_link) {
    outward_link->AllocateParcelData(num_bytes, allow_partial, parcel);
  } else {
    parcel.AllocateData(num_bytes, allow_partial, nullptr);
  }
  return IPCZ_RESULT_OK;
}

IpczResult Router::SendOutboundParcel(Parcel& parcel) {
  Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    if (inbound_parcels_.final_sequence_length()) {
      // If the inbound sequence is finalized, the peer portal must be gone.
      return IPCZ_RESULT_NOT_FOUND;
    }

    const SequenceNumber sequence_number =
        outbound_parcels_.GetCurrentSequenceLength();
    parcel.set_sequence_number(sequence_number);
    if (outward_edge_.primary_link() &&
        outbound_parcels_.MaybeSkipSequenceNumber(sequence_number)) {
      link = outward_edge_.primary_link();
    } else {
      // If there are no unsent parcels ahead of this one in the outbound
      // sequence, and we have an active outward link, we can immediately
      // transmit the parcel without any intermediate queueing step. That is the
      // most common case, but otherwise we have to queue the parcel here and it
      // will be flushed out ASAP.
      DVLOG(4) << "Queuing outbound " << parcel.Describe();
      const bool push_ok =
          outbound_parcels_.Push(sequence_number, std::move(parcel));
      ABSL_ASSERT(push_ok);
    }
  }

  if (link) {
    link->AcceptParcel(parcel);
  } else {
    Flush();
  }
  return IPCZ_RESULT_OK;
}

void Router::CloseRoute() {
  TrapEventDispatcher dispatcher;
  Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    outbound_parcels_.SetFinalSequenceLength(
        outbound_parcels_.GetCurrentSequenceLength());
    traps_.RemoveAll(dispatcher);
  }

  Flush();
}

void Router::SetOutwardLink(Ref<RouterLink> link) {
  ABSL_ASSERT(link);

  {
    absl::MutexLock lock(&mutex_);

    // If we have a stable inward edge (or none at all), and the outward edge
    // is stable too, our new link can be marked stable from our side.
    if (link->GetType().is_central() && outward_edge_.is_stable() &&
        (!inward_edge_ || inward_edge_->is_stable())) {
      link->MarkSideStable();
    }

    if (!is_disconnected_) {
      outward_edge_.SetPrimaryLink(std::move(link));
    }
  }

  if (link) {
    // If the link wasn't adopted, this Router has already been disconnected.
    link->AcceptRouteDisconnected();
    link->Deactivate();
    return;
  }

  Flush(kForceProxyBypassAttempt);
}

size_t Router::GetOutboundCapacityInBytes(const IpczPutLimits& limits) {
  if (limits.max_queued_bytes == 0 || limits.max_queued_parcels == 0) {
    return 0;
  }

  size_t num_queued_bytes = 0;
  Ref<RouterLink> link;
  {
    absl::MutexLock lock(&mutex_);
    if (outbound_parcels_.GetNumAvailableElements() >=
        limits.max_queued_parcels) {
      return 0;
    }
    if (outbound_parcels_.GetTotalAvailableElementSize() >
        limits.max_queued_bytes) {
      return 0;
    }

    num_queued_bytes = outbound_parcels_.GetTotalAvailableElementSize();
    link = outward_edge_.primary_link();
  }

  size_t link_capacity =
      link ? link->GetParcelCapacityInBytes(limits) : limits.max_queued_bytes;
  if (link_capacity <= num_queued_bytes) {
    return 0;
  }

  return link_capacity - num_queued_bytes;
}

size_t Router::GetInboundCapacityInBytes(const IpczPutLimits& limits) {
  absl::MutexLock lock(&mutex_);
  const size_t num_queued_parcels = inbound_parcels_.GetNumAvailableElements();
  const size_t num_queued_bytes =
      inbound_parcels_.GetTotalAvailableElementSize();
  if (num_queued_bytes >= limits.max_queued_bytes ||
      num_queued_parcels >= limits.max_queued_parcels) {
    return 0;
  }
  return limits.max_queued_bytes - num_queued_bytes;
}

bool Router::AcceptInboundParcel(Parcel& parcel) {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    const SequenceNumber sequence_number = parcel.sequence_number();
    if (!inbound_parcels_.Push(sequence_number, std::move(parcel))) {
      // Unexpected route disconnection can cut off inbound sequences, so don't
      // treat an out-of-bounds parcel as a validation failure.
      return true;
    }

    if (!inward_edge_) {
      // If this is a terminal router, we may have trap events to fire.
      status_.num_local_parcels = inbound_parcels_.GetNumAvailableElements();
      status_.num_local_bytes = inbound_parcels_.GetTotalAvailableElementSize();
      traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kNewLocalParcel,
                                dispatcher);

      const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
      if (outward_link && outward_link->GetType().is_central()) {
        outward_link->UpdateInboundQueueState(status_.num_local_parcels,
                                              status_.num_local_bytes);
      }
    }
  }

  Flush();
  return true;
}

bool Router::AcceptOutboundParcel(Parcel& parcel) {
  {
    absl::MutexLock lock(&mutex_);

    // Proxied outbound parcels are always queued in a ParcelQueue even if they
    // will be forwarded immediately. This allows us to track the full sequence
    // of forwarded parcels so we can know with certainty when we're done
    // forwarding.
    //
    // TODO: Using a queue here may increase latency along the route, because it
    // it unnecessarily forces in-order forwarding. We could use an unordered
    // queue for forwarding, but we'd still need some lighter-weight abstraction
    // that tracks complete sequences from potentially fragmented contributions.
    const SequenceNumber sequence_number = parcel.sequence_number();
    if (!outbound_parcels_.Push(sequence_number, std::move(parcel))) {
      // Unexpected route disconnection can cut off outbound sequences, so don't
      // treat an out-of-bounds parcel as a validation failure.
      return true;
    }
  }

  Flush();
  return true;
}

bool Router::AcceptRouteClosureFrom(LinkType link_type,
                                    SequenceNumber sequence_length) {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (link_type.is_outward()) {
      if (!inbound_parcels_.SetFinalSequenceLength(sequence_length)) {
        // Ignore if and only if the sequence was terminated early.
        DVLOG(4) << "Discarding inbound route closure notification";
        return inbound_parcels_.final_sequence_length().has_value() &&
               *inbound_parcels_.final_sequence_length() <= sequence_length;
      }

      if (!inward_edge_ && !bridge_) {
        status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
        if (inbound_parcels_.IsSequenceFullyConsumed()) {
          status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
        }
        traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kPeerClosed,
                                  dispatcher);
      }
    } else if (link_type.is_peripheral_inward()) {
      if (!outbound_parcels_.SetFinalSequenceLength(sequence_length)) {
        // Ignore if and only if the sequence was terminated early.
        DVLOG(4) << "Discarding outbound route closure notification";
        return outbound_parcels_.final_sequence_length().has_value() &&
               *outbound_parcels_.final_sequence_length() <= sequence_length;
      }
    } else if (link_type.is_bridge()) {
      if (!outbound_parcels_.SetFinalSequenceLength(sequence_length)) {
        return false;
      }
      bridge_.reset();
    }
  }

  Flush();
  return true;
}

bool Router::AcceptRouteDisconnectedFrom(LinkType link_type) {
  TrapEventDispatcher dispatcher;
  absl::InlinedVector<Ref<RouterLink>, 4> forwarding_links;
  {
    absl::MutexLock lock(&mutex_);

    DVLOG(4) << "Router " << this << " disconnected from "
             << link_type.ToString() << "link";

    is_disconnected_ = true;
    if (link_type.is_peripheral_inward()) {
      outbound_parcels_.ForceTerminateSequence();
    } else {
      inbound_parcels_.ForceTerminateSequence();
    }

    // Wipe out all remaining links and propagate the disconnection over them.
    forwarding_links.push_back(outward_edge_.ReleasePrimaryLink());
    forwarding_links.push_back(outward_edge_.ReleaseDecayingLink());
    if (inward_edge_) {
      forwarding_links.push_back(inward_edge_->ReleasePrimaryLink());
      forwarding_links.push_back(inward_edge_->ReleaseDecayingLink());
    } else if (bridge_) {
      forwarding_links.push_back(bridge_->ReleasePrimaryLink());
      forwarding_links.push_back(bridge_->ReleaseDecayingLink());
    } else {
      // Terminal routers may have trap events to fire.
      status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (inbound_parcels_.IsSequenceFullyConsumed()) {
        status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
      traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kPeerClosed,
                                dispatcher);
    }
  }

  for (const Ref<RouterLink>& link : forwarding_links) {
    if (link) {
      DVLOG(4) << "Forwarding disconnection over " << link->Describe();
      link->AcceptRouteDisconnected();
      link->Deactivate();
    }
  }

  Flush();
  return true;
}

void Router::NotifyPeerConsumedData() {
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    if (!outward_link || !outward_link->GetType().is_central() ||
        inward_edge_) {
      return;
    }

    const RouterLinkState::QueueState peer_state =
        outward_link->GetPeerQueueState();
    status_.num_remote_parcels = peer_state.num_parcels;
    status_.num_remote_bytes = peer_state.num_bytes;
    traps_.UpdatePortalStatus(status_, TrapSet::UpdateReason::kRemoteActivity,
                              dispatcher);

    if (!traps_.need_remote_state()) {
      outward_link->EnablePeerMonitoring(false);
    }
  }
}

IpczResult Router::GetNextInboundParcel(IpczGetFlags flags,
                                        void* data,
                                        size_t* num_bytes,
                                        IpczHandle* handles,
                                        size_t* num_handles) {
  TrapEventDispatcher dispatcher;
  Ref<RouterLink> link_to_notify;
  {
    absl::MutexLock lock(&mutex_);
    if (inbound_parcels_.IsSequenceFullyConsumed()) {
      return IPCZ_RESULT_NOT_FOUND;
    }
    if (!inbound_parcels_.HasNextElement()) {
      return IPCZ_RESULT_UNAVAILABLE;
    }

    Parcel& p = inbound_parcels_.NextElement();
    const bool allow_partial = (flags & IPCZ_GET_PARTIAL) != 0;
    const size_t data_capacity = num_bytes ? *num_bytes : 0;
    const size_t handles_capacity = num_handles ? *num_handles : 0;
    const size_t data_size =
        allow_partial ? std::min(p.data_size(), data_capacity) : p.data_size();
    const size_t handles_size =
        allow_partial ? std::min(p.num_objects(), handles_capacity)
                      : p.num_objects();
    if (num_bytes) {
      *num_bytes = data_size;
    }
    if (num_handles) {
      *num_handles = handles_size;
    }

    const bool consuming_whole_parcel =
        data_capacity >= data_size && handles_capacity >= handles_size;
    if (!consuming_whole_parcel && !allow_partial) {
      return IPCZ_RESULT_RESOURCE_EXHAUSTED;
    }

    memcpy(data, p.data_view().data(), data_size);
    const bool ok = inbound_parcels_.Consume(
        data_size, absl::MakeSpan(handles, handles_size));
    ABSL_ASSERT(ok);

    status_.num_local_parcels = inbound_parcels_.GetNumAvailableElements();
    status_.num_local_bytes = inbound_parcels_.GetTotalAvailableElementSize();
    if (inbound_parcels_.IsSequenceFullyConsumed()) {
      status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
    traps_.UpdatePortalStatus(
        status_, TrapSet::UpdateReason::kLocalParcelConsumed, dispatcher);

    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    if (outward_link && outward_link->GetType().is_central() &&
        outward_link->UpdateInboundQueueState(status_.num_local_parcels,
                                              status_.num_local_bytes)) {
      link_to_notify = outward_link;
    }
  }

  if (link_to_notify) {
    link_to_notify->NotifyDataConsumed();
  }
  return IPCZ_RESULT_OK;
}

IpczResult Router::BeginGetNextIncomingParcel(const void** data,
                                              size_t* num_data_bytes,
                                              size_t* num_handles) {
  absl::MutexLock lock(&mutex_);
  if (inward_edge_) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  if (!inbound_parcels_.HasNextElement()) {
    return IPCZ_RESULT_UNAVAILABLE;
  }

  Parcel& p = inbound_parcels_.NextElement();
  if (data) {
    *data = p.data_view().data();
  }
  if (num_data_bytes) {
    *num_data_bytes = p.data_size();
  }
  if (num_handles) {
    *num_handles = p.num_objects();
  }
  if ((p.data_size() && (!data || !num_data_bytes)) ||
      (p.num_objects() && !num_handles)) {
    return IPCZ_RESULT_RESOURCE_EXHAUSTED;
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::CommitGetNextIncomingParcel(size_t num_data_bytes_consumed,
                                               absl::Span<IpczHandle> handles) {
  Ref<RouterLink> link_to_notify;
  TrapEventDispatcher dispatcher;
  {
    absl::MutexLock lock(&mutex_);
    if (inward_edge_) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }
    if (!inbound_parcels_.HasNextElement()) {
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    Parcel& p = inbound_parcels_.NextElement();
    if (num_data_bytes_consumed > p.data_size() ||
        handles.size() > p.num_objects()) {
      return IPCZ_RESULT_OUT_OF_RANGE;
    }

    const bool ok = inbound_parcels_.Consume(num_data_bytes_consumed, handles);
    ABSL_ASSERT(ok);

    status_.num_local_parcels = inbound_parcels_.GetNumAvailableElements();
    status_.num_local_bytes = inbound_parcels_.GetTotalAvailableElementSize();
    if (inbound_parcels_.IsSequenceFullyConsumed()) {
      status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
    }
    traps_.UpdatePortalStatus(
        status_, TrapSet::UpdateReason::kLocalParcelConsumed, dispatcher);

    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    if (outward_link && outward_link->GetType().is_central() &&
        outward_link->UpdateInboundQueueState(status_.num_local_parcels,
                                              status_.num_local_bytes)) {
      link_to_notify = outward_link;
    }
  }

  if (link_to_notify) {
    link_to_notify->NotifyDataConsumed();
  }

  return IPCZ_RESULT_OK;
}

IpczResult Router::Trap(const IpczTrapConditions& conditions,
                        IpczTrapEventHandler handler,
                        uint64_t context,
                        IpczTrapConditionFlags* satisfied_condition_flags,
                        IpczPortalStatus* status) {
  const bool need_remote_state =
      (conditions.flags & (IPCZ_TRAP_BELOW_MAX_REMOTE_PARCELS |
                           IPCZ_TRAP_BELOW_MAX_REMOTE_BYTES)) != 0;
  {
    absl::MutexLock lock(&mutex_);
    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    if (need_remote_state) {
      status_.num_remote_parcels = outbound_parcels_.GetNumAvailableElements();
      status_.num_remote_bytes =
          outbound_parcels_.GetTotalAvailableElementSize();

      if (outward_link && outward_link->GetType().is_central()) {
        const RouterLinkState::QueueState peer_state =
            outward_link->GetPeerQueueState();
        status_.num_remote_parcels =
            SaturatedAdd(status_.num_remote_parcels,
                         static_cast<size_t>(peer_state.num_parcels));
        status_.num_remote_bytes =
            SaturatedAdd(status_.num_remote_bytes,
                         static_cast<size_t>(peer_state.num_bytes));
      }
    }

    const bool already_monitoring_remote_state = traps_.need_remote_state();
    IpczResult result = traps_.Add(conditions, handler, context, status_,
                                   satisfied_condition_flags, status);
    if (result != IPCZ_RESULT_OK || !need_remote_state) {
      return result;
    }

    if (!already_monitoring_remote_state) {
      outward_link->EnablePeerMonitoring(true);
    }
  }

  // Safeguard against races between remote state changes and the new trap being
  // installed above. Only reached if the new trap monitors remote state.
  ABSL_ASSERT(need_remote_state);
  NotifyPeerConsumedData();
  return IPCZ_RESULT_OK;
}

IpczResult Router::MergeRoute(const Ref<Router>& other) {
  if (HasLocalPeer(*other) || other == this) {
    return IPCZ_RESULT_INVALID_ARGUMENT;
  }

  {
    MultiMutexLock lock(&mutex_, &other->mutex_);
    if (inward_edge_ || other->inward_edge_ || bridge_ || other->bridge_) {
      // It's not legal to call this on non-terminal routers.
      return IPCZ_RESULT_INVALID_ARGUMENT;
    }

    if (inbound_parcels_.current_sequence_number() > SequenceNumber(0) ||
        outbound_parcels_.GetCurrentSequenceLength() > SequenceNumber(0) ||
        other->inbound_parcels_.current_sequence_number() > SequenceNumber(0) ||
        other->outbound_parcels_.GetCurrentSequenceLength() >
            SequenceNumber(0)) {
      // It's not legal to call this on a router which has transmitted outbound
      // parcels to its peer or retrieved inbound parcels from its queue.
      return IPCZ_RESULT_FAILED_PRECONDITION;
    }

    bridge_ = std::make_unique<RouteEdge>();
    other->bridge_ = std::make_unique<RouteEdge>();

    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kBridge, Router::Pair(WrapRefCounted(this), other));
    bridge_->SetPrimaryLink(std::move(links.first));
    other->bridge_->SetPrimaryLink(std::move(links.second));
  }

  Flush();
  return IPCZ_RESULT_OK;
}

// static
Ref<Router> Router::Deserialize(const RouterDescriptor& descriptor,
                                NodeLink& from_node_link) {
  bool disconnected = false;
  auto router = MakeRefCounted<Router>();
  {
    absl::MutexLock lock(&router->mutex_);
    router->outbound_parcels_.ResetInitialSequenceNumber(
        descriptor.next_outgoing_sequence_number);
    router->inbound_parcels_.ResetInitialSequenceNumber(
        descriptor.next_incoming_sequence_number);
    if (descriptor.peer_closed) {
      router->status_.flags |= IPCZ_PORTAL_STATUS_PEER_CLOSED;
      if (!router->inbound_parcels_.SetFinalSequenceLength(
              descriptor.closed_peer_sequence_length)) {
        return nullptr;
      }
      if (router->inbound_parcels_.IsSequenceFullyConsumed()) {
        router->status_.flags |= IPCZ_PORTAL_STATUS_DEAD;
      }
    }

    Ref<RemoteRouterLink> new_link = from_node_link.AddRemoteRouterLink(
        descriptor.new_sublink, nullptr, LinkType::kPeripheralOutward,
        LinkSide::kB, router);
    if (new_link) {
      router->outward_edge_.SetPrimaryLink(std::move(new_link));

      DVLOG(4) << "Route extended from "
               << from_node_link.remote_node_name().ToString() << " to "
               << from_node_link.local_node_name().ToString() << " via sublink "
               << descriptor.new_sublink;
    } else if (!descriptor.peer_closed) {
      // The new portal is DOA, either because the associated NodeLink is dead,
      // or the sublink ID was already in use. The latter implies a bug or bad
      // behavior, but it should be harmless to ignore beyond this point.
      disconnected = true;
    }
  }

  if (disconnected) {
    DVLOG(4) << "Disconnected new Router immediately after deserialization";
    router->AcceptRouteDisconnectedFrom(LinkType::kPeripheralOutward);
  }
  router->Flush(kForceProxyBypassAttempt);
  return router;
}

void Router::SerializeNewRouter(NodeLink& to_node_link,
                                RouterDescriptor& descriptor) {
  TrapEventDispatcher dispatcher;
  const SublinkId new_sublink = to_node_link.memory().AllocateSublinkIds(1);
  descriptor.new_sublink = new_sublink;
  {
    absl::MutexLock lock(&mutex_);
    traps_.RemoveAll(dispatcher);

    descriptor.next_outgoing_sequence_number =
        outbound_parcels_.GetCurrentSequenceLength();
    descriptor.next_incoming_sequence_number =
        inbound_parcels_.current_sequence_number();

    // Initialize an inward edge but with no link yet. This ensures that we
    // don't look like a terminal router while waiting for a link to be set,
    // which can only happen after `descriptor` is transmitted.
    inward_edge_.emplace();

    if (status_.flags & IPCZ_PORTAL_STATUS_PEER_CLOSED) {
      descriptor.peer_closed = true;
      descriptor.closed_peer_sequence_length =
          *inbound_parcels_.final_sequence_length();

      // Ensure that the new edge decays its link as soon as it has one, since
      // we know the link will not be used.
      inward_edge_->BeginPrimaryLinkDecay();
      inward_edge_->set_length_to_decaying_link(
          *inbound_parcels_.final_sequence_length());
      inward_edge_->set_length_from_decaying_link(
          outbound_parcels_.current_sequence_number());
    }

    // Once `descriptor` is transmitted to the destination node and the new
    // Router is created there, it may immediately begin transmitting messages
    // back to this node regarding `new_sublink`. We establish a new
    // RemoteRouterLink now and register it to `new_sublink` on `to_node_link`,
    // so that any such incoming messages are routed to `this`.
    //
    // NOTE: We do not yet provide `this` itself with a reference to the new
    // RemoteRouterLink, because it's not yet safe for us to send messages to
    // the remote node regarding `new_sublink`. `descriptor` must be transmitted
    // first.
    Ref<RemoteRouterLink> new_link = to_node_link.AddRemoteRouterLink(
        new_sublink, nullptr, LinkType::kPeripheralInward, LinkSide::kA,
        WrapRefCounted(this));

    DVLOG(4) << "Router " << this << " extending route with tentative new "
             << new_link->Describe();
  }
}

void Router::BeginProxyingToNewRouter(NodeLink& to_node_link,
                                      const RouterDescriptor& descriptor) {
  // Acquire a reference to the RemoteRouterLink created by an earlier call to
  // SerializeNewRouter(). If the NodeLink has already been disconnected, this
  // may be null.
  if (auto new_sublink = to_node_link.GetSublink(descriptor.new_sublink)) {
    Ref<RemoteRouterLink> new_router_link = new_sublink->router_link;
    {
      absl::MutexLock lock(&mutex_);
      ABSL_ASSERT(inward_edge_);

      // If the new router has already been closed or disconnected, we will
      // discard the new link to it.
      if (!outbound_parcels_.final_sequence_length() && !is_disconnected_) {
        DVLOG(4) << "Router " << this << " will proxy to new router over "
                 << new_router_link->Describe();

        inward_edge_->SetPrimaryLink(std::move(new_router_link));

        Ref<RouterLink> outward_link = outward_edge_.primary_link();
        if (outward_link && outward_edge_.is_stable() &&
            inward_edge_->is_stable()) {
          outward_link->MarkSideStable();
        }
      }
    }

    if (new_router_link) {
      // The link was not adopted, so deactivate and discard it.
      DVLOG(4) << "Dropping link to new router " << new_router_link->Describe();
      new_router_link->AcceptRouteDisconnected();
      new_router_link->Deactivate();
      return;
    }
  }

  // We may have inbound parcels queued which need to be forwarded to the new
  // Router, so give them a chance to be flushed out.
  Flush(kForceProxyBypassAttempt);
}

bool Router::BypassPeer(RemoteRouterLink& requestor,
                        const NodeName& bypass_target_node,
                        SublinkId bypass_target_sublink) {
  NodeLink& from_node_link = *requestor.node_link();

  // Validate that the source of this request is actually our peripheral outward
  // peer, and that we are therefore its inward peer.
  {
    absl::MutexLock lock(&mutex_);
    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    if (!outward_link) {
      // This Router may have been disconnected already due to some other
      // failure along the route. This is not the fault of the requestor, so we
      // silently ignore the request.
      return true;
    }

    if (outward_link != &requestor) {
      DLOG(ERROR) << "Rejecting BypassPeer received on " << requestor.Describe()
                  << " with existing "
                  << outward_edge_.primary_link()->Describe();
      return false;
    }
  }

  // There are two distinct cases to handle. The first case here is when the
  // proxy's outward peer lives on a different node from us.
  if (bypass_target_node != from_node_link.local_node_name()) {
    Ref<NodeLink> link_to_bypass_target =
        from_node_link.node()->GetLink(bypass_target_node);
    if (link_to_bypass_target) {
      return BypassPeerWithNewRemoteLink(
          requestor, *link_to_bypass_target, bypass_target_sublink,
          link_to_bypass_target->memory().TryAllocateRouterLinkState());
    }

    // We need to establish a link to the target node before we can proceed.
    from_node_link.node()->EstablishLink(
        bypass_target_node,
        [router = WrapRefCounted(this), requestor = WrapRefCounted(&requestor),
         bypass_target_sublink](NodeLink* link_to_bypass_target) {
          if (!link_to_bypass_target) {
            DLOG(ERROR) << "Disconnecting Router due to failed introduction";
            router->AcceptRouteDisconnectedFrom(LinkType::kPeripheralOutward);
            return;
          }

          router->BypassPeerWithNewRemoteLink(
              *requestor, *link_to_bypass_target, bypass_target_sublink,
              link_to_bypass_target->memory().TryAllocateRouterLinkState());
        });
    return true;
  }

  // The second case is when the proxy's outward peer lives on our own node.
  return BypassPeerWithNewLocalLink(requestor, bypass_target_sublink);
}

bool Router::AcceptBypassLink(
    NodeLink& new_node_link,
    SublinkId new_sublink,
    FragmentRef<RouterLinkState> new_link_state,
    SequenceNumber inbound_sequence_length_from_bypassed_link) {
  SequenceNumber length_to_proxy_from_us;
  Ref<RemoteRouterLink> old_link;
  Ref<RemoteRouterLink> new_link;
  {
    absl::ReleasableMutexLock lock(&mutex_);
    if (is_disconnected_ || !outward_edge_.primary_link()) {
      // We've already been unexpectedly disconnected from the proxy, so the
      // route is dysfunctional. Don't establish new links.
      DVLOG(4) << "Discarding proxy bypass link due to peer disconnection";
      return true;
    }

    old_link =
        WrapRefCounted(outward_edge_.primary_link()->AsRemoteRouterLink());
    if (!old_link) {
      // It only makes sense to receive this at a router whose outward link is
      // remote. If we have a non-remote outward link, something is wrong.
      DVLOG(4) << "Rejecting unexpected bypass link";
      return false;
    }

    if (old_link->node_link() != &new_node_link &&
        !old_link->CanNodeRequestBypass(new_node_link.remote_node_name())) {
      // The new link must either go to the same node as the old link, or the
      // the old link must have been expecting a bypass link to the new node.
      DLOG(ERROR) << "Rejecting unauthorized BypassProxy";
      return false;
    }

    length_to_proxy_from_us = outbound_parcels_.current_sequence_number();
    if (!outward_edge_.BeginPrimaryLinkDecay()) {
      DLOG(ERROR) << "Rejecting BypassProxy on failure to decay link";
      return false;
    }

    // By convention the initiator of a bypass assumes side A of the bypass
    // link, so we assume side B.
    new_link = new_node_link.AddRemoteRouterLink(
        new_sublink, std::move(new_link_state), LinkType::kCentral,
        LinkSide::kB, WrapRefCounted(this));

    if (new_link) {
      DVLOG(4) << "Bypassing proxy on other end of " << old_link->Describe()
               << " using a new " << new_link->Describe()
               << " with length to proxy " << length_to_proxy_from_us
               << " and length from proxy "
               << inbound_sequence_length_from_bypassed_link;

      outward_edge_.set_length_to_decaying_link(length_to_proxy_from_us);
      outward_edge_.set_length_from_decaying_link(
          inbound_sequence_length_from_bypassed_link);
      outward_edge_.SetPrimaryLink(new_link);
    }
  }

  if (!new_link) {
    AcceptRouteDisconnectedFrom(LinkType::kCentral);
    return true;
  }

  if (new_link->node_link() == old_link->node_link()) {
    // If the new link goes to the same place as the old link, we only need
    // to tell the proxy there to stop proxying. It has already conspired with
    // its local outward peer.
    old_link->StopProxyingToLocalPeer(length_to_proxy_from_us);
  } else {
    // Otherwise, tell the proxy to stop proxying and let its inward peer (our
    // new outward peer) know that the proxy will stop.
    old_link->StopProxying(length_to_proxy_from_us,
                           inbound_sequence_length_from_bypassed_link);
    new_link->ProxyWillStop(length_to_proxy_from_us);
  }

  Flush();
  return true;
}

bool Router::StopProxying(SequenceNumber inbound_sequence_length,
                          SequenceNumber outbound_sequence_length) {
  Ref<Router> bridge_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (outward_edge_.is_stable()) {
      // Proxies begin decaying their links before requesting to be bypassed,
      // and they don't adopt new links after that. So if either edge is stable
      // then someone is doing something wrong.
      DLOG(ERROR) << "Rejecting StopProxying on invalid or non-proxying Router";
      return false;
    }

    if (bridge_) {
      // If we have a bridge link, we also need to update the router on the
      // other side of the bridge.
      bridge_peer = bridge_->GetDecayingLocalPeer();
      if (!bridge_peer) {
        return false;
      }
    } else if (!inward_edge_ || inward_edge_->is_stable()) {
      // Not a proxy, so this request is invalid.
      return false;
    } else {
      inward_edge_->set_length_to_decaying_link(inbound_sequence_length);
      inward_edge_->set_length_from_decaying_link(outbound_sequence_length);
      outward_edge_.set_length_to_decaying_link(outbound_sequence_length);
      outward_edge_.set_length_from_decaying_link(inbound_sequence_length);
    }
  }

  if (bridge_peer) {
    MultiMutexLock lock(&mutex_, &bridge_peer->mutex_);
    if (!bridge_ || bridge_->is_stable() || !bridge_peer->bridge_ ||
        bridge_peer->bridge_->is_stable()) {
      // The bridge is being or has already been torn down, so there's nothing
      // to do here.
      return true;
    }

    bridge_->set_length_to_decaying_link(inbound_sequence_length);
    bridge_->set_length_from_decaying_link(outbound_sequence_length);
    outward_edge_.set_length_to_decaying_link(outbound_sequence_length);
    outward_edge_.set_length_from_decaying_link(inbound_sequence_length);
    bridge_peer->bridge_->set_length_to_decaying_link(outbound_sequence_length);
    bridge_peer->bridge_->set_length_from_decaying_link(
        inbound_sequence_length);
    bridge_peer->outward_edge_.set_length_to_decaying_link(
        inbound_sequence_length);
    bridge_peer->outward_edge_.set_length_from_decaying_link(
        outbound_sequence_length);
  }

  Flush();
  if (bridge_peer) {
    bridge_peer->Flush();
  }
  return true;
}

bool Router::NotifyProxyWillStop(SequenceNumber inbound_sequence_length) {
  {
    absl::MutexLock lock(&mutex_);
    if (outward_edge_.is_stable()) {
      // If the outward edge is already stable, either this request is invalid,
      // or we've lost all links due to disconnection. In the latter case we
      // can silently ignore this, but the former case is a validation failure.
      return is_disconnected_;
    }

    DVLOG(4) << "Bypassed proxy will stop forwarding inbound parcels after a "
             << "sequence length of " << inbound_sequence_length;

    outward_edge_.set_length_from_decaying_link(inbound_sequence_length);
  }

  Flush();
  return true;
}

bool Router::StopProxyingToLocalPeer(SequenceNumber outbound_sequence_length) {
  Ref<Router> local_peer;
  Ref<Router> bridge_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (bridge_) {
      bridge_peer = bridge_->GetDecayingLocalPeer();
    } else if (outward_edge_.decaying_link()) {
      local_peer = outward_edge_.decaying_link()->GetLocalPeer();
    } else {
      // Ignore this request if we've been unexpectedly disconnected.
      return is_disconnected_;
    }
  }

  if (local_peer && !bridge_peer) {
    // This is the common case, with no bridge link.
    MultiMutexLock lock(&mutex_, &local_peer->mutex_);
    const Ref<RouterLink>& our_link = outward_edge_.decaying_link();
    const Ref<RouterLink>& peer_link =
        local_peer->outward_edge_.decaying_link();
    if (!our_link || !peer_link) {
      // Either Router may have been unexpectedly disconnected, in which case
      // we can ignore this request.
      return true;
    }

    if (!inward_edge_ || our_link->GetLocalPeer() != local_peer ||
        peer_link->GetLocalPeer() != this) {
      // Consistency check: this must be a proxying router, and both this router
      // and its local peer must link to each other.
      DLOG(ERROR) << "Rejecting StopProxyingToLocalPeer at invalid proxy";
      return false;
    }

    DVLOG(4) << "Stopping proxy with decaying "
             << inward_edge_->decaying_link()->Describe() << " and decaying "
             << our_link->Describe();

    local_peer->outward_edge_.set_length_from_decaying_link(
        outbound_sequence_length);
    outward_edge_.set_length_to_decaying_link(outbound_sequence_length);
    inward_edge_->set_length_from_decaying_link(outbound_sequence_length);
  } else if (bridge_peer) {
    // When a bridge peer is present we actually have three local routers
    // involved: this router, its outward peer, and its bridge peer. Both this
    // router and the bridge peer serve as "the" proxy being bypassed in this
    // case, so we'll be bypassing both of them below.
    {
      absl::MutexLock lock(&bridge_peer->mutex_);
      if (bridge_peer->outward_edge_.is_stable()) {
        return false;
      }
      local_peer = bridge_peer->outward_edge_.GetDecayingLocalPeer();
      if (!local_peer) {
        return false;
      }
    }

    MultiMutexLock lock(&mutex_, &local_peer->mutex_, &bridge_peer->mutex_);
    if (outward_edge_.is_stable() || local_peer->outward_edge_.is_stable() ||
        bridge_peer->outward_edge_.is_stable()) {
      return false;
    }

    local_peer->outward_edge_.set_length_from_decaying_link(
        outbound_sequence_length);
    outward_edge_.set_length_from_decaying_link(outbound_sequence_length);
    bridge_->set_length_to_decaying_link(outbound_sequence_length);
    bridge_peer->outward_edge_.set_length_to_decaying_link(
        outbound_sequence_length);
    bridge_peer->bridge_->set_length_from_decaying_link(
        outbound_sequence_length);
  } else {
    // It's invalid to send call this on a Router with a non-local outward peer
    // or bridge link.
    DLOG(ERROR) << "Rejecting StopProxyingToLocalPeer with no local peer";
    return false;
  }

  Flush();
  local_peer->Flush();
  if (bridge_peer) {
    bridge_peer->Flush();
  }
  return true;
}

void Router::NotifyLinkDisconnected(RemoteRouterLink& link) {
  {
    absl::MutexLock lock(&mutex_);
    if (outward_edge_.primary_link() == &link) {
      DVLOG(4) << "Primary " << link.Describe() << " disconnected";
      outward_edge_.ReleasePrimaryLink();
    } else if (outward_edge_.decaying_link() == &link) {
      DVLOG(4) << "Decaying " << link.Describe() << " disconnected";
      outward_edge_.ReleaseDecayingLink();
    } else if (inward_edge_ && inward_edge_->primary_link() == &link) {
      DVLOG(4) << "Primary " << link.Describe() << " disconnected";
      inward_edge_->ReleasePrimaryLink();
    } else if (inward_edge_ && inward_edge_->decaying_link() == &link) {
      DVLOG(4) << "Decaying " << link.Describe() << " disconnected";
      inward_edge_->ReleaseDecayingLink();
    }
  }

  if (link.GetType().is_outward()) {
    AcceptRouteDisconnectedFrom(LinkType::kPeripheralOutward);
  } else {
    AcceptRouteDisconnectedFrom(LinkType::kPeripheralInward);
  }
}

void Router::Flush(FlushBehavior behavior) {
  Ref<RouterLink> outward_link;
  Ref<RouterLink> inward_link;
  Ref<RouterLink> bridge_link;
  Ref<RouterLink> decaying_outward_link;
  Ref<RouterLink> decaying_inward_link;
  Ref<RouterLink> dead_inward_link;
  Ref<RouterLink> dead_outward_link;
  Ref<RouterLink> dead_bridge_link;
  absl::optional<SequenceNumber> final_inward_sequence_length;
  absl::optional<SequenceNumber> final_outward_sequence_length;
  bool on_central_link = false;
  bool inward_link_decayed = false;
  bool outward_link_decayed = false;
  bool dropped_last_decaying_link = false;
  ParcelsToFlush parcels_to_flush;
  {
    absl::MutexLock lock(&mutex_);

    // Acquire stack references to all links we might want to use, so it's safe
    // to acquire additional (unmanaged) references per ParcelToFlush.
    outward_link = outward_edge_.primary_link();
    inward_link = inward_edge_ ? inward_edge_->primary_link() : nullptr;
    decaying_outward_link = outward_edge_.decaying_link();
    decaying_inward_link =
        inward_edge_ ? inward_edge_->decaying_link() : nullptr;
    on_central_link = outward_link && outward_link->GetType().is_central();
    if (bridge_) {
      // Bridges have either a primary link or decaying link, but never both.
      bridge_link = bridge_->primary_link() ? bridge_->primary_link()
                                            : bridge_->decaying_link();
    }

    // Collect any parcels which are safe to transmit now. Note that we do not
    // transmit anything or generally call into any RouterLinks while `mutex_`
    // is held, because such calls may ultimately re-enter this Router
    // (e.g. if a link is a LocalRouterLink, or even a RemoteRouterLink with a
    // fully synchronous driver.) Instead we accumulate work within this block,
    // and then perform any transmissions or link deactivations after the mutex
    // is released further below.

    CollectParcelsToFlush(outbound_parcels_, outward_edge_, parcels_to_flush);
    const SequenceNumber outbound_sequence_length_sent =
        outbound_parcels_.current_sequence_number();
    const SequenceNumber inbound_sequence_length_received =
        inbound_parcels_.GetCurrentSequenceLength();
    if (outward_edge_.MaybeFinishDecay(outbound_sequence_length_sent,
                                       inbound_sequence_length_received)) {
      DVLOG(4) << "Outward " << decaying_outward_link->Describe()
               << " fully decayed at " << outbound_sequence_length_sent
               << " sent and " << inbound_sequence_length_received
               << " recived";
      outward_link_decayed = true;
    }

    if (inward_edge_) {
      CollectParcelsToFlush(inbound_parcels_, *inward_edge_, parcels_to_flush);
      const SequenceNumber inbound_sequence_length_sent =
          inbound_parcels_.current_sequence_number();
      const SequenceNumber outbound_sequence_length_received =
          outbound_parcels_.GetCurrentSequenceLength();
      if (inward_edge_->MaybeFinishDecay(inbound_sequence_length_sent,
                                         outbound_sequence_length_received)) {
        DVLOG(4) << "Inward " << decaying_inward_link->Describe()
                 << " fully decayed at " << inbound_sequence_length_sent
                 << " sent and " << outbound_sequence_length_received
                 << " received";
        inward_link_decayed = true;
      }
    } else if (bridge_link) {
      CollectParcelsToFlush(inbound_parcels_, *bridge_, parcels_to_flush);
    }

    if (bridge_ && bridge_->MaybeFinishDecay(
                       inbound_parcels_.current_sequence_number(),
                       outbound_parcels_.current_sequence_number())) {
      bridge_.reset();
    }

    // If we're dropping the last of our decaying links, our outward link may
    // now be stable. This may unblock proxy bypass or other operations.
    const bool inward_edge_stable =
        !decaying_inward_link || inward_link_decayed;
    const bool outward_edge_stable =
        outward_link && (!decaying_outward_link || outward_link_decayed);
    const bool both_edges_stable = inward_edge_stable && outward_edge_stable;
    const bool either_link_decayed =
        inward_link_decayed || outward_link_decayed;
    if (on_central_link && either_link_decayed && both_edges_stable) {
      DVLOG(4) << "Router with fully decayed links may be eligible for bypass "
               << " with outward " << outward_link->Describe();
      outward_link->MarkSideStable();
      dropped_last_decaying_link = true;
    }

    if (on_central_link && outbound_parcels_.IsSequenceFullyConsumed() &&
        outward_link->TryLockForClosure()) {
      // Notify the other end of the route that this end is closed. See the
      // AcceptRouteClosure() invocation further below.
      final_outward_sequence_length =
          *outbound_parcels_.final_sequence_length();

      // We also have no more use for either outward or inward links: trivially
      // there are no more outbound parcels to send outward, and there no longer
      // exists an ultimate destination for any forwarded inbound parcels. So we
      // drop both links now.
      dead_outward_link = outward_edge_.ReleasePrimaryLink();
    } else if (!inbound_parcels_.ExpectsMoreElements()) {
      // If the other end of the route is gone and we've received all its
      // parcels, we can simply drop the outward link in that case.
      dead_outward_link = outward_edge_.ReleasePrimaryLink();
    }

    if (inbound_parcels_.IsSequenceFullyConsumed()) {
      // We won't be receiving anything new from our peer, and if we're a proxy
      // then we've also forwarded everything already. We can propagate closure
      // inward and drop the inward link, if applicable.
      final_inward_sequence_length = inbound_parcels_.final_sequence_length();
      if (inward_edge_) {
        dead_inward_link = inward_edge_->ReleasePrimaryLink();
      } else {
        dead_bridge_link = std::move(bridge_link);
        bridge_.reset();
      }
    }
  }

  for (ParcelToFlush& parcel : parcels_to_flush) {
    parcel.link->AcceptParcel(parcel.parcel);
  }

  if (outward_link_decayed) {
    decaying_outward_link->Deactivate();
  }

  if (inward_link_decayed) {
    decaying_inward_link->Deactivate();
  }

  if (bridge_link && outward_link && !inward_link && !decaying_inward_link &&
      !decaying_outward_link) {
    MaybeStartBridgeBypass();
  }

  if (dead_outward_link) {
    if (final_outward_sequence_length) {
      dead_outward_link->AcceptRouteClosure(*final_outward_sequence_length);
    }
    dead_outward_link->Deactivate();
  }

  if (dead_inward_link) {
    if (final_inward_sequence_length) {
      dead_inward_link->AcceptRouteClosure(*final_inward_sequence_length);
    }
    dead_inward_link->Deactivate();
  }

  if (dead_bridge_link) {
    if (final_inward_sequence_length) {
      dead_bridge_link->AcceptRouteClosure(*final_inward_sequence_length);
    }
  }

  if (dead_outward_link || !on_central_link) {
    // If we're not on a central link, there's no more work to do.
    return;
  }

  if (!dropped_last_decaying_link && behavior != kForceProxyBypassAttempt) {
    // No relevant state changes, so there are no new bypass opportunities.
    return;
  }

  if (inward_link && MaybeStartSelfBypass()) {
    return;
  }

  if (outward_link) {
    outward_link->FlushOtherSideIfWaiting();
  }
}

bool Router::MaybeStartSelfBypass() {
  Ref<RemoteRouterLink> remote_inward_link;
  Ref<RemoteRouterLink> remote_outward_link;
  Ref<Router> local_outward_peer;
  {
    absl::MutexLock lock(&mutex_);
    if (!inward_edge_ || !inward_edge_->primary_link() ||
        !inward_edge_->is_stable()) {
      // Only a proxy with stable links can be bypassed.
      return false;
    }

    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    RemoteRouterLink* inward_link =
        inward_edge_->primary_link()->AsRemoteRouterLink();
    if (!outward_link || !inward_link) {
      return false;
    }

    const NodeName& inward_peer_name =
        inward_link->node_link()->remote_node_name();
    if (!outward_link->TryLockForBypass(inward_peer_name)) {
      DVLOG(4) << "Proxy bypass blocked by busy " << outward_link->Describe();
      return false;
    }

    remote_inward_link = WrapRefCounted(inward_link);
    local_outward_peer = outward_link->GetLocalPeer();
    if (!local_outward_peer) {
      remote_outward_link = WrapRefCounted(outward_link->AsRemoteRouterLink());
    }
  }

  if (remote_outward_link) {
    // The simpler case here: our outward peer is on another node, so we begin
    // decaying our inward and outward links and ask the inward peer to bypass
    // us ASAP.
    {
      absl::MutexLock lock(&mutex_);
      if (!inward_edge_ || !inward_edge_->primary_link() ||
          !outward_edge_.primary_link()) {
        // We've been disconnected since leaving the block above. Nothing to do.
        return false;
      }

      outward_edge_.BeginPrimaryLinkDecay();
      inward_edge_->BeginPrimaryLinkDecay();
    }

    DVLOG(4) << "Proxy sending bypass request to inward peer over "
             << remote_inward_link->Describe()
             << " targeting outward peer on other side of "
             << remote_outward_link->Describe();

    remote_inward_link->BypassPeer(
        remote_outward_link->node_link()->remote_node_name(),
        remote_outward_link->sublink());
    return true;
  }

  // When the bypass target is local to the same node as this router, we can
  // establish the bypass link immediately and send it to the remote inward
  // peer.
  return StartSelfBypassToLocalPeer(
      *local_outward_peer, *remote_inward_link,
      remote_inward_link->node_link()->memory().TryAllocateRouterLinkState());
}

bool Router::StartSelfBypassToLocalPeer(
    Router& local_outward_peer,
    RemoteRouterLink& inward_link,
    FragmentRef<RouterLinkState> new_link_state) {
  if (new_link_state.is_null()) {
    NodeLinkMemory& memory = inward_link.node_link()->memory();
    memory.AllocateRouterLinkState(
        [router = WrapRefCounted(this),
         local_outward_peer = WrapRefCounted(&local_outward_peer),
         inward_link = WrapRefCounted(&inward_link)](
            FragmentRef<RouterLinkState> new_link_state) {
          router->StartSelfBypassToLocalPeer(*local_outward_peer, *inward_link,
                                             std::move(new_link_state));
        });
    return true;
  }

  Ref<RemoteRouterLink> new_link;
  SequenceNumber length_from_outward_peer;
  const SublinkId new_sublink =
      inward_link.node_link()->memory().AllocateSublinkIds(1);
  {
    MultiMutexLock lock(&mutex_, &local_outward_peer.mutex_);

    const Ref<RouterLink>& outward_link = outward_edge_.primary_link();
    const Ref<RouterLink>& peer_outward_link =
        local_outward_peer.outward_edge_.primary_link();
    if (!outward_link || !peer_outward_link || is_disconnected_ ||
        local_outward_peer.is_disconnected_) {
      DVLOG(4) << "Proxy bypass blocked due to peer closure or disconnection";
      return false;
    }

    DVLOG(4) << "Proxy requesting own bypass from inward peer on "
             << inward_link.node_link()->remote_node_name().ToString()
             << " to local outward peer";

    ABSL_ASSERT(outward_link->GetLocalPeer() == &local_outward_peer);
    ABSL_ASSERT(peer_outward_link->GetLocalPeer() == this);

    // Decay both of our existing links, as well as the local peer's link to us.
    length_from_outward_peer =
        local_outward_peer.outbound_parcels_.current_sequence_number();
    local_outward_peer.outward_edge_.BeginPrimaryLinkDecay();
    local_outward_peer.outward_edge_.set_length_to_decaying_link(
        length_from_outward_peer);
    outward_edge_.BeginPrimaryLinkDecay();
    outward_edge_.set_length_from_decaying_link(length_from_outward_peer);
    inward_edge_->BeginPrimaryLinkDecay();
    inward_edge_->set_length_to_decaying_link(length_from_outward_peer);

    new_link = inward_link.node_link()->AddRemoteRouterLink(
        new_sublink, new_link_state, LinkType::kCentral, LinkSide::kA,
        WrapRefCounted(&local_outward_peer));
  }

  if (!new_link) {
    AcceptRouteDisconnectedFrom(LinkType::kCentral);
    return false;
  }

  // Inform our inward peer on another node that they can bypass us using the
  // new link we just created to our own outward local peer. Once that message
  // is sent, it's safe for that local peer to adopt the new link.
  inward_link.BypassPeerWithLink(new_sublink, std::move(new_link_state),
                                 length_from_outward_peer);
  local_outward_peer.SetOutwardLink(std::move(new_link));
  return true;
}

void Router::MaybeStartBridgeBypass() {
  Ref<Router> first_bridge = WrapRefCounted(this);
  Ref<Router> second_bridge;
  {
    absl::MutexLock lock(&mutex_);
    if (!bridge_ || !bridge_->is_stable()) {
      return;
    }

    second_bridge = bridge_->GetLocalPeer();
    if (!second_bridge) {
      return;
    }
  }

  Ref<Router> first_local_peer;
  Ref<Router> second_local_peer;
  Ref<RemoteRouterLink> first_remote_link;
  Ref<RemoteRouterLink> second_remote_link;
  {
    MultiMutexLock lock(&mutex_, &second_bridge->mutex_);
    const Ref<RouterLink>& link_to_first_peer = outward_edge_.primary_link();
    const Ref<RouterLink>& link_to_second_peer =
        second_bridge->outward_edge_.primary_link();
    if (!link_to_first_peer || !link_to_second_peer) {
      return;
    }

    NodeName first_peer_node_name;
    first_local_peer = link_to_first_peer->GetLocalPeer();
    first_remote_link =
        WrapRefCounted(link_to_first_peer->AsRemoteRouterLink());
    if (first_remote_link) {
      first_peer_node_name = first_remote_link->node_link()->remote_node_name();
    }

    NodeName second_peer_node_name;
    second_local_peer = link_to_second_peer->GetLocalPeer();
    second_remote_link =
        WrapRefCounted(link_to_second_peer->AsRemoteRouterLink());
    if (second_remote_link) {
      second_peer_node_name =
          second_remote_link->node_link()->remote_node_name();
    }

    if (!link_to_first_peer->TryLockForBypass(second_peer_node_name)) {
      return;
    }
    if (!link_to_second_peer->TryLockForBypass(first_peer_node_name)) {
      // Cancel the decay on this bridge's side, because we couldn't decay the
      // other side of the bridge yet.
      link_to_first_peer->Unlock();
      return;
    }
  }

  // At this point, the outward links from each bridge router have been locked
  // for bypass. There are now three distinct cases to handle, based around
  // where the outward peer routers are located.

  // Case 1: Neither bridge router's outward peer is local to this node. This is
  // roughly equivalent to the normal proxy bypass case where the proxy belongs
  // to a different node from its inward and outward peers. We send a message to
  // our outward peer with sufficient information for it to bypass both bridge
  // routers with a new central link directly to the other bridge router's
  // outward peer.
  if (!first_local_peer && !second_local_peer) {
    {
      MultiMutexLock lock(&mutex_, &second_bridge->mutex_);
      outward_edge_.BeginPrimaryLinkDecay();
      second_bridge->outward_edge_.BeginPrimaryLinkDecay();
      bridge_->BeginPrimaryLinkDecay();
      second_bridge->bridge_->BeginPrimaryLinkDecay();
    }
    second_remote_link->BypassPeer(
        first_remote_link->node_link()->remote_node_name(),
        first_remote_link->sublink());
    return;
  }

  // Case 2: Only one of the bridge routers has a local outward peer. This is
  // roughly equivalent to the normal proxy bypass case where the proxy and its
  // outward peer belong to the same node. This case is handled separately since
  // it's a bit more complex than the cases above and below.
  if (!second_local_peer) {
    StartBridgeBypassFromLocalPeer(
        second_remote_link->node_link()->memory().TryAllocateRouterLinkState());
    return;
  } else if (!first_local_peer) {
    second_bridge->StartBridgeBypassFromLocalPeer(
        first_remote_link->node_link()->memory().TryAllocateRouterLinkState());
    return;
  }

  // Case 3: Both bridge routers' outward peers are local to this node. This is
  // a unique bypass case, as it's the only scenario where all involved routers
  // are local to the same node and bypass can be orchestrated synchronously in
  // a single step.
  {
    MultiMutexLock lock(&mutex_, &second_bridge->mutex_,
                        &first_local_peer->mutex_, &second_local_peer->mutex_);
    const SequenceNumber length_from_first_peer =
        first_local_peer->outbound_parcels_.current_sequence_number();
    const SequenceNumber length_from_second_peer =
        second_local_peer->outbound_parcels_.current_sequence_number();

    RouteEdge& first_peer_edge = first_local_peer->outward_edge_;
    first_peer_edge.BeginPrimaryLinkDecay();
    first_peer_edge.set_length_to_decaying_link(length_from_first_peer);
    first_peer_edge.set_length_from_decaying_link(length_from_second_peer);

    RouteEdge& second_peer_edge = second_local_peer->outward_edge_;
    second_peer_edge.BeginPrimaryLinkDecay();
    second_peer_edge.set_length_to_decaying_link(length_from_second_peer);
    second_peer_edge.set_length_from_decaying_link(length_from_first_peer);

    outward_edge_.BeginPrimaryLinkDecay();
    outward_edge_.set_length_to_decaying_link(length_from_second_peer);
    outward_edge_.set_length_from_decaying_link(length_from_first_peer);

    RouteEdge& peer_bridge_outward_edge = second_bridge->outward_edge_;
    peer_bridge_outward_edge.BeginPrimaryLinkDecay();
    peer_bridge_outward_edge.set_length_to_decaying_link(
        length_from_first_peer);
    peer_bridge_outward_edge.set_length_from_decaying_link(
        length_from_second_peer);

    bridge_->BeginPrimaryLinkDecay();
    bridge_->set_length_to_decaying_link(length_from_first_peer);
    bridge_->set_length_from_decaying_link(length_from_second_peer);

    RouteEdge& peer_bridge = *second_bridge->bridge_;
    peer_bridge.BeginPrimaryLinkDecay();
    peer_bridge.set_length_to_decaying_link(length_from_second_peer);
    peer_bridge.set_length_from_decaying_link(length_from_first_peer);

    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, Router::Pair(first_local_peer, second_local_peer));
    first_local_peer->outward_edge_.SetPrimaryLink(std::move(links.first));
    second_local_peer->outward_edge_.SetPrimaryLink(std::move(links.second));
  }

  first_bridge->Flush();
  second_bridge->Flush();
  first_local_peer->Flush();
  second_local_peer->Flush();
}

void Router::StartBridgeBypassFromLocalPeer(
    FragmentRef<RouterLinkState> link_state) {
  Ref<Router> local_peer;
  Ref<Router> other_bridge;
  {
    absl::MutexLock lock(&mutex_);
    if (!bridge_ || !bridge_->is_stable()) {
      return;
    }

    local_peer = outward_edge_.GetLocalPeer();
    other_bridge = bridge_->GetLocalPeer();
    if (!local_peer || !other_bridge) {
      return;
    }
  }

  Ref<RemoteRouterLink> remote_link;
  {
    absl::MutexLock lock(&other_bridge->mutex_);
    if (!other_bridge->outward_edge_.primary_link()) {
      return;
    }

    remote_link = WrapRefCounted(
        other_bridge->outward_edge_.primary_link()->AsRemoteRouterLink());
    if (!remote_link) {
      return;
    }
  }

  if (link_state.is_null()) {
    // We need a new RouterLinkState on the remote link before we can complete
    // this operation.
    remote_link->node_link()->memory().AllocateRouterLinkState(
        [router = WrapRefCounted(this)](FragmentRef<RouterLinkState> state) {
          if (!state.is_null()) {
            router->StartBridgeBypassFromLocalPeer(std::move(state));
          }
        });
    return;
  }

  // At this point, we have a new RouterLinkState for a new link, we have
  // references to all three local routers (this bridge router, its local peer,
  // and the other bridge router), and we have a remote link to the other bridge
  // router's outward peer. This is sufficient to initiate bypass.

  const Ref<NodeLink>& node_link_to_peer = remote_link->node_link();
  SequenceNumber length_from_local_peer;
  const SublinkId bypass_sublink =
      node_link_to_peer->memory().AllocateSublinkIds(1);
  Ref<RemoteRouterLink> new_link = node_link_to_peer->AddRemoteRouterLink(
      bypass_sublink, link_state, LinkType::kCentral, LinkSide::kA, local_peer);
  {
    MultiMutexLock lock(&mutex_, &other_bridge->mutex_, &local_peer->mutex_);

    length_from_local_peer =
        local_peer->outbound_parcels_.current_sequence_number();

    RouteEdge& edge_from_local_peer = local_peer->outward_edge_;
    edge_from_local_peer.BeginPrimaryLinkDecay();
    edge_from_local_peer.set_length_to_decaying_link(length_from_local_peer);

    RouteEdge& edge_to_other_peer = other_bridge->outward_edge_;
    edge_to_other_peer.BeginPrimaryLinkDecay();
    edge_to_other_peer.set_length_to_decaying_link(length_from_local_peer);

    bridge_->BeginPrimaryLinkDecay();
    bridge_->set_length_to_decaying_link(length_from_local_peer);

    outward_edge_.BeginPrimaryLinkDecay();
    outward_edge_.set_length_from_decaying_link(length_from_local_peer);

    RouteEdge& other_bridge_edge = *other_bridge->bridge_;
    other_bridge_edge.BeginPrimaryLinkDecay();
    other_bridge_edge.set_length_from_decaying_link(length_from_local_peer);
  }

  remote_link->BypassPeerWithLink(bypass_sublink, std::move(link_state),
                                  length_from_local_peer);
  local_peer->SetOutwardLink(std::move(new_link));
  Flush();
  other_bridge->Flush();
  local_peer->Flush();
}

bool Router::BypassPeerWithNewRemoteLink(
    RemoteRouterLink& requestor,
    NodeLink& node_link,
    SublinkId bypass_target_sublink,
    FragmentRef<RouterLinkState> new_link_state) {
  if (new_link_state.is_null()) {
    // We can't proceed with bypass until we have a fragment allocated for a new
    // RouterLinkState.
    node_link.memory().AllocateRouterLinkState(
        [router = WrapRefCounted(this), requestor = WrapRefCounted(&requestor),
         node_link = WrapRefCounted(&node_link),
         bypass_target_sublink](FragmentRef<RouterLinkState> new_link_state) {
          router->BypassPeerWithNewRemoteLink(*requestor, *node_link,
                                              bypass_target_sublink,
                                              std::move(new_link_state));
        });
    return true;
  }

  // Begin decaying our outward link.
  SequenceNumber length_to_decaying_link;
  Ref<RouterLink> new_link;
  const SublinkId new_sublink = node_link.memory().AllocateSublinkIds(1);
  {
    absl::ReleasableMutexLock lock(&mutex_);
    if (!outward_edge_.primary_link() || is_disconnected_) {
      // We've been disconnected since leaving the above block. Don't bother
      // to request a bypass. This is not the requestor's fault, so it's not
      // treated as an error.
      return true;
    }

    if (!outward_edge_.BeginPrimaryLinkDecay()) {
      DLOG(ERROR) << "Rejecting BypassPeer on failure to decay link";
      return false;
    }

    length_to_decaying_link = outbound_parcels_.current_sequence_number();
    outward_edge_.set_length_to_decaying_link(length_to_decaying_link);
    new_link = node_link.AddRemoteRouterLink(new_sublink, new_link_state,
                                             LinkType::kCentral, LinkSide::kA,
                                             WrapRefCounted(this));
  }

  if (!new_link) {
    // The NodeLink was disconnected before we could create a new link for
    // this Router. This is not the requestor's fault, so it's not treated as
    // an error.
    AcceptRouteDisconnectedFrom(LinkType::kCentral);
    return true;
  }

  const NodeName proxy_node_name = requestor.node_link()->remote_node_name();
  DVLOG(4) << "Sending AcceptBypassLink from "
           << node_link.local_node_name().ToString() << " to "
           << node_link.remote_node_name().ToString() << " with new sublink "
           << new_sublink << " to replace a link to proxy "
           << proxy_node_name.ToString() << " via sublink "
           << bypass_target_sublink;

  node_link.AcceptBypassLink(proxy_node_name, bypass_target_sublink,
                             length_to_decaying_link, new_sublink,
                             std::move(new_link_state));

  // NOTE: This link is intentionally set *after* transmitting the
  // above message. Otherwise the router might race on another thread to send
  // messages via `new_sublink`, and the remote node would have no idea where
  // to route them.
  SetOutwardLink(std::move(new_link));
  return true;
}

bool Router::BypassPeerWithNewLocalLink(RemoteRouterLink& requestor,
                                        SublinkId bypass_target_sublink) {
  NodeLink& from_node_link = *requestor.node_link();
  const Ref<Router> new_local_peer =
      from_node_link.GetRouter(bypass_target_sublink);
  if (!new_local_peer) {
    // The peer may have already been destroyed or disconnected from the proxy
    // by the time we get here.
    AcceptRouteDisconnectedFrom(LinkType::kPeripheralOutward);
    return true;
  }

  Ref<RouterLink> link_from_new_local_peer_to_proxy;
  SequenceNumber length_to_proxy_from_us;
  SequenceNumber length_from_proxy_to_us;
  {
    MultiMutexLock lock(&mutex_, &new_local_peer->mutex_);
    length_from_proxy_to_us =
        new_local_peer->outbound_parcels_.current_sequence_number();
    length_to_proxy_from_us = outbound_parcels_.current_sequence_number();

    DVLOG(4) << "Proxy bypass requested with new local peer on "
             << from_node_link.local_node_name().ToString() << " and proxy on "
             << from_node_link.remote_node_name().ToString() << " via sublinks "
             << bypass_target_sublink << " and " << requestor.sublink()
             << "; length to the proxy is " << length_to_proxy_from_us
             << " and length from the proxy " << length_from_proxy_to_us;

    link_from_new_local_peer_to_proxy =
        new_local_peer->outward_edge_.primary_link();
    if (!outward_edge_.primary_link() || !link_from_new_local_peer_to_proxy ||
        is_disconnected_ || new_local_peer->is_disconnected_) {
      return true;
    }

    // Otherwise immediately begin decay of both links to the proxy.
    if (!outward_edge_.BeginPrimaryLinkDecay() ||
        !new_local_peer->outward_edge_.BeginPrimaryLinkDecay()) {
      DLOG(ERROR) << "Rejecting BypassPeer on failure to decay link";
      return false;
    }
    outward_edge_.set_length_to_decaying_link(length_to_proxy_from_us);
    outward_edge_.set_length_from_decaying_link(length_from_proxy_to_us);
    new_local_peer->outward_edge_.set_length_to_decaying_link(
        length_from_proxy_to_us);
    new_local_peer->outward_edge_.set_length_from_decaying_link(
        length_to_proxy_from_us);

    // Finally, link the two routers with a new LocalRouterLink. This link will
    // remain unstable until the decaying proxy links are gone.
    RouterLink::Pair links = LocalRouterLink::CreatePair(
        LinkType::kCentral, Router::Pair(WrapRefCounted(this), new_local_peer));
    outward_edge_.SetPrimaryLink(std::move(links.first));
    new_local_peer->outward_edge_.SetPrimaryLink(std::move(links.second));
  }

  link_from_new_local_peer_to_proxy->StopProxying(length_from_proxy_to_us,
                                                  length_to_proxy_from_us);

  Flush();
  new_local_peer->Flush();
  return true;
}

}  // namespace ipcz

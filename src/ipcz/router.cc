// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/router.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <utility>

#include "ipcz/ipcz.h"
#include "ipcz/node_link.h"
#include "ipcz/remote_router_link.h"
#include "ipcz/sequence_number.h"
#include "ipcz/trap_event_dispatcher.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/container/inlined_vector.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/log.h"

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

void Router::QueryStatus(IpczPortalStatus& status) {
  absl::MutexLock lock(&mutex_);
  const size_t size = std::min(status.size, status_.size);
  memcpy(&status, &status_, size);
  status.size = size;
}

bool Router::HasLocalPeer(Router& router) {
  absl::MutexLock lock(&mutex_);
  return outward_edge_.primary_link() &&
         outward_edge_.primary_link()->HasLocalPeer(router);
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

  Flush();
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

      if (!inward_edge_) {
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

IpczResult Router::GetNextInboundParcel(IpczGetFlags flags,
                                        void* data,
                                        size_t* num_bytes,
                                        IpczHandle* handles,
                                        size_t* num_handles) {
  TrapEventDispatcher dispatcher;
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
  const size_t handles_size = allow_partial
                                  ? std::min(p.num_objects(), handles_capacity)
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
  return IPCZ_RESULT_OK;
}

IpczResult Router::Trap(const IpczTrapConditions& conditions,
                        IpczTrapEventHandler handler,
                        uint64_t context,
                        IpczTrapConditionFlags* satisfied_condition_flags,
                        IpczPortalStatus* status) {
  absl::MutexLock lock(&mutex_);
  return traps_.Add(conditions, handler, context, status_,
                    satisfied_condition_flags, status);
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

  router->Flush();
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
        outbound_parcels_.current_sequence_number();
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
  Flush();
}

bool Router::BypassPeer(RemoteRouterLink& requestor,
                        const NodeName& bypass_target_node,
                        SublinkId bypass_target_sublink) {
  // TODO: Implement this.
  return true;
}

bool Router::AcceptBypassLink(
    Ref<NodeLink> new_node_link,
    SublinkId new_sublink,
    FragmentRef<RouterLinkState> new_link_state,
    SequenceNumber inbound_sequence_length_from_bypassed_link) {
  // TODO: Implement this.
  return true;
}

bool Router::StopProxying(SequenceNumber inbound_sequence_length,
                          SequenceNumber outbound_sequence_length) {
  // TODO: Implement this.
  return true;
}

bool Router::NotifyProxyWillStop(SequenceNumber inbound_sequence_length) {
  // TODO: Implement this.
  return true;
}

bool Router::BypassPeerWithLink(NodeLink& from_node_link,
                                SublinkId new_sublink,
                                FragmentRef<RouterLinkState> new_link_state,
                                SequenceNumber inbound_sequence_length) {
  // TODO: Implement this.
  return true;
}

bool Router::StopProxyingToLocalPeer(SequenceNumber outbound_sequence_length) {
  // TODO: Implement this.
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

void Router::Flush() {
  Ref<RouterLink> outward_link;
  Ref<RouterLink> inward_link;
  Ref<RouterLink> decaying_outward_link;
  Ref<RouterLink> decaying_inward_link;
  Ref<RouterLink> dead_inward_link;
  Ref<RouterLink> dead_outward_link;
  absl::optional<SequenceNumber> final_inward_sequence_length;
  absl::optional<SequenceNumber> final_outward_sequence_length;
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

    // Collect any parcels which are safe to transmit now. Note that we do not
    // transmit anything or generally call into any RouterLinks while `mutex_`
    // is held, because such calls may ultimately re-enter this Router
    // (e.g. if a link is a LocalRouterLink, or even a RemoteRouterLink with a
    // fully synchronous driver.) Instead we accumulate work within this block,
    // and then perform any transmissions or link deactivations after the mutex
    // is released further below.

    CollectParcelsToFlush(outbound_parcels_, outward_edge_, parcels_to_flush);
    if (inward_edge_) {
      CollectParcelsToFlush(inbound_parcels_, *inward_edge_, parcels_to_flush);
    }

    if (outward_link && outbound_parcels_.IsSequenceFullyConsumed()) {
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
      }
    }
  }

  for (ParcelToFlush& parcel : parcels_to_flush) {
    parcel.link->AcceptParcel(parcel.parcel);
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
}

}  // namespace ipcz

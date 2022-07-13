// Copyright 2022 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ipcz/node_link_memory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "ipcz/buffer_id.h"
#include "ipcz/driver_memory.h"
#include "ipcz/fragment_descriptor.h"
#include "ipcz/ipcz.h"
#include "ipcz/node.h"
#include "ipcz/node_link.h"
#include "third_party/abseil-cpp/absl/base/macros.h"
#include "third_party/abseil-cpp/absl/numeric/bits.h"
#include "third_party/abseil-cpp/absl/synchronization/mutex.h"
#include "util/log.h"
#include "util/ref_counted.h"

namespace ipcz {

namespace {

constexpr BufferId kPrimaryBufferId{0};

// Fixed allocation size for each NodeLink's primary shared buffer.
constexpr size_t kPrimaryBufferSize = 64 * 1024;

// The front of the primary buffer is reserved for special current and future
// uses which require synchronous availability throughout a link's lifetime.
constexpr size_t kPrimaryBufferReservedHeaderSize = 256;

// NodeLinkMemory may expand its BufferPool's capacity for each fragment size
// as needed. All newly allocated buffers for this purpose must be a multiple of
// kBlockAllocatorPageSize. More specifically, a new buffer allocation for
// fragment size `n` will be the smallest multiple of kBlockAllocatorPageSize
// which can still fit at least kMinBlockAllocatorCapacity blocks of size `n`.
constexpr size_t kBlockAllocatorPageSize = 64 * 1024;

// The minimum number of blocks which new BlockAllocator buffers must support.
// See comments on kBlockAllocatorPageSize above.
constexpr size_t kMinBlockAllocatorCapacity = 8;

// The maximum total BlockAllocator capacity to automatically reserve for any
// given fragment size within the BufferPool. This is not a hard cap on capacity
// per fragment size, but it sets a limit on how large the pool will grow
// automatically in response to failed allocation requests.
constexpr size_t kMaxBlockAllocatorCapacityPerFragmentSize = 256 * 1024;

// The minimum fragment size (in bytes) to support with dedicated BufferPool
// capacity. All fragment sizes are powers of two. Fragment allocations below
// this size are rounded up to this size.
constexpr size_t kMinFragmentSize = 64;

// The maximum fragment size to support with dedicated BlockAllocator capacity
// within the BufferPool. Allocations beyond this size must fail or fall back
// onto a different allocation scheme which does not use a BlockAllocator.
constexpr size_t kMaxFragmentSizeForBlockAllocation = 16 * 1024;

// The number of fixed RouterLinkState locations in the primary buffer. This
// limits the maximum number of initial portals supported by the ConnectNode()
// API. Note that these states reside in a fixed location at the end of the
// reserved block.
using InitialRouterLinkStateArray =
    std::array<RouterLinkState, NodeLinkMemory::kMaxInitialPortals>;
static_assert(sizeof(InitialRouterLinkStateArray) == 768,
              "Invalid InitialRouterLinkStateArray size");

struct IPCZ_ALIGN(8) PrimaryBufferHeader {
  // Atomic generator for new unique BufferIds to use across the associated
  // NodeLink. This allows each side of a NodeLink to generate new BufferIds
  // spontaneously without synchronization or risk of collisions.
  std::atomic<uint64_t> next_buffer_id;

  // Atomic generator for new unique SublinkIds to use across the associated
  // NodeLink. This allows each side of a NodeLink to generate new SublinkIds
  // spontaneously without synchronization or risk of collisions.
  std::atomic<uint64_t> next_sublink_id;
};

static_assert(sizeof(PrimaryBufferHeader) < kPrimaryBufferReservedHeaderSize);

constexpr size_t kPrimaryBufferHeaderPaddingSize =
    kPrimaryBufferReservedHeaderSize - sizeof(PrimaryBufferHeader);

// Computes the byte offset of one address from another.
uint32_t ToOffset(void* ptr, void* base) {
  return static_cast<uint32_t>(static_cast<uint8_t*>(ptr) -
                               static_cast<uint8_t*>(base));
}

size_t GetBlockSizeForFragmentSize(size_t fragment_size) {
  return std::max(kMinFragmentSize, absl::bit_ceil(fragment_size));
}

}  // namespace

// This structure always sits at offset 0 in the primary buffer and has a fixed
// layout according to the NodeLink's agreed upon protocol version. This is the
// layout for version 0 (currently the only version.)
struct IPCZ_ALIGN(8) NodeLinkMemory::PrimaryBuffer {
  // Header + padding occupies the first 256 bytes.
  PrimaryBufferHeader header;
  uint8_t reserved_header_padding[kPrimaryBufferHeaderPaddingSize];

  // Reserved RouterLinkState instances for use only by the NodeLink's initial
  // portals.
  InitialRouterLinkStateArray initial_link_states;

  // Reserved memory for a series of fixed block allocators. Additional
  // allocators may be adopted by a NodeLinkMemory over its lifetime, but these
  // ones remain fixed within the primary buffer.
  std::array<uint8_t, 4096> mem_for_64_byte_blocks;
  std::array<uint8_t, 12288> mem_for_256_byte_blocks;
  std::array<uint8_t, 15360> mem_for_512_byte_blocks;
  std::array<uint8_t, 11264> mem_for_1024_byte_blocks;
  std::array<uint8_t, 16384> mem_for_2048_byte_blocks;

  BlockAllocator block_allocator_64() {
    return BlockAllocator(absl::MakeSpan(mem_for_64_byte_blocks), 64);
  }

  BlockAllocator block_allocator_256() {
    return BlockAllocator(absl::MakeSpan(mem_for_256_byte_blocks), 256);
  }

  BlockAllocator block_allocator_512() {
    return BlockAllocator(absl::MakeSpan(mem_for_512_byte_blocks), 512);
  }

  BlockAllocator block_allocator_1024() {
    return BlockAllocator(absl::MakeSpan(mem_for_1024_byte_blocks), 1024);
  }

  BlockAllocator block_allocator_2048() {
    return BlockAllocator(absl::MakeSpan(mem_for_2048_byte_blocks), 2048);
  }
};

NodeLinkMemory::NodeLinkMemory(Ref<Node> node,
                               DriverMemoryMapping primary_buffer_memory)
    : node_(std::move(node)),
      primary_buffer_memory_(primary_buffer_memory.bytes()),
      primary_buffer_(
          *reinterpret_cast<PrimaryBuffer*>(primary_buffer_memory_.data())) {
  // Consistency check here, because PrimaryBuffer is private to NodeLinkMemory.
  static_assert(sizeof(PrimaryBuffer) <= kPrimaryBufferSize,
                "PrimaryBuffer structure is too large.");

  const BlockAllocator allocators[] = {
      primary_buffer_.block_allocator_64(),
      primary_buffer_.block_allocator_256(),
      primary_buffer_.block_allocator_512(),
      primary_buffer_.block_allocator_1024(),
      primary_buffer_.block_allocator_2048(),
  };

  buffer_pool_.AddBlockBuffer(kPrimaryBufferId,
                              std::move(primary_buffer_memory), allocators);
}

NodeLinkMemory::~NodeLinkMemory() = default;

void NodeLinkMemory::SetNodeLink(Ref<NodeLink> link) {
  absl::MutexLock lock(&mutex_);
  node_link_ = std::move(link);
}

// static
NodeLinkMemory::Allocation NodeLinkMemory::Allocate(Ref<Node> node) {
  DriverMemory primary_buffer_memory(node->driver(), sizeof(PrimaryBuffer));
  if (!primary_buffer_memory.is_valid()) {
    return {.node_link_memory = nullptr, .primary_buffer_memory = {}};
  }

  auto memory = AdoptRef(
      new NodeLinkMemory(std::move(node), primary_buffer_memory.Map()));

  PrimaryBuffer& primary_buffer = memory->primary_buffer_;

  // The first allocable BufferId is 1, because the primary buffer uses 0.
  primary_buffer.header.next_buffer_id.store(1, std::memory_order_relaxed);

  // The first allocable SublinkId is kMaxInitialPortals. This way it doesn't
  // matter whether the two ends of a NodeLink initiate their connection with a
  // different initial portal count: neither can request more than
  // kMaxInitialPortals, so neither will be assuming initial ownership of any
  // SublinkIds at or above this value.
  primary_buffer.header.next_sublink_id.store(kMaxInitialPortals,
                                              std::memory_order_relaxed);

  // Note: Each InitializeRegion() performs an atomic release, so atomic stores
  // before this section can be relaxed.
  primary_buffer.block_allocator_64().InitializeRegion();
  primary_buffer.block_allocator_256().InitializeRegion();
  primary_buffer.block_allocator_512().InitializeRegion();
  primary_buffer.block_allocator_1024().InitializeRegion();
  primary_buffer.block_allocator_2048().InitializeRegion();

  return {
      .node_link_memory = std::move(memory),
      .primary_buffer_memory = std::move(primary_buffer_memory),
  };
}

// static
Ref<NodeLinkMemory> NodeLinkMemory::Adopt(Ref<Node> node,
                                          DriverMemory primary_buffer_memory) {
  return AdoptRef(
      new NodeLinkMemory(std::move(node), primary_buffer_memory.Map()));
}

BufferId NodeLinkMemory::AllocateNewBufferId() {
  return BufferId{primary_buffer_.header.next_buffer_id.fetch_add(
      1, std::memory_order_relaxed)};
}

SublinkId NodeLinkMemory::AllocateSublinkIds(size_t count) {
  return SublinkId{primary_buffer_.header.next_sublink_id.fetch_add(
      count, std::memory_order_relaxed)};
}

FragmentRef<RouterLinkState> NodeLinkMemory::GetInitialRouterLinkState(
    size_t i) {
  auto& states = primary_buffer_.initial_link_states;
  ABSL_ASSERT(i < states.size());
  RouterLinkState* state = &states[i];

  FragmentDescriptor descriptor(kPrimaryBufferId,
                                ToOffset(state, primary_buffer_memory_.data()),
                                sizeof(RouterLinkState));
  return FragmentRef<RouterLinkState>(RefCountedFragment::kUnmanagedRef,
                                      Fragment(descriptor, state));
}

Fragment NodeLinkMemory::GetFragment(const FragmentDescriptor& descriptor) {
  return buffer_pool_.GetFragment(descriptor);
}

bool NodeLinkMemory::AddBlockBuffer(BufferId id,
                                    size_t block_size,
                                    DriverMemoryMapping mapping) {
  const BlockAllocator allocator(mapping.bytes(), block_size);
  return buffer_pool_.AddBlockBuffer(id, std::move(mapping), {&allocator, 1});
}

Fragment NodeLinkMemory::AllocateFragment(size_t size) {
  if (size == 0 || size > kMaxFragmentSizeForBlockAllocation) {
    // TODO: Support an alternative allocation scheme for large requests.
    return {};
  }

  const size_t block_size = GetBlockSizeForFragmentSize(size);
  Fragment fragment = buffer_pool_.AllocateBlock(block_size);
  if (fragment.is_null()) {
    // Use failure as a hint to possibly expand the pool's capacity. The
    // caller's allocation will still fail, but maybe future allocations won't.
    if (CanExpandBlockCapacity(block_size)) {
      RequestBlockCapacity(block_size, [](bool success) {
        if (!success) {
          DLOG(ERROR) << "Failed to allocate new block capacity.";
        }
      });
    }
  }
  return fragment;
}

bool NodeLinkMemory::FreeFragment(const Fragment& fragment) {
  if (fragment.is_null() ||
      fragment.size() > kMaxFragmentSizeForBlockAllocation) {
    // TODO: Once we support larger non-block-based allocations, support freeing
    // them from here as well.
    return false;
  }

  ABSL_ASSERT(fragment.is_addressable());
  return buffer_pool_.FreeBlock(fragment);
}

void NodeLinkMemory::WaitForBufferAsync(
    BufferId id,
    BufferPool::WaitForBufferCallback callback) {
  buffer_pool_.WaitForBufferAsync(id, std::move(callback));
}

bool NodeLinkMemory::CanExpandBlockCapacity(size_t block_size) {
  return buffer_pool_.GetTotalBlockCapacity(block_size) <
         kMaxBlockAllocatorCapacityPerFragmentSize;
}

void NodeLinkMemory::RequestBlockCapacity(
    size_t block_size,
    RequestBlockCapacityCallback callback) {
  ABSL_ASSERT(block_size >= kMinFragmentSize);

  const size_t min_buffer_size = block_size * kMinBlockAllocatorCapacity;
  const size_t num_pages =
      (min_buffer_size + kBlockAllocatorPageSize - 1) / kBlockAllocatorPageSize;
  const size_t buffer_size = num_pages * kBlockAllocatorPageSize;

  Ref<NodeLink> link;
  {
    absl::MutexLock lock(&mutex_);
    auto [it, need_new_request] =
        capacity_callbacks_.emplace(block_size, CapacityCallbackList());
    it->second.push_back(std::move(callback));
    if (!need_new_request) {
      // There was already a request pending for this block size. `callback`
      // will be run when that request completes.
      return;
    }
    link = node_link_;
  }

  node_->AllocateSharedMemory(
      buffer_size, [self = WrapRefCounted(this), block_size,
                    link = std::move(link)](DriverMemory memory) {
        if (!memory.is_valid()) {
          self->OnCapacityRequestComplete(block_size, false);
          return;
        }

        DriverMemoryMapping mapping = memory.Map();
        BlockAllocator allocator(mapping.bytes(), block_size);
        allocator.InitializeRegion();

        // SUBTLE: We first share the new buffer with the remote node, then
        // register it locally. If we registered the buffer locally first, this
        // could lead to a deadlock on the remote node: another thread on this
        // node could race to send a message which uses a fragment from the new
        // buffer before the message below is sent to share the new buffer with
        // the remote node.
        //
        // The remote node would not be able to dispatch the first message until
        // its pending fragment was resolved, and it wouldn't be able to resolve
        // the pending fragment until it received the new buffer. But the
        // message carrying the new buffer would have been queued after the
        // first message and therefore could not be dispatched until after the
        // first message. Hence, deadlock.
        const BufferId id = self->AllocateNewBufferId();
        link->AddBlockBuffer(id, block_size, std::move(memory));
        self->AddBlockBuffer(id, block_size, std::move(mapping));
        self->OnCapacityRequestComplete(block_size, true);
      });
}

void NodeLinkMemory::OnCapacityRequestComplete(size_t block_size,
                                               bool success) {
  CapacityCallbackList callbacks;
  {
    absl::MutexLock lock(&mutex_);
    auto it = capacity_callbacks_.find(block_size);
    if (it == capacity_callbacks_.end()) {
      return;
    }

    callbacks = std::move(it->second);
    capacity_callbacks_.erase(it);
  }

  for (auto& callback : callbacks) {
    callback(success);
  }
}

}  // namespace ipcz

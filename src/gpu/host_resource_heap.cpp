/**
 * @file    gpu/host_resource_heap.cpp
 * @brief   Guest VA -> ResourceType registry backing FromGuest resolution.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/host_resource_heap.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

namespace bd::gpu {

namespace {

// FromGuest rejects unregistered (sentinel) VAs, and the stored type lets
// Release/DestroyByType dispatch without a uniform offset runtime type field
// (the X360 prefix size differs per resource kind).
//
// Switch: a plain mutex, not a reader/writer lock. Every lookup is a read (the
// draw thread asks once or more per draw) and newlib's pthread_rwlock on
// Horizon wakes the waiters through the kernel on every unlock
// (svcSignalProcessWideKey): measured at ~20% of the draw thread in the field.
// libnx's mutex stays in user space unless another thread holds it, and the
// writers (resource create and release) are rare.
#if defined(__SWITCH__)
using RegistryMutex = std::mutex;
using RegistryReadLock = std::lock_guard<std::mutex>;
#else
using RegistryMutex = std::shared_mutex;
using RegistryReadLock = std::shared_lock<std::shared_mutex>;
#endif
RegistryMutex g_registry_mutex;
std::unordered_map<u32, ResourceType> g_registry;

} // namespace

void HostResourceHeap::Register(u32 guest_va, ResourceType type) {
  std::unique_lock lock(g_registry_mutex);
  g_registry[guest_va] = type;
}

void HostResourceHeap::Unregister(u32 guest_va) {
  std::unique_lock lock(g_registry_mutex);
  g_registry.erase(guest_va);
}

bool HostResourceHeap::IsRegistered(u32 guest_va) {
  RegistryReadLock lock(g_registry_mutex);
  return g_registry.count(guest_va) != 0;
}

bool HostResourceHeap::GetType(u32 guest_va, ResourceType *out_type) {
  RegistryReadLock lock(g_registry_mutex);
  auto it = g_registry.find(guest_va);
  if (it == g_registry.end())
    return false;
  *out_type = it->second;
  return true;
}

} // namespace bd::gpu

// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <optional>
#include <vector>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {
struct Range {
    u64 begin;
    u64 end;

    [[nodiscard]] bool Contains(u64 iterator, u64 size) const noexcept {
        return iterator < end && begin < iterator + size;
    }
};

[[nodiscard]] u64 AllocationChunkSize(u64 required_size) {
    static constexpr std::array sizes{
        0x1000ULL << 10,  0x1400ULL << 10,  0x1800ULL << 10,  0x1c00ULL << 10, 0x2000ULL << 10,
        0x3200ULL << 10,  0x4000ULL << 10,  0x6000ULL << 10,  0x8000ULL << 10, 0xA000ULL << 10,
        0x10000ULL << 10, 0x18000ULL << 10, 0x20000ULL << 10,
    };
    static_assert(std::is_sorted(sizes.begin(), sizes.end()));

    const auto it = std::ranges::lower_bound(sizes, required_size);
    return it != sizes.end() ? *it : Common::AlignUp(required_size, 4ULL << 20);
}

[[nodiscard]] VkMemoryPropertyFlags MemoryUsagePropertyFlags(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    case MemoryUsage::Upload:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    case MemoryUsage::Download:
        return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
               VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    }
    UNREACHABLE_MSG("Invalid memory usage={}", usage);
    return VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
} // Anonymous namespace

class MemoryAllocation {
public:
    explicit MemoryAllocation(const Device& device_, vk::DeviceMemory memory_,
                              VkMemoryPropertyFlags properties, u64 allocation_size_, u32 type)
        : device{device_}, memory{std::move(memory_)}, allocation_size{allocation_size_},
          property_flags{properties}, shifted_memory_type{1U << type} {}

    [[nodiscard]] std::optional<MemoryCommit> Commit(VkDeviceSize size, VkDeviceSize alignment) {
        const std::optional<u64> alloc = FindFreeRegion(size, alignment);
        if (!alloc) {
            // Signal out of memory, it'll try to do more allocations.
            return std::nullopt;
        }
        const Range range{
            .begin = *alloc,
            .end = *alloc + size,
        };
        commits.insert(std::ranges::upper_bound(commits, *alloc, {}, &Range::begin), range);
        return std::make_optional<MemoryCommit>(device, this, *memory, *alloc, *alloc + size);
    }

    void Free(u64 begin) {
        const auto it = std::ranges::find(commits, begin, &Range::begin);
        ASSERT_MSG(it != commits.end(), "Invalid commit");
        commits.erase(it);
    }

    [[nodiscard]] std::span<u8> Map() {
        if (memory_mapped_span.empty()) {
            u8* const raw_pointer = memory.Map(0, allocation_size);
            memory_mapped_span = std::span<u8>(raw_pointer, allocation_size);
        }
        return memory_mapped_span;
    }

    /// Returns whether this allocation is compatible with the arguments.
    [[nodiscard]] bool IsCompatible(VkMemoryPropertyFlags flags, u32 type_mask) const {
        return (flags & property_flags) && (type_mask & shifted_memory_type) != 0;
    }

private:
    [[nodiscard]] static constexpr u32 ShiftType(u32 type) {
        return 1U << type;
    }

    [[nodiscard]] std::optional<u64> FindFreeRegion(u64 size, u64 alignment) noexcept {
        ASSERT(std::has_single_bit(alignment));
        const u64 alignment_log2 = std::countr_zero(alignment);
        std::optional<u64> candidate;
        u64 iterator = 0;
        auto commit = commits.begin();
        while (iterator + size <= allocation_size) {
            candidate = candidate.value_or(iterator);
            if (commit == commits.end()) {
                break;
            }
            if (commit->Contains(*candidate, size)) {
                candidate = std::nullopt;
            }
            iterator = Common::AlignUpLog2(commit->end, alignment_log2);
            ++commit;
        }
        return candidate;
    }

    const Device& device;                       ///< Vulkan device.
    const vk::DeviceMemory memory;              ///< Vulkan memory allocation handler.
    const u64 allocation_size;                  ///< Size of this allocation.
    const VkMemoryPropertyFlags property_flags; ///< Vulkan memory property flags.
    const u32 shifted_memory_type;              ///< Shifted Vulkan memory type.
    std::vector<Range> commits;                 ///< All commit ranges done from this allocation.
    std::span<u8> memory_mapped_span; ///< Memory mapped span. Empty if not queried before.
};

MemoryCommit::MemoryCommit(const Device& device_, MemoryAllocation* allocation_,
                           VkDeviceMemory memory_, u64 begin, u64 end) noexcept
    : device{&device_}, allocation{allocation_}, memory{memory_}, interval{begin, end} {}

MemoryCommit::~MemoryCommit() {
    Release();
}

MemoryCommit& MemoryCommit::operator=(MemoryCommit&& rhs) noexcept {
    Release();
    device = rhs.device;
    allocation = std::exchange(rhs.allocation, nullptr);
    memory = rhs.memory;
    interval = rhs.interval;
    span = std::exchange(rhs.span, std::span<u8>{});
    return *this;
}

MemoryCommit::MemoryCommit(MemoryCommit&& rhs) noexcept
    : device{rhs.device}, allocation{std::exchange(rhs.allocation, nullptr)}, memory{rhs.memory},
      interval{rhs.interval}, span{std::exchange(rhs.span, std::span<u8>{})} {}

std::span<u8> MemoryCommit::Map() {
    if (span.empty()) {
        span = allocation->Map().subspan(interval.first, interval.second - interval.first);
    }
    return span;
}

void MemoryCommit::Release() {
    if (allocation) {
        allocation->Free(interval.first);
    }
}

MemoryAllocator::MemoryAllocator(const Device& device_)
    : device{device_}, properties{device_.GetPhysical().GetMemoryProperties()} {}

MemoryAllocator::~MemoryAllocator() = default;

MemoryCommit MemoryAllocator::Commit(const VkMemoryRequirements& requirements, MemoryUsage usage) {
    // Find the fastest memory flags we can afford with the current requirements
    const VkMemoryPropertyFlags flags = MemoryPropertyFlags(requirements.memoryTypeBits, usage);
    if (std::optional<MemoryCommit> commit = TryCommit(requirements, flags)) {
        return std::move(*commit);
    }
    // Commit has failed, allocate more memory.
    // TODO(Rodrigo): Handle out of memory situations in some way like flushing to guest memory.
    AllocMemory(flags, requirements.memoryTypeBits, AllocationChunkSize(requirements.size));

    // Commit again, this time it won't fail since there's a fresh allocation above.
    // If it does, there's a bug.
    return TryCommit(requirements, flags).value();
}

MemoryCommit MemoryAllocator::Commit(const vk::Buffer& buffer, MemoryUsage usage) {
    auto commit = Commit(device.GetLogical().GetBufferMemoryRequirements(*buffer), usage);
    buffer.BindMemory(commit.Memory(), commit.Offset());
    return commit;
}

MemoryCommit MemoryAllocator::Commit(const vk::Image& image, MemoryUsage usage) {
    auto commit = Commit(device.GetLogical().GetImageMemoryRequirements(*image), usage);
    image.BindMemory(commit.Memory(), commit.Offset());
    return commit;
}

void MemoryAllocator::AllocMemory(VkMemoryPropertyFlags flags, u32 type_mask, u64 size) {
    const u32 type = FindType(flags, type_mask).value();
    vk::DeviceMemory memory = device.GetLogical().AllocateMemory({
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = size,
        .memoryTypeIndex = type,
    });
    allocations.push_back(
        std::make_unique<MemoryAllocation>(device, std::move(memory), flags, size, type));
}

std::optional<MemoryCommit> MemoryAllocator::TryCommit(const VkMemoryRequirements& requirements,
                                                       VkMemoryPropertyFlags flags) {
    for (auto& allocation : allocations) {
        if (!allocation->IsCompatible(flags, requirements.memoryTypeBits)) {
            continue;
        }
        if (auto commit = allocation->Commit(requirements.size, requirements.alignment)) {
            return commit;
        }
    }
    return std::nullopt;
}

VkMemoryPropertyFlags MemoryAllocator::MemoryPropertyFlags(u32 type_mask, MemoryUsage usage) const {
    return MemoryPropertyFlags(type_mask, MemoryUsagePropertyFlags(usage));
}

VkMemoryPropertyFlags MemoryAllocator::MemoryPropertyFlags(u32 type_mask,
                                                           VkMemoryPropertyFlags flags) const {
    if (FindType(flags, type_mask)) {
        // Found a memory type with those requirements
        return flags;
    }
    if (flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
        // Remove host cached bit in case it's not supported
        return MemoryPropertyFlags(type_mask, flags & ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    if (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) {
        // Remove device local, if it's not supported by the requested resource
        return MemoryPropertyFlags(type_mask, flags & ~VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    UNREACHABLE_MSG("No compatible memory types found");
    return 0;
}

std::optional<u32> MemoryAllocator::FindType(VkMemoryPropertyFlags flags, u32 type_mask) const {
    for (u32 type_index = 0; type_index < properties.memoryTypeCount; ++type_index) {
        const VkMemoryPropertyFlags type_flags = properties.memoryTypes[type_index].propertyFlags;
        if ((type_mask & (1U << type_index)) && (type_flags & flags)) {
            // The type matches in type and in the wanted properties.
            return type_index;
        }
    }
    // Failed to find index
    return std::nullopt;
}

bool IsHostVisible(MemoryUsage usage) noexcept {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return false;
    case MemoryUsage::Upload:
    case MemoryUsage::Download:
        return true;
    }
    UNREACHABLE_MSG("Invalid memory usage={}", usage);
    return false;
}

} // namespace Vulkan

#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace dtessl {

// A small reusable bump allocator for parser/search scratch data. It does not
// expose memory to DTESSL programs and deliberately supports only trivially
// destructible objects, making reset deterministic and constant per block.
class ScratchPool {
 public:
  explicit ScratchPool(std::size_t block_size = 64U * 1024U,
                       std::size_t max_capacity = 16U * 1024U * 1024U)
      : block_size_(block_size), max_capacity_(max_capacity) {
    if (block_size_ == 0 || max_capacity_ < block_size_) {
      throw std::invalid_argument("invalid scratch pool capacity");
    }
  }

  ScratchPool(const ScratchPool&) = delete;
  ScratchPool& operator=(const ScratchPool&) = delete;
  ScratchPool(ScratchPool&&) noexcept = default;
  ScratchPool& operator=(ScratchPool&&) noexcept = default;

  [[nodiscard]] void* allocate(std::size_t size,
                               std::size_t alignment = alignof(std::max_align_t)) {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
        alignment > alignof(std::max_align_t)) {
      throw std::invalid_argument("unsupported scratch alignment");
    }
    size = std::max<std::size_t>(size, 1U);

    for (std::size_t index = active_block_; index < blocks_.size(); ++index) {
      if (void* memory = try_allocate(blocks_[index], size, alignment)) {
        active_block_ = index;
        used_ += size;
        high_watermark_ = std::max(high_watermark_, used_);
        return memory;
      }
    }

    const std::size_t required = size + alignment - 1U;
    const std::size_t capacity = std::max(block_size_, required);
    if (capacity > max_capacity_ - reserved_) throw std::bad_alloc();
    blocks_.push_back(Block{std::make_unique<std::byte[]>(capacity), capacity, 0});
    reserved_ += capacity;
    active_block_ = blocks_.size() - 1U;
    void* memory = try_allocate(blocks_.back(), size, alignment);
    if (memory == nullptr) throw std::bad_alloc();
    used_ += size;
    high_watermark_ = std::max(high_watermark_, used_);
    return memory;
  }

  template <class T, class... Args>
  [[nodiscard]] T* make(Args&&... args) {
    static_assert(std::is_trivially_destructible_v<T>,
                  "ScratchPool objects must be trivially destructible");
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "ScratchPool does not support over-aligned objects");
    return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
  }

  void reset() noexcept {
    for (Block& block : blocks_) block.used = 0;
    active_block_ = 0;
    used_ = 0;
  }

  [[nodiscard]] std::size_t used() const noexcept { return used_; }
  [[nodiscard]] std::size_t reserved() const noexcept { return reserved_; }
  [[nodiscard]] std::size_t high_watermark() const noexcept { return high_watermark_; }

 private:
  struct Block {
    std::unique_ptr<std::byte[]> data;
    std::size_t capacity;
    std::size_t used;
  };

  static void* try_allocate(Block& block, std::size_t size, std::size_t alignment) noexcept {
    void* current = block.data.get() + block.used;
    std::size_t space = block.capacity - block.used;
    void* aligned = std::align(alignment, size, current, space);
    if (aligned == nullptr) return nullptr;
    const auto* begin = block.data.get();
    const auto* end = static_cast<std::byte*>(aligned) + size;
    block.used = static_cast<std::size_t>(end - begin);
    return aligned;
  }

  std::size_t block_size_;
  std::size_t max_capacity_;
  std::vector<Block> blocks_;
  std::size_t active_block_{0};
  std::size_t used_{0};
  std::size_t reserved_{0};
  std::size_t high_watermark_{0};
};

}  // namespace dtessl

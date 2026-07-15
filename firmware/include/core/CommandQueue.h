#pragma once

#include <array>
#include <cstddef>

namespace chc {

template <typename T, size_t Capacity>
class CommandQueue {
 public:
  bool push(const T& value) {
    if (size_ == Capacity) return false;
    values_[tail_] = value;
    tail_ = (tail_ + 1) % Capacity;
    ++size_;
    return true;
  }

  bool pop(T& value) {
    if (size_ == 0) return false;
    value = values_[head_];
    head_ = (head_ + 1) % Capacity;
    --size_;
    return true;
  }

  size_t size() const { return size_; }

 private:
  std::array<T, Capacity> values_{};
  size_t head_{0};
  size_t tail_{0};
  size_t size_{0};
};

}  // namespace chc

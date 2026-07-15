#pragma once

#include <array>
#include <cstddef>

namespace chc {

template <typename T, size_t Capacity>
class EventBuffer {
 public:
  bool push(const T& value) {
    if (size_ == Capacity) {
      head_ = (head_ + 1) % Capacity;
      --size_;
    }
    values_[(head_ + size_) % Capacity] = value;
    ++size_;
    return true;
  }

  bool pop(T& value) {
    if (!size_) return false;
    value = values_[head_];
    head_ = (head_ + 1) % Capacity;
    --size_;
    return true;
  }

  size_t size() const { return size_; }

 private:
  std::array<T, Capacity> values_{};
  size_t head_{0};
  size_t size_{0};
};

}  // namespace chc

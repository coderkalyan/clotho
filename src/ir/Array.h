#pragma once

// Owning handle to a fixed-size, heap-allocated array: the frozen counterpart
// to std::vector with no spare capacity and no append. Move-only, so it can
// only be produced (e.g. from a builder's vector) and then read.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <vector>

namespace clotho::ir {

template<class T>
class Array {
public:
    Array() = default;

    // Take ownership of a vector's elements, dropping any spare capacity.
    // for_overwrite: every element is move-assigned right below, so the
    // value-initialization (memset for the POD instruction columns) that plain
    // make_unique performs would be a dead double-touch of the allocation.
    explicit Array(std::vector<T> values)
      : size_(values.size())
      , data_(size_ ? std::make_unique_for_overwrite<T[]>(size_) : nullptr) {
        std::move(values.begin(), values.end(), data_.get());
    }

    size_t size() const {
        return size_;
    }

    const T& operator[](size_t i) const {
        return data_[i];
    }

    const T* data() const {
        return data_.get();
    }

private:
    // Number of elements.
    size_t size_ = 0;
    // Heap-allocated element storage (size_ elements).
    std::unique_ptr<T[]> data_;
};

} // namespace clotho::ir

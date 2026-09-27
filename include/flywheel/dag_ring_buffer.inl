// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_ring_buffer.inl — implementation of dag_ring_buffer.hpp declarations.
// Included at the bottom of dag_ring_buffer.hpp; never include this file directly.

#pragma once

namespace dag {

template <typename T>
RingBuffer<T>::RingBuffer(std::size_t capacity) {
    setCapacity(capacity);
}

template <typename T>
void RingBuffer<T>::setCapacity(std::size_t capacity) {
    if (capacity == buf_.size()) return;
    buf_.assign(capacity, T{});
    head_ = 0;
    size_ = 0;
}

template <typename T>
void RingBuffer<T>::push_back(const T& v) {
    if (full())
        throw std::logic_error(
            "RingBuffer::push_back on a full buffer (capacity " +
            std::to_string(buf_.size()) + ") — pop before pushing, or size the "
            "buffer for the transient peak");
    buf_[(head_ + size_) % buf_.size()] = v;
    ++size_;
}

template <typename T>
void RingBuffer<T>::push_back(T&& v) {
    if (full())
        throw std::logic_error(
            "RingBuffer::push_back on a full buffer (capacity " +
            std::to_string(buf_.size()) + ") — pop before pushing, or size the "
            "buffer for the transient peak");
    buf_[(head_ + size_) % buf_.size()] = std::move(v);
    ++size_;
}

template <typename T>
void RingBuffer<T>::pop_front() {
    if (empty()) throwEmpty("pop_front");
    head_ = (head_ + 1) % buf_.size();
    --size_;
}

template <typename T>
void RingBuffer<T>::pop_back() {
    if (empty()) throwEmpty("pop_back");
    --size_;
}

template <typename T>
void RingBuffer<T>::clear() noexcept {
    head_ = 0;
    size_ = 0;
}

template <typename T>
const T& RingBuffer<T>::front() const {
    if (empty()) throwEmpty("front");
    return buf_[head_];
}

template <typename T>
const T& RingBuffer<T>::back() const {
    if (empty()) throwEmpty("back");
    return buf_[(head_ + size_ - 1) % buf_.size()];
}

template <typename T>
const T& RingBuffer<T>::operator[](std::size_t i) const {
    if (i >= size_)
        throw std::out_of_range(
            "RingBuffer index " + std::to_string(i) + " out of range (size " +
            std::to_string(size_) + ")");
    return buf_[(head_ + i) % buf_.size()];
}

template <typename T>
template <typename Fn>
void RingBuffer<T>::for_each_contiguous(Fn&& fn) const {
    if (size_ == 0) return;
    const std::size_t firstRun = std::min(size_, buf_.size() - head_);
    fn(buf_.data() + head_, firstRun);
    if (firstRun < size_)
        fn(buf_.data(), size_ - firstRun);   // wrapped tail
}

template <typename T>
void RingBuffer<T>::throwEmpty(const char* op) {
    throw std::logic_error(std::string("RingBuffer::") + op + " on an empty buffer");
}

}  // namespace dag

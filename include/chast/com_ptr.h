#pragma once

// Small COM smart pointer. chast never lets a raw COM interface escape a
// capture object, so every AcquireNextFrame() path must be exception-safe:
// acquiring a frame and then throwing leaks the frame, which DXGI then refuses
// to re-acquire until ReleaseFrame().

#include <unknwn.h>

#include <cassert>
#include <utility>

namespace chast {

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}

    explicit ComPtr(T* ptr) : ptr_(ptr) {}

    ComPtr(const ComPtr& other) : ptr_(other.ptr_) { internal_add_ref(); }

    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

    ~ComPtr() { internal_release(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            ComPtr copy(other);
            swap(copy);
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            internal_release();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    ComPtr& operator=(std::nullptr_t) {
        internal_release();
        ptr_ = nullptr;
        return *this;
    }

    void swap(ComPtr& other) noexcept { std::swap(ptr_, other.ptr_); }

    T* get() const noexcept { return ptr_; }
    T** put() noexcept {
        internal_release();
        return &ptr_;
    }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    bool is_null() const noexcept { return ptr_ == nullptr; }

    // Leaves the pointer owned by the caller (for out-parameters).
    T* detach() noexcept { return std::exchange(ptr_, nullptr); }

    // Takes ownership of an existing reference.
    void attach(T* ptr) noexcept {
        if (ptr_ != ptr) {
            internal_release();
            ptr_ = ptr;
        }
    }

    void add_ref() const noexcept { internal_add_ref(); }
    void reset() noexcept { *this = nullptr; }

private:
    void internal_add_ref() const noexcept {
        if (ptr_) {
            ptr_->AddRef();
        }
    }
    void internal_release() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    T* ptr_ = nullptr;
};

template <typename T, typename U>
ComPtr<T> com_query(U* source) {
    if (!source) {
        return {};
    }
    T* out = nullptr;
    if (FAILED(source->QueryInterface(__uuidof(T), reinterpret_cast<void**>(&out)))) {
        return {};
    }
    return ComPtr<T>(out);
}

template <typename T, typename U>
ComPtr<T> com_query(const ComPtr<U>& source) {
    return com_query<T>(source.get());
}

}  // namespace chast

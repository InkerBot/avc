#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace avc::rt {

template <typename T>
class RcuSlot {
public:
    RcuSlot() = default;
    ~RcuSlot() { clear(); }

    RcuSlot(const RcuSlot &) = delete;
    RcuSlot &operator=(const RcuSlot &) = delete;

    class Guard {
    public:
        // ZFW: HOT PATH
        explicit Guard(RcuSlot &slot) noexcept : slot_(&slot)
        {
            slot_->epoch_.fetch_add(1, std::memory_order_acq_rel);
            ptr_ = slot_->current_.load(std::memory_order_acquire);
        }

        // ZFW: HOT PATH
        ~Guard() { slot_->epoch_.fetch_add(1, std::memory_order_release); }

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

        T *get() const noexcept { return ptr_; }

    private:
        RcuSlot *slot_;
        T *ptr_ = nullptr;
    };

    // ZFW: HOT PATH
    Guard enter() noexcept { return Guard(*this); }

    void store(std::unique_ptr<T> next)
    {
        T *previous = current_.exchange(next.release(), std::memory_order_acq_rel);
        if (previous != nullptr) {
            retired_.push_back(std::unique_ptr<T>(previous));
        }
    }

    std::size_t collect()
    {
        if (retired_.empty() || (epoch_.load(std::memory_order_acquire) & 1U) != 0) {
            return 0;
        }
        const std::size_t freed = retired_.size();
        retired_.clear();
        return freed;
    }

    void clear()
    {
        T *previous = current_.exchange(nullptr, std::memory_order_acq_rel);
        delete previous;
        retired_.clear();
    }

    std::size_t pendingRetired() const noexcept { return retired_.size(); }

private:
    std::atomic<T *> current_{nullptr};
    std::atomic<std::uint64_t> epoch_{0};
    std::vector<std::unique_ptr<T>> retired_;
};

}

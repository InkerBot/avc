#pragma once

#include <cmath>

namespace avc::rt {

class SmoothedParam {
public:
    void configure(float coeff) noexcept { coeff_ = coeff; }

    void snap(float value) noexcept
    {
        target_ = value;
        current_ = value;
    }

    // ZFW: HOT PATH
    void setTarget(float value) noexcept { target_ = value; }

    // ZFW: HOT PATH
    float nextBlock() noexcept
    {
        current_ += (target_ - current_) * coeff_;
        return current_;
    }

    float current() const noexcept { return current_; }
    float target() const noexcept { return target_; }

    bool settled(float epsilon = 1e-5F) const noexcept
    {
        return std::fabs(target_ - current_) <= epsilon;
    }

private:
    float coeff_ = 0.2F;
    float target_ = 0.0F;
    float current_ = 0.0F;
};

}

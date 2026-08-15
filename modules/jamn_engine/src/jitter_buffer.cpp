#include "jamn_engine/jitter_buffer.h"

#include <algorithm>

namespace jamn::engine {

std::int64_t JitterBuffer::ComputeClampedTargetUs(std::int64_t nowUs) const {
    const std::int64_t p99 = histogram_.P99(nowUs);
    std::int64_t v = p99 + kSafetyMarginUs;
    v = std::max<std::int64_t>(v, minTargetUs_);
    v = std::max<std::int64_t>(v, 0);
    v = std::min<std::int64_t>(v, kMaxPlayoutUs);
    return v;
}

void JitterBuffer::ReportLateEvent(std::int64_t nowUs) {
    ++lateEventGraceOwed_;
    const std::int64_t computed = ComputeClampedTargetUs(nowUs);
    if (computed > targetUs_) targetUs_ = computed;
}

std::int64_t JitterBuffer::PlayoutTargetUs(std::int64_t nowUs) {
    const std::int64_t computed = ComputeClampedTargetUs(nowUs);
    if (computed > targetUs_) targetUs_ = computed;

    if (!haveLastEval_) {
        lastEvalUs_ = nowUs;
        haveLastEval_ = true;
        return targetUs_;
    }

    if (nowUs - lastEvalUs_ < kShrinkIntervalUs) return targetUs_;
    lastEvalUs_ = nowUs;

    if (lateEventGraceOwed_ > 0) {
        --lateEventGraceOwed_;
        return targetUs_;
    }

    targetUs_ = std::max(computed, targetUs_ - kShrinkStepUs);
    return targetUs_;
}

}  // namespace jamn::engine

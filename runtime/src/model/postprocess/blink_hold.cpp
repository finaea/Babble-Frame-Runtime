#include "model/postprocess/blink_hold.h"

namespace babble {

namespace eye {

double BlinkHold::apply(double openness, uint64_t now_ns, double hold_ms) {
    if (hold_ms <= 0) {
        reset();
        return openness;
    }
    const bool shut = openness < kClosedBelow || now_ns < shut_until_ns_;
    if (shut && !was_shut_)
        shut_until_ns_ = now_ns + uint64_t(hold_ms * 1e6);
    was_shut_ = shut;
    return shut ? 0.0 : openness;
}

void BlinkHold::reset() {
    shut_until_ns_ = 0;
    was_shut_ = false;
}

}

}

#pragma once

#include <cstdint>

namespace babble {

namespace eye {

// The blink hold of frameeyeosc: when an eye closes, it is sent closed for at least hold_ms, so that a short blink
// is long enough for other players to see. It is a minimum, not an extension, and 0 turns it off.
class BlinkHold {
public:
    static constexpr double kClosedBelow = 0.1;   // An openness below this counts as closed.

    // Returns the openness to send for the openness at time now_ns.
    double apply(double openness, uint64_t now_ns, double hold_ms);
    void reset();

private:
    uint64_t shut_until_ns_ = 0;
    bool was_shut_ = false;
};

}

}

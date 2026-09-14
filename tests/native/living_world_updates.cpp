#include "LivingWorldUpdates.h"
#include <cassert>

int main() {
    LivingActivity::NativeNeighborhoodUpdates tick;
    tick.Record(444, false);
    assert(!tick.ShouldVisit(444, false, false, false));
    // A complete bot tick now visits native creatures/gameobjects as well.
    tick.Record(444, true);
    assert(tick.ShouldVisit(444, false, false, false));
    assert(!tick.ShouldVisit(445, false, false, false));
    assert(tick.ShouldVisit(445, false, false, true)); // combat began during AI
    assert(tick.ShouldVisit(445, false, true, false));
    assert(tick.ShouldVisit(445, true, false, false));
    tick.Record(444, false); // a repeated inactive record cannot erase admission
    assert(tick.ShouldVisit(444, false, false, false));
    LivingActivity::NativeNeighborhoodUpdates nextTick;
    assert(!nextTick.ShouldVisit(444, false, false, false)); // no permanent activation
    for(uint64_t actor=1;actor<=1000;++actor) nextTick.Record(actor, actor%4==0);
    for(uint64_t actor=1;actor<=1000;++actor)
        assert(nextTick.ShouldVisit(actor, false, false, false)==(actor%4==0));
}

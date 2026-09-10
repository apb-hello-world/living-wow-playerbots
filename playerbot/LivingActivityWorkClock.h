#ifndef LIVING_ACTIVITY_WORK_CLOCK_H
#define LIVING_ACTIVITY_WORK_CLOCK_H
#include <algorithm>
#include <cstdint>
#include <limits>

namespace LivingActivity {
    // Driven by the existing executor, not a timer/scheduler. Only intervals
    // bracketed by two authorized observations consume an active-work budget.
    // Safety/admission waits and the first resumed sample consume nothing.
    class WorkClock {
    public:
        void Observe(uint64_t monotonicMs, bool authorized) {
            if (running && authorized && monotonicMs >= sampled)
                active += std::min(monotonicMs-sampled,std::numeric_limits<uint64_t>::max()-active);
            sampled=monotonicMs; running=authorized;
        }
        void Progress() { progress=active; }
        uint64_t ActiveMs() const { return active; }
        uint64_t NoProgressMs() const { return active-progress; }
        bool Running() const { return running; }
    private:
        uint64_t sampled=0,active=0,progress=0;
        bool running=false;
    };
}
#endif

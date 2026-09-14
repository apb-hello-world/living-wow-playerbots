#pragma once
#include <cstdint>
#include <unordered_set>

namespace LivingActivity {
    // One map-update lifetime; scalar identities only. The core already decides
    // which bots receive a full AI tick. Their nearby native world must run too,
    // otherwise combat targets, respawns and interactions can freeze off-screen.
    // Inactive bots keep the existing load-only optimization. Combat beginning
    // during this tick must also advance, even if admission happened earlier.
    class NativeNeighborhoodUpdates {
    public:
        void Record(uint64_t actor, bool fullAiTick) {
            if(actor && fullAiTick) active.insert(actor);
        }
        bool ShouldVisit(uint64_t actor, bool optimizationsDisabled, bool realPlayer, bool combat) const {
            return optimizationsDisabled || realPlayer || combat || active.count(actor)!=0;
        }
    private:
        std::unordered_set<uint64_t> active;
    };
}

#ifndef LIVING_ACTIVITY_EFFECTS_H
#define LIVING_ACTIVITY_EFFECTS_H
#include "LivingActivity.h"
namespace LivingActivity {
    constexpr uint32_t Mask(Effect effect) { return static_cast<uint32_t>(effect); }
    constexpr uint32_t AllEffects = 511;
    enum class Lane { Managed, Inspection, Combat, Healing, Loot, Roll, LocalQuest, Safety, Social, State };
    enum class Safety : uint32_t {
        None = 0, Combat = 1, Death = 2, Transfer = 4, Taxi = 8,
        Transport = 16, Falling = 32, UnsafeOperation = 64
    };
    struct Effects {
        uint32_t mask = 0;
        Lane lane = Lane::Managed;
        bool classified = false;
    };
    struct NativePermit {
        WorldContext world;
        Lane lane = Lane::Inspection;
        uint32_t effects = 0, allowedSafety = 0;
        bool validated = false;
    };
}
#endif

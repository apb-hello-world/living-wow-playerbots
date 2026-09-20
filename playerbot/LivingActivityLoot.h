#ifndef LIVING_ACTIVITY_LOOT_H
#define LIVING_ACTIVITY_LOOT_H
#include "LivingActivityEffects.h"
class PlayerbotAI;
namespace LivingActivity {
    // Corpse loot only. Gathering, chests, skinning and remote services retain
    // their own resource journals and consent rules.
    NativePermit NativeCorpseLootPermit(PlayerbotAI&,uint64_t source,uint32_t effects,bool opened=false);
    NativePermit NativeLootBookkeepingPermit(PlayerbotAI&);
}
#endif

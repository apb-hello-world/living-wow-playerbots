#pragma once
#include "LivingGatherQuote.h"
#include <vector>
class Player;
class PlayerbotAI;
class Creature;
namespace LivingActivity {
    // Only already-looted native corpses are offered here. This is not a
    // synthetic kill, a global hunt planner or permission to loot another party.
    bool LocalNativeSkinningQuote(Player&,uint64_t,uint32_t,NativeGatherQuote&,std::string&);
    void NativeSkinningSources(Player&,uint32_t,std::vector<int32_t>&);
    Creature* NativeSkinningNode(Player&,uint32_t,const std::vector<int32_t>&);
    bool HoldsNativeSkinningLoot(PlayerbotAI&,uint64_t,uint32_t);
}

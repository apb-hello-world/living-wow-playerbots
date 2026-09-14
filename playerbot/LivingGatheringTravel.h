#pragma once
#include "TravelMgr.h"
#include <set>

// Immutable spawn exclusions for one accepted service journey. No live actor
// or map object is sent to the existing route worker.
ai::PartitionedTravelList LivingRequestedGatheringPartitions(const ai::TravelMgr& manager,
    const ai::WorldPosition& center, const std::vector<uint32>& partitions,
    const ai::PlayerTravelInfo& info, uint32 purpose, const std::vector<int32>& entries,
    const std::set<uint64>& unavailable, bool onlyPossible=true, float maxDistance=10000.0f);
bool LivingGatheringSpawnMissing(Player& actor,const ai::WorldPosition& destination,uint64& identity);

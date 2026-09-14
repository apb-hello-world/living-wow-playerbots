#include "botpch.h"
#include "LivingGatheringTravel.h"
#include "ServerFacade.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

using namespace ai;

PartitionedTravelList LivingRequestedGatheringPartitions(const TravelMgr& manager,
    const WorldPosition& center,const std::vector<uint32>& partitions,const PlayerTravelInfo& info,
    uint32 purpose,const std::vector<int32>& entries,const std::set<uint64>& unavailable,
    bool onlyPossible,float maxDistance) {
    PartitionedTravelList result;
    if(!info.RequestedGathering() || unavailable.size()>32 ||
        (purpose!=uint32(TravelDestinationPurpose::GatherMining) &&
         purpose!=uint32(TravelDestinationPurpose::GatherHerbalism)))return result;
    struct SearchLock {
        SearchLock(){sTravelMgr.GetPartitionsLock();}
        ~SearchLock(){sTravelMgr.GetPartitionsLock(false);}
    } lock;
    auto destinations=manager.GetDestinations(info,purpose,entries,onlyPossible,maxDistance);
    std::stable_sort(destinations.begin(),destinations.end(),[](const TravelDestination* a,const TravelDestination* b)
        {return a->GetEntry()<b->GetEntry();});
    size_t checked=0,unsafe=0,distant=0,excluded=0;
    for(auto* destination:destinations) {
        if(checked>=8192)break;
        WorldPosition* nearest=nullptr;float nearestDistance=std::numeric_limits<float>::max();
        for(auto* position:destination->GetPoints()) {
            if(checked>=8192)break;
            ++checked;
            if(!position || !TravelMgr::IsLocationLevelValid(*position,info)){++unsafe;continue;}
            const auto* spawn=dynamic_cast<const GuidPosition*>(position);
            if(spawn && unavailable.count(spawn->GetRawValue())){++excluded;continue;}
            const float distance=position->distance(center);
            if(!std::isfinite(distance) || distance<0 || distance>maxDistance){++distant;continue;}
            const auto key=[](const WorldPosition* p){return std::make_tuple(p->getMapId(),p->getX(),p->getY(),p->getZ());};
            if(!nearest || distance<nearestDistance || (distance==nearestDistance && key(position)<key(nearest))) {
                nearest=position;nearestDistance=distance;
            }
        }
        if(nearest)for(const auto radius:partitions) {
            if(!radius || nearestDistance>=radius)continue;
            const auto bucket=uint32(std::min(double(radius)*radius,double(std::numeric_limits<uint32>::max())));
            result[bucket].emplace_back(destination,nearest,nearestDistance);break;
        }
    }
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    sLog.outString("Living isolated gather search: purpose=%u requested=1 sources=%u eligible=%u points=%u unsafe=%u distant=%u excluded=%u scan_limit=%u",
        purpose,uint32(entries.size()),uint32(destinations.size()),uint32(checked),uint32(unsafe),uint32(distant),uint32(excluded),uint32(checked>=8192));
#else
    (void)unsafe;(void)distant;(void)excluded;
#endif
    return result;
}

bool LivingGatheringSpawnMissing(Player& actor,const WorldPosition& destination,uint64& identity) {
    identity=0;
    if(!actor.IsInWorld() || !actor.GetMap() || actor.GetMapId()!=destination.getMapId())return false;
    const float distance=WorldPosition(&actor).distance(destination);
    if(!std::isfinite(distance) || distance<0 || distance>INTERACTION_DISTANCE+3)return false;
    const auto* spawn=dynamic_cast<const GuidPosition*>(&destination);
    if(!spawn || !spawn->IsGameObject())return false;
    // A pooled resource uses a dynamic runtime GUID. Resolve its DB spawn ID,
    // and only blacklist absence after reaching the actual local spawn area.
    auto* node=actor.GetMap()->GetGameObject(spawn->GetCounter());
    if(node && sServerFacade.isSpawned(node))return false;
    identity=spawn->GetRawValue();return identity!=0;
}

#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace LivingActivity {
// Native loot is volatile, unlike carried items. Its custody survives a movement
// lease release but not a different actor/map epoch or loot generation. This is
// an exclusion record only: it cannot grant movement or execute an operation.
class GatherCustody {
    struct Hold {
        std::string task;
        uint64_t actorEpoch=0, mapEpoch=0, source=0, generation=0;
    };
    std::mutex mutex;
    std::map<uint32_t,Hold> held;
public:
    bool Begin(uint32_t actor,const std::string& task,uint64_t actorEpoch,uint64_t mapEpoch,uint64_t source) {
        if(!actor || task.empty() || !actorEpoch || !mapEpoch || !source)return false;
        std::lock_guard<std::mutex> lock(mutex);
        auto old=held.find(actor);
        if(old!=held.end()) {
            if(old->second.actorEpoch==actorEpoch && old->second.mapEpoch==mapEpoch)return false;
            held.erase(old); // The old native object context no longer exists.
        }
        if(held.size()>=20000)return false;
        held.emplace(actor,Hold{task,actorEpoch,mapEpoch,source,0});return true;
    }
    void Opened(uint32_t actor,const std::string& task,uint64_t actorEpoch,uint64_t mapEpoch,uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found=held.find(actor);
        if(found!=held.end() && found->second.task==task && found->second.actorEpoch==actorEpoch &&
            found->second.mapEpoch==mapEpoch)found->second.generation=generation;
    }
    bool Holds(uint32_t actor,uint64_t actorEpoch,uint64_t mapEpoch,uint64_t source,uint64_t generation) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found=held.find(actor);if(found==held.end())return false;
        const auto& value=found->second;
        if(value.actorEpoch!=actorEpoch || value.mapEpoch!=mapEpoch) {held.erase(found);return false;}
        if(value.source!=source)return false;
        if(value.generation && value.generation!=generation) {held.erase(found);return false;}
        return true;
    }
    void Release(uint32_t actor,const std::string& task,uint64_t actorEpoch=0,uint64_t mapEpoch=0) {
        std::lock_guard<std::mutex> lock(mutex);
        auto found=held.find(actor);
        if(found!=held.end() && found->second.task==task &&
            (!actorEpoch || (found->second.actorEpoch==actorEpoch && found->second.mapEpoch==mapEpoch)))held.erase(found);
    }
};
}

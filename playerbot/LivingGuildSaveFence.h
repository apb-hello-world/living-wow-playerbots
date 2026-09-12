#pragma once
#include "LivingActivity.h"
#include <atomic>
#include <map>
#include <memory>

namespace LivingActivity {
// One world-thread writer, immutable reads from native handlers. This protects
// pending native bank state, not guild permissions or a second activity lease.
class GuildSaveFence {
    struct Owner {uint32_t actor;std::string operation;};
    using Map=std::map<uint32_t,Owner>;
    std::shared_ptr<const Map> published=std::make_shared<const Map>();
public:
    bool Hold(uint32_t guild,uint32_t actor,const std::string& operation) {
        if(!guild || !actor || !IsUuid(operation))return false;
        const auto current=std::atomic_load(&published);const auto found=current->find(guild);
        if(found!=current->end())return found->second.actor==actor && found->second.operation==operation;
        if(current->size()>=16)return false;
        auto next=std::make_shared<Map>(*current);next->emplace(guild,Owner{actor,operation});
        std::shared_ptr<const Map> immutable=std::move(next);std::atomic_store(&published,std::move(immutable));return true;
    }
    bool Release(uint32_t guild,uint32_t actor,const std::string& operation) {
        const auto current=std::atomic_load(&published);const auto found=current->find(guild);
        if(found==current->end() || found->second.actor!=actor || found->second.operation!=operation)return false;
        auto next=std::make_shared<Map>(*current);next->erase(guild);
        std::shared_ptr<const Map> immutable=std::move(next);std::atomic_store(&published,std::move(immutable));return true;
    }
    bool Blocks(uint32_t guild,uint32_t actor=0,const std::string& operation="") const {
        const auto snapshot=std::atomic_load(&published);const auto found=snapshot->find(guild);
        return found!=snapshot->end() && (found->second.actor!=actor || found->second.operation!=operation);
    }
};
}

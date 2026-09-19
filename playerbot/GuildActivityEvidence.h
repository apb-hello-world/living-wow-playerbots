#ifndef LIVING_GUILD_ACTIVITY_EVIDENCE_H
#define LIVING_GUILD_ACTIVITY_EVIDENCE_H
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
namespace livingguild {
struct ActivityBinding {
    std::string event;
    uint32_t revision=0,guild=0,group=0,expires=0;
};
struct ActivityProof {
    ActivityBinding binding;
    uint32_t actor=0,kind=0,entry=0,map=0,instance=0,occurred=0;
    uint64_t source=0;
};
// Map-worker callbacks enqueue immutable IDs only. No database access, model
// work, or Player/Map/Group pointers are allowed in this mailbox.
class ActivityEvidenceQueue {
public:
    void Publish(std::map<uint32_t,ActivityBinding> bindings) {
        std::lock_guard<std::mutex> lock(mutex_);
        bindings_=std::move(bindings);enabled_.store(!bindings_.empty(),std::memory_order_release);
    }
    bool Record(uint32_t actor,uint32_t guild,uint32_t group,uint32_t kind,uint32_t entry,
        uint64_t source,uint32_t map,uint32_t instance,uint32_t occurred) {
        if(!enabled_.load(std::memory_order_acquire)||!entry||!source||(kind!=1&&kind!=2)) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        auto found=bindings_.find(actor);
        if(found==bindings_.end()||found->second.guild!=guild||found->second.group!=group||!group||
            occurred>=found->second.expires||proofs_.size()>=512) return false;
        for(const auto& p:proofs_) if(p.actor==actor&&p.kind==kind&&p.source==source&&p.instance==instance&&
            p.binding.event==found->second.event&&p.binding.revision==found->second.revision) return false;
        proofs_.push_back({found->second,actor,kind,entry,map,instance,occurred,source});
        return true;
    }
    std::vector<ActivityProof> Drain(size_t limit=512) {
        std::lock_guard<std::mutex> lock(mutex_);std::vector<ActivityProof> result;
        const size_t count=std::min(limit,proofs_.size());
        result.reserve(count);
        for(size_t i=0;i<count;++i)result.push_back(std::move(proofs_[i]));
        proofs_.erase(proofs_.begin(),proofs_.begin()+count);return result;
    }
private:
    std::atomic<bool> enabled_{false};std::mutex mutex_;
    std::map<uint32_t,ActivityBinding> bindings_;std::vector<ActivityProof> proofs_;
};
}
#endif

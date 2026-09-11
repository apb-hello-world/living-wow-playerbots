#include "LivingActivityItemGain.h"
#include <algorithm>
#include <boost/uuid/name_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace LivingActivity {
    namespace {
        bool SamePlace(const NativeItemStack& a, const NativeItemStack& b) {
            return std::tie(a.actor,a.guid,a.entry,a.bagGuid,a.slot) ==
                std::tie(b.actor,b.guid,b.entry,b.bagGuid,b.slot);
        }
        std::string ClaimPredicate(const ResourceClaim& claim) {
            return "c.claim_id="+SqlValue(claim.id)+" AND c.task_id="+SqlValue(claim.task)+
                " AND c.actor_guid="+std::to_string(claim.actor)+" AND c.item_guid="+std::to_string(claim.itemGuid)+
                " AND c.item_entry="+std::to_string(claim.itemEntry)+" AND c.quantity="+std::to_string(claim.quantity)+
                " AND c.copper=0 AND c.location='bags' AND c.native_reference=0 AND c.state='held' AND c.revision=1";
        }
        std::vector<VerifiedItemGain> Canonical(const ItemGainSpec& spec,
            uint32_t actor, const std::vector<VerifiedItemGain>& gains) {
            if (!ValidItemGainSpec(spec) || !actor || gains.empty() || gains.size() > MaximumItemGainStacks)
                throw std::invalid_argument("Bounded exact native item gains required");
            auto sorted=gains;
            std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.after.guid < b.after.guid;});
            uint64_t total=0; uint32_t previous=0;
            std::set<std::pair<uint32_t,uint8_t>> places;
            for (const auto& gain : sorted) {
                const auto& after=gain.after;
                if (!after.guid || after.guid == previous || after.actor != actor || after.entry != spec.entry ||
                    !SamePlace(gain.before,after) || !gain.added || after.count < gain.added ||
                    after.count-gain.added != gain.before.count || !places.emplace(after.bagGuid,after.slot).second)
                    throw std::invalid_argument("Native gain identity, location or quantity mismatch");
                total+=gain.added; previous=after.guid;
            }
            if (total != spec.quantity) throw std::invalid_argument("Native gained quantity mismatch");
            return sorted;
        }
    }
    bool ValidItemGainSpec(const ItemGainSpec& spec) { return spec.entry && spec.quantity && spec.quantity <= 10000; }
    std::string ItemGainSpecJson(const ItemGainSpec& spec) {
        if (!ValidItemGainSpec(spec)) throw std::invalid_argument("Exact item gain required");
        return "{\"entry\":"+std::to_string(spec.entry)+",\"quantity\":"+std::to_string(spec.quantity)+'}';
    }
    bool VerifyNativeItemGain(uint32_t actor,const ItemGainSpec& spec,
        const std::vector<NativeItemStack>& before,const std::vector<NativeItemStack>& after,
        std::vector<VerifiedItemGain>& gains,std::string& blocker) {
        gains.clear();
        auto reject=[&](const char* code){ gains.clear(); blocker=code; return false; };
        if (!actor || !ValidItemGainSpec(spec) || before.size() > 256 || after.size() > 256)
            return reject("invalid_native_item_gain_scope");
        std::map<uint32_t,NativeItemStack> initial,final;
        auto indexSnapshot = [&](const std::vector<NativeItemStack>& rows,
            std::map<uint32_t,NativeItemStack>& index) {
            std::set<std::pair<uint32_t,uint8_t>> places;
            for (const auto& row : rows)
                if (row.actor != actor || !row.guid || row.entry != spec.entry || !row.count ||
                    !index.emplace(row.guid,row).second || !places.emplace(row.bagGuid,row.slot).second)
                    return false;
            return true;
        };
        if (!indexSnapshot(before,initial) || !indexSnapshot(after,final))
            return reject("ambiguous_native_item_gain_snapshot");
        for (const auto& old : initial) {
            const auto found=final.find(old.first);
            if (found == final.end() || !SamePlace(old.second,found->second) || found->second.count < old.second.count)
                return reject("existing_native_stack_changed_during_acquisition");
        }
        for (const auto& row : final) {
            auto old=row.second; old.count=0;
            const auto found=initial.find(row.first);
            if (found != initial.end()) old=found->second;
            if (row.second.count > old.count) gains.push_back({old,row.second,row.second.count-old.count});
        }
        try { gains=Canonical(spec,actor,gains); }
        catch (const std::exception&) { return reject("native_item_gain_quantity_or_identity_mismatch"); }
        blocker.clear(); return true;
    }
    std::string ItemGainClaimId(const std::string& operation,uint32_t itemGuid) {
        if (!IsUuid(operation) || !itemGuid) throw std::invalid_argument("Native gain operation/identity required");
        static const auto ns=boost::uuids::string_generator()("f7b8a606-c3ef-5f36-b599-2a958be4596e");
        return boost::uuids::to_string(boost::uuids::name_generator(ns)(operation+':'+std::to_string(itemGuid)));
    }
    std::vector<ClaimReceiptChange> ItemGainClaims(const Task& task,const std::string& operation,
        const ItemGainSpec& spec,const std::vector<VerifiedItemGain>& gains) {
        if (!IsUuid(task.root)) throw std::invalid_argument("Acquired items require a root commitment");
        std::vector<ClaimReceiptChange> result;
        for (const auto& gain : Canonical(spec,task.actor,gains)) {
            ResourceClaim claim;
            claim.id=ItemGainClaimId(operation,gain.after.guid); claim.task=task.root; claim.actor=task.actor;
            claim.itemGuid=gain.after.guid; claim.itemEntry=gain.after.entry; claim.quantity=gain.added;
            claim.location="bags"; claim.state="held";
            if (!ValidResourceClaim(claim)) throw std::invalid_argument("Invalid acquired resource claim");
            result.push_back({claim,0});
        }
        return result;
    }
    ClaimedOutcome AcquiredOperationWrite(const Task& task,uint64_t expected,const OperationResult& result,
        const std::string& receipt,const std::string& nativeAfter,const std::vector<ClaimConsumption>& consumption,
        const ItemGainSpec& spec,const std::vector<VerifiedItemGain>& gains) {
        const auto sorted=Canonical(spec,task.actor,gains);
        const auto outputClaims=ItemGainClaims(task,result.id,spec,sorted);
        if (consumption.empty() || consumption.size()+outputClaims.size() > 16)
            throw std::invalid_argument("Bounded consumed and acquired claim batch required");
        std::string proof="{\"result\":"+nativeAfter+",\"item_gain\":"+ItemGainSpecJson(spec)+",\"stacks\":[";
        for (const auto& gain : sorted) {
            if (proof.back() != '[') proof+=',';
            proof+="{\"guid\":"+std::to_string(gain.after.guid)+",\"bag\":"+std::to_string(gain.after.bagGuid)+
                ",\"slot\":"+std::to_string(gain.after.slot)+",\"before\":"+std::to_string(gain.before.count)+
                ",\"after\":"+std::to_string(gain.after.count)+",\"added\":"+std::to_string(gain.added)+'}';
        }
        proof+="]}";
        auto out=ConsumedOperationWrite(task,expected,result,receipt,proof,consumption);
        out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
            SqlValue(result.id)+" AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.item_gain'))="+SqlValue(ItemGainSpecJson(spec))+')';
        const auto accepted=out.journal.receiptQuery;
        for (const auto& change : outputClaims) {
            const auto& claim=change.after;
            for (const auto& input : consumption) if (input.before.id == claim.id)
                throw std::invalid_argument("Input and output claim identities cannot alias");
            out.journal.statements.front()+=" AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.claim_id="+SqlValue(claim.id)+')';
            out.journal.statements.push_back("INSERT INTO living_activity_claim (claim_id,task_id,actor_guid,item_guid,item_entry,quantity,"
                "copper,location,native_reference,state,revision,updated_at_ms) SELECT "+SqlValue(claim.id)+','+SqlValue(claim.task)+','+
                std::to_string(claim.actor)+','+std::to_string(claim.itemGuid)+','+std::to_string(claim.itemEntry)+','+
                std::to_string(claim.quantity)+",0,'bags',0,'held',1,"+std::to_string(task.updatedAtMs)+
                " WHERE EXISTS ("+accepted+") ON DUPLICATE KEY UPDATE claim_id=claim_id");
            out.journal.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+ClaimPredicate(claim)+')';
            out.changes.push_back(change);
        }
        return out;
    }
}

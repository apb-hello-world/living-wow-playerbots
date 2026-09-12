#pragma once
#include "LivingActivityResources.h"
#include "LivingActivityJournal.h"
#include <limits>
#include <map>
#include <set>

namespace LivingActivity {
    // Shared terminal bookkeeping for verified personal work. This releases
    // reservations, never native possessions. Mail/transfers remain obligations.
    struct PersonalResourceSettlement {
        std::vector<ClaimReceiptChange> claims;
        std::string guards;
        std::string fingerprint;
    };
    inline std::string SettlementClaimWhere(const ResourceClaim& c) {
        return "c.claim_id="+SqlValue(c.id)+" AND c.task_id="+SqlValue(c.task)+" AND c.actor_guid="+std::to_string(c.actor)+
            " AND c.item_guid="+std::to_string(c.itemGuid)+" AND c.item_entry="+std::to_string(c.itemEntry)+
            " AND c.quantity="+std::to_string(c.quantity)+" AND c.copper="+std::to_string(c.copper)+
            " AND c.location="+SqlValue(c.location)+" AND c.native_reference="+std::to_string(c.nativeReference)+
            " AND c.state="+SqlValue(c.state)+" AND c.revision="+std::to_string(c.revision);
    }
    inline bool PreparePersonalResourceSettlement(const Task& task,const UnsettledClaimBatch& batch,
        const std::vector<NativeResourceBalance>& balances,PersonalResourceSettlement& result,
        std::string& blocker,const std::string& prefix) {
        result={};auto reject=[&](const char* why){blocker=prefix+why;return false;};
        if (!batch.bookRevision || batch.claims.size()>16 || balances.size()>16 || (!batch.complete && batch.claims.empty()))
            return reject("claim_batch_invalid");
        std::map<uint32_t,NativeResourceBalance> owned;
        for (const auto& native:balances) {
            if (native.actor!=task.actor || native.nativeReference || !owned.emplace(native.itemGuid,native).second ||
                (native.location=="money" ? (native.itemGuid || native.itemEntry || native.quantity) :
                 (!native.itemGuid || !native.itemEntry || native.copper ||
                  (native.location!="bags" && native.location!="bank" && native.location!="equipment") ||
                  (native.location=="equipment" && native.quantity!=1))))
                return reject("native_balance_invalid");
        }
        PersonalResourceSettlement prepared;std::set<std::string> ids;std::map<uint32_t,uint64_t> totals;
        std::string excluded;prepared.fingerprint=task.checkpoint.data+'|'+std::to_string(batch.bookRevision)+'|'+(batch.complete?"complete":"batch");
        for (const auto& c:batch.claims) {
            if (!ValidResourceClaim(c) || c.task!=task.id || c.actor!=task.actor || !ids.insert(c.id).second ||
                c.revision>=std::numeric_limits<uint64_t>::max()-1 || (c.state!="held" && c.state!="proposed") || c.nativeReference)
                return reject("claim_requires_reconciliation");
            if (c.state=="proposed") {
                if (c.itemGuid) return reject("proposal_has_native_identity");
            } else {
                const auto found=owned.find(c.itemGuid);
                if (found==owned.end() || found->second.location!=c.location || found->second.itemEntry!=c.itemEntry)
                    return reject("native_stock_missing");
                const uint64_t amount=uint64_t(c.quantity)+c.copper;
                const uint64_t available=c.copper?found->second.copper:found->second.quantity;
                if (amount>available || totals[c.itemGuid]>available-amount) return reject("native_stock_changed");
                totals[c.itemGuid]+=amount;
            }
            if (!excluded.empty()) excluded+=',';
            excluded+=SqlValue(c.id);prepared.fingerprint+='|'+SettlementClaimWhere(c);
            prepared.guards+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+SettlementClaimWhere(c)+')';
            auto released=c;released.state="released";++released.revision;
            prepared.claims.push_back({released,c.revision});
        }
        if (batch.complete)
            prepared.guards+=" AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id"
                " AND c.state NOT IN ('consumed','released')"+(excluded.empty()?"":" AND c.claim_id NOT IN ("+excluded+')')+')';
        result=std::move(prepared);blocker.clear();return true;
    }
    inline void AppendPersonalResourceSettlement(WritePlan& plan,const PersonalResourceSettlement& prepared,
        const UnsettledClaimBatch& batch,uint64_t now,const std::string& receipt) {
        const auto accepted=plan.receiptQuery;
        for (size_t i=0;i<batch.claims.size();++i) {
            const auto& c=batch.claims[i];const auto& after=prepared.claims[i].after;
            plan.statements.push_back("UPDATE living_activity_claim c JOIN living_activity_task t ON t.task_id=c.task_id"
                " SET c.state='released',c.revision="+std::to_string(after.revision)+",c.updated_at_ms="+std::to_string(now)+
                " WHERE "+SettlementClaimWhere(c)+" AND t.last_receipt_id="+SqlValue(receipt)+" AND EXISTS ("+accepted+')');
            plan.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+SettlementClaimWhere(after)+')';
        }
    }
}

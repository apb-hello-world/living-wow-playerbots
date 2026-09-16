#include "LivingActivityResources.h"
#include "LivingActivityJournal.h"
#include <algorithm>
#include <set>
#include <stdexcept>
#include <tuple>

namespace LivingActivity {
    namespace {
        std::string Number(uint64_t value) { return std::to_string(value); }
        std::vector<std::pair<std::string, std::string>> Fields(const ResourceClaim& claim) {
            return {{"claim_id",SqlValue(claim.id)}, {"task_id",SqlValue(claim.task)},
                {"actor_guid",Number(claim.actor)}, {"item_guid",Number(claim.itemGuid)},
                {"item_entry",Number(claim.itemEntry)}, {"quantity",Number(claim.quantity)},
                {"copper",Number(claim.copper)}, {"location",SqlValue(claim.location)},
                {"native_reference",Number(claim.nativeReference)}, {"state",SqlValue(claim.state)},
                {"revision",Number(claim.revision)}};
        }
        std::string NativeKey(const ResourceClaim& claim) {
            return "c.actor_guid=" + Number(claim.actor) + " AND c.task_id=" + SqlValue(claim.task) +
                " AND c.item_guid=" + Number(claim.itemGuid) + " AND c.item_entry=" + Number(claim.itemEntry) +
                " AND c.location=" + SqlValue(claim.location) + " AND c.native_reference=" + Number(claim.nativeReference);
        }
    }
    WritePlan PersonalClaimLocationWrite(const Task& task,uint64_t expected,const std::string& receipt,
        const PersonalClaimLocation& observed) {
        if(!ValidPersonalClaimLocation(observed) || !expected || task.revision!=expected+1 ||
            task.mode!=Mode::Active || !task.accepted || task.root!=task.id || !task.parent.empty() ||
            Terminal(task.phase) || task.phase==Phase::Executing || observed.before.task!=task.id || observed.before.actor!=task.actor)
            throw std::invalid_argument("Exact nonexecuting personal claim location reconciliation required");
        const auto& old=observed.before;auto after=old;++after.revision;after.location=observed.native.location;
        std::string fingerprint;for(const auto& f:Fields(old))fingerprint+=f.first+'='+f.second+'|';
        fingerprint+="observed:"+after.location+':'+Number(observed.bagGuid)+':'+Number(observed.bagSlot)+':'+Number(observed.slot);
        auto plan=Detail::TaskTransitionWrite(task,expected,receipt,"resource_claim_location_reconciled",fingerprint);
        auto& update=plan.statements.front();
        const auto actor=Number(task.actor),guid=Number(old.itemGuid),entry=Number(old.itemEntry);
        std::string exact;for(const auto& f:Fields(old)){if(!exact.empty())exact+=" AND ";exact+="c."+f.first+'='+f.second;}
        update+=" AND mode='active' AND accepted=1 AND phase="+SqlValue(Name(task.phase))+
            " AND checkpoint="+SqlValue(task.checkpoint.data)+
            " AND NOT EXISTS(SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id"
            " WHERE t.actor_guid="+actor+" AND o.state IN ('intent','reconciling'))"+
            " AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+exact+')'+
            " AND (SELECT COUNT(*) FROM living_activity_claim c WHERE (c.item_guid="+guid+
            " OR (c.actor_guid="+actor+" AND c.item_entry="+entry+" AND c.item_guid=0))"
            " AND c.state IN ('held','in_transfer','reconciling'))=1"+
            " AND EXISTS(SELECT 1 FROM item_instance i JOIN character_inventory v ON v.item=i.guid WHERE i.guid="+guid+
            " AND i.owner_guid="+actor+" AND v.guid="+actor+" AND i.itemEntry="+entry+" AND i.count="+Number(old.quantity)+
            " AND v.bag="+Number(observed.bagGuid)+" AND v.slot="+Number(observed.slot)+')'+
            " AND (SELECT COUNT(*) FROM character_inventory WHERE item="+guid+")=1"+
            " AND NOT EXISTS(SELECT 1 FROM mail_items WHERE item_guid="+guid+')'+
            " AND NOT EXISTS(SELECT 1 FROM guild_bank_item WHERE item_guid="+guid+')'+
            " AND NOT EXISTS(SELECT 1 FROM auction WHERE itemguid="+guid+')';
        if(observed.bagGuid)update+=" AND EXISTS(SELECT 1 FROM character_inventory b JOIN item_instance i ON i.guid=b.item"
            " WHERE b.guid="+actor+" AND i.owner_guid=b.guid AND b.item="+Number(observed.bagGuid)+
            " AND b.bag=0 AND b.slot="+Number(observed.bagSlot)+')';
        plan.statements.insert(plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+actor);
        const auto accepted=plan.receiptQuery;
        plan.statements.push_back("UPDATE living_activity_claim c SET c.location="+SqlValue(after.location)+
            ",c.revision="+Number(after.revision)+",c.updated_at_ms="+Number(task.updatedAtMs)+
            " WHERE "+exact+" AND EXISTS("+accepted+')');
        std::string settled;for(const auto& f:Fields(after)){if(!settled.empty())settled+=" AND ";settled+="c."+f.first+'='+f.second;}
        plan.receiptQuery+=" AND EXISTS(SELECT 1 FROM living_activity_claim c WHERE "+settled+')';
        return plan;
    }
    WritePlan BagClaimCoalescenceWrite(const Task& task,uint64_t expected,const std::string& receipt,
        const BagClaimCoalescence& folded,const NativeResourceBalance& balance) {
        if(!ValidBagClaimCoalescence(folded) || !ValidNativeResourceBalance(balance) || !expected ||
            task.mode!=Mode::Active || !task.accepted || task.root!=task.id ||
            (task.phase!=Phase::Preparing && task.phase!=Phase::Traveling && task.phase!=Phase::Verifying && task.phase!=Phase::Reconciling))
            throw std::invalid_argument("Exact nonexecuting bag claim coalescence required");
        const auto& first=folded.before.front();
        if(first.task!=task.id || first.actor!=task.actor || balance.actor!=task.actor || balance.itemGuid!=first.itemGuid ||
            balance.itemEntry!=first.itemEntry || balance.location!="bags" || balance.nativeReference || balance.copper ||
            balance.quantity<folded.changes.front().after.quantity)
            throw std::invalid_argument("Coalescence must conserve one owned native stack");
        std::string fingerprint="balance:"+Number(balance.quantity)+'|';
        for(const auto& old:folded.before)for(const auto& field:Fields(old))fingerprint+=field.first+'='+field.second+'|';
        // Preserve an existing verification/reconciliation phase; the SQL
        // below requires that exact predecessor phase and no unresolved effect.
        auto plan=Detail::TaskTransitionWrite(task,expected,receipt,"resource_claims_coalesced",fingerprint);
        auto& update=plan.statements.front();
        update+=" AND mode='active' AND phase="+SqlValue(Name(task.phase))+
            " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
            "WHERE t.actor_guid="+Number(task.actor)+" AND o.state IN ('intent','reconciling'))";
        for(const auto& old:folded.before) {
            std::string exact;for(const auto& f:Fields(old)) {if(!exact.empty())exact+=" AND ";exact+="c."+f.first+'='+f.second;}
            update+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+exact+')';
        }
        const auto guid=Number(first.itemGuid),actor=Number(task.actor),entry=Number(first.itemEntry);
        update+=" AND EXISTS (SELECT 1 FROM item_instance i JOIN character_inventory v ON v.item=i.guid WHERE i.guid="+guid+
            " AND i.owner_guid="+actor+" AND v.guid="+actor+" AND i.itemEntry="+entry+" AND i.count="+Number(balance.quantity)+
            " AND ((v.bag=0 AND v.slot BETWEEN 23 AND 38) OR (v.bag<>0 AND EXISTS (SELECT 1 FROM character_inventory b "
            "WHERE b.item=v.bag AND b.guid="+actor+" AND b.bag=0 AND b.slot BETWEEN 19 AND 22))))"+
            " AND NOT EXISTS (SELECT 1 FROM mail_items m WHERE m.item_guid="+guid+')'+
            " AND NOT EXISTS (SELECT 1 FROM guild_bank_item g WHERE g.item_guid="+guid+')'+
            " AND (SELECT COALESCE(SUM(c.quantity),0) FROM living_activity_claim c WHERE (c.item_guid="+guid+
            " OR (c.item_guid=0 AND c.actor_guid="+actor+" AND c.item_entry="+entry+")) AND c.state IN ('held','in_transfer','reconciling'))<="+Number(balance.quantity);
        plan.statements.insert(plan.statements.begin(),"UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid="+actor);
        const auto accepted=plan.receiptQuery;
        for(const auto& change:folded.changes) {
            const auto& c=change.after;std::string exact;
            for(const auto& f:Fields(c)){if(!exact.empty())exact+=" AND ";exact+="c."+f.first+'='+f.second;}
            plan.statements.push_back("UPDATE living_activity_claim c SET c.quantity="+Number(c.quantity)+",c.state="+SqlValue(c.state)+
                ",c.revision="+Number(c.revision)+",c.updated_at_ms="+Number(task.updatedAtMs)+" WHERE c.claim_id="+SqlValue(c.id)+
                " AND c.revision="+Number(change.expectedRevision)+" AND EXISTS ("+accepted+')');
            plan.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+exact+')';
        }
        return plan;
    }
    WritePlan ResourceReservationWrite(const Task& task, uint64_t expected, const std::string& receipt,
        std::vector<ClaimReceiptChange> changes, std::vector<NativeResourceBalance> balances) {
        if (!expected || task.mode != Mode::Active || task.phase != Phase::Preparing ||
            changes.empty() || changes.size() > 16 || balances.size() > 16)
            throw std::invalid_argument("Invalid reservation batch");
        std::sort(changes.begin(),changes.end(),[](const auto& a,const auto& b){return a.after.id < b.after.id;});
        std::sort(balances.begin(),balances.end(),[](const auto& a,const auto& b){return a.itemGuid < b.itemGuid;});
        std::set<std::string> ids;
        std::map<uint32_t, NativeResourceBalance> limits;
        std::string fingerprint, excluded;
        for (const auto& balance : balances) {
            if (balance.actor != task.actor || !ValidNativeResourceBalance(balance) ||
                !limits.emplace(balance.itemGuid,balance).second)
                throw std::invalid_argument("Invalid native reservation balance");
            fingerprint += "balance:" + Number(balance.actor) + ':' + Number(balance.itemGuid) + ':' +
                Number(balance.itemEntry) + ':' + Number(balance.quantity) + ':' + Number(balance.copper) + ':' + balance.location + '|';
            // Keep historical bag/bank/money operation fingerprints unchanged.
            if (balance.nativeReference) fingerprint += "native_ref:"+Number(balance.nativeReference)+'|';
        }
        std::map<uint32_t,uint64_t> requested;
        for (const auto& change : changes) {
            const auto& claim = change.after;
            if (!ValidResourceClaim(claim) || claim.task != task.id || claim.actor != task.actor ||
                claim.revision != change.expectedRevision + 1 || !ids.insert(claim.id).second ||
                (claim.state != "held" && claim.state != "proposed" && claim.state != "released") ||
                (!change.expectedRevision && claim.state == "released"))
                throw std::invalid_argument("Reservation cannot transfer or consume resources");
            if (!excluded.empty()) excluded += ',';
            excluded += SqlValue(claim.id);
            fingerprint += "expected:" + Number(change.expectedRevision) + '|';
            for (const auto& field : Fields(claim)) fingerprint += field.first + '=' + field.second + '|';
            if (claim.state == "held") {
                const auto balance = limits.find(claim.itemGuid);
                if (balance == limits.end() || balance->second.itemEntry != claim.itemEntry ||
                    balance->second.location != claim.location || balance->second.nativeReference!=claim.nativeReference)
                    throw std::invalid_argument("Held reservation requires an owned native balance");
                requested[claim.itemGuid] += claim.copper ? claim.copper : claim.quantity;
            } else if (claim.state == "proposed" && (claim.itemGuid || claim.nativeReference))
                throw std::invalid_argument("Speculative demand is not a physical reservation");
        }
        auto plan = TaskWrite(task,expected,receipt,"resources_reserved",fingerprint);
        auto& update = plan.statements.front();
        update += " AND mode='active' AND phase IN ('queued','preparing','traveling') "
            "AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task ot ON ot.task_id=o.task_id "
            "WHERE ot.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))";
        for (const auto& change : changes) {
            const auto& claim = change.after;
            if (!change.expectedRevision)
                update += " AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.claim_id=" + SqlValue(claim.id) + ')';
            else
                update += " AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.claim_id=" + SqlValue(claim.id) +
                    " AND c.revision=" + Number(change.expectedRevision) + " AND " + NativeKey(claim) +
                    " AND c.state IN ('proposed','held'))";
        }
        for (const auto& pair : requested) {
            const auto& balance = limits.at(pair.first);
            const bool money = balance.location == "money";
            const std::string quantity = money ? "copper" : "quantity";
            const std::string match = money ? "c.actor_guid=" + Number(task.actor) + " AND c.location='money'" :
                "(c.item_guid=" + Number(balance.itemGuid) + " OR (c.item_guid=0 AND c.actor_guid=" +
                    Number(task.actor) + " AND c.item_entry=" + Number(balance.itemEntry) + "))";
            update += " AND ((SELECT COALESCE(SUM(c." + quantity + "),0) FROM living_activity_claim c WHERE " + match +
                " AND c.state IN ('held','in_transfer','reconciling') AND c.claim_id NOT IN (" + excluded + "))+" +
                Number(pair.second) + "<=" + Number(money ? balance.copper : balance.quantity) + ')';
        }
        // Serialize reservations for an actor across its different root tasks.
        // This is a no-op row lock, not a native-character save or a new table.
        // All following statements and the receipt remain in one transaction.
        plan.statements.insert(plan.statements.begin(),
            "UPDATE living_activity_task SET actor_guid=actor_guid WHERE actor_guid=" + Number(task.actor));
        const auto accepted = plan.receiptQuery; // Exact fingerprint, not just a reused receipt ID.
        for (const auto& change : changes) {
            const auto& claim = change.after;
            std::string columns,values,updates,exact;
            for (const auto& field : Fields(claim)) {
                if (!columns.empty()) { columns += ','; values += ','; updates += ','; exact += " AND "; }
                columns += field.first; values += field.second;
                updates += "c." + field.first + '=' + field.second;
                exact += "c." + field.first + '=' + field.second;
            }
            columns += ",updated_at_ms"; values += ',' + Number(task.updatedAtMs);
            updates += ",c.updated_at_ms=" + Number(task.updatedAtMs);
            if (!change.expectedRevision)
                plan.statements.push_back("INSERT INTO living_activity_claim (" + columns + ") SELECT " + values +
                    " FROM living_activity_task t WHERE t.task_id=" + SqlValue(task.id) +
                    " AND t.last_receipt_id=" + SqlValue(receipt) + " AND EXISTS (" + accepted +
                    ") ON DUPLICATE KEY UPDATE claim_id=claim_id");
            else
                plan.statements.push_back("UPDATE living_activity_claim c JOIN living_activity_task t ON t.task_id=c.task_id SET " +
                    updates + " WHERE c.claim_id=" + SqlValue(claim.id) + " AND c.revision=" + Number(change.expectedRevision) +
                    " AND t.last_receipt_id=" + SqlValue(receipt) + " AND EXISTS (" + accepted + ')');
            plan.receiptQuery += " AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE " + exact + ')';
        }
        return plan;
    }
}

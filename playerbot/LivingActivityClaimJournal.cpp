#include "LivingActivityResources.h"
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
            if (balance.actor != task.actor ||
                (balance.location == "money" ? (balance.itemGuid || balance.itemEntry || balance.quantity) :
                    (!balance.itemGuid || !balance.itemEntry || balance.copper ||
                     (balance.location != "bags" && balance.location != "bank"))) ||
                !limits.emplace(balance.itemGuid,balance).second)
                throw std::invalid_argument("Invalid native reservation balance");
            fingerprint += "balance:" + Number(balance.actor) + ':' + Number(balance.itemGuid) + ':' +
                Number(balance.itemEntry) + ':' + Number(balance.quantity) + ':' + Number(balance.copper) + ':' + balance.location + '|';
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
                if (claim.nativeReference || balance == limits.end() || balance->second.itemEntry != claim.itemEntry ||
                    balance->second.location != claim.location)
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

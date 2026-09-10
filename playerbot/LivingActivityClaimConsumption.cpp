#include "LivingActivityClaimConsumption.h"
#include <algorithm>
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
namespace LivingActivity {
    namespace {
        std::vector<ClaimConsumption> Canonical(const std::vector<ClaimConsumption>& input) {
            if (input.empty() || input.size() > 16) throw std::invalid_argument("Bounded consumption claims required");
            auto sorted=input;
            std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.before.id < b.before.id;});
            std::set<std::string> ids;
            for (const auto& use : sorted) {
                const auto& c=use.before;
                if (!ValidResourceClaim(c) || c.state != "held" || c.nativeReference ||
                    (c.location != "money" && c.location != "bags") || !use.used ||
                    use.used > c.quantity+c.copper || !ids.insert(c.id).second ||
                    c.revision >= std::numeric_limits<uint64_t>::max()-1)
                    throw std::invalid_argument("Exact held native consumption required");
            }
            return sorted;
        }
        std::string ClaimJson(const std::vector<ClaimConsumption>& sorted) {
            std::string out="[";
            for (const auto& use : sorted) {
                const auto& c=use.before;
                if (out.size() > 1) out+=',';
                // UUIDs and the finite locations have already been validated.
                out+="{\"claim\":\""+c.id+"\",\"task\":\""+c.task+"\",\"actor\":"+std::to_string(c.actor)+
                    ",\"revision\":"+std::to_string(c.revision)+",\"item_guid\":"+std::to_string(c.itemGuid)+
                    ",\"item_entry\":"+std::to_string(c.itemEntry)+",\"quantity\":"+std::to_string(c.quantity)+
                    ",\"copper\":"+std::to_string(c.copper)+",\"location\":\""+c.location+
                    "\",\"used\":"+std::to_string(use.used)+'}';
            }
            return out+']';
        }
        std::string Predicate(const ResourceClaim& c) {
            return "c.claim_id="+SqlValue(c.id)+" AND c.task_id="+SqlValue(c.task)+
                " AND c.actor_guid="+std::to_string(c.actor)+" AND c.item_guid="+std::to_string(c.itemGuid)+
                " AND c.item_entry="+std::to_string(c.itemEntry)+" AND c.quantity="+std::to_string(c.quantity)+
                " AND c.copper="+std::to_string(c.copper)+" AND c.location="+SqlValue(c.location)+
                " AND c.native_reference="+std::to_string(c.nativeReference)+" AND c.state="+SqlValue(c.state)+
                " AND c.revision="+std::to_string(c.revision);
        }
    }
    std::string ClaimedNativeState(const std::string& nativeState,
        const std::vector<ClaimConsumption>& consumption,size_t limit) {
        if (nativeState.empty() || nativeState.size() > 4096 || limit > 8192 ||
            nativeState.find_first_not_of(" \t\r\n") == std::string::npos ||
            nativeState[nativeState.find_first_not_of(" \t\r\n")] != '{')
            throw std::invalid_argument("Native state must be a bounded JSON object");
        boost::property_tree::ptree parsed; std::istringstream in(nativeState);
        boost::property_tree::read_json(in,parsed);
        auto out="{\"native\":"+nativeState+",\"claimed_consumption\":"+ClaimJson(Canonical(consumption))+'}';
        if (out.size() > limit) throw std::invalid_argument("Claimed state exceeds journal bound");
        return out;
    }
    ClaimedOutcome ConsumedOperationWrite(const Task& task,uint64_t expected,
        const OperationResult& result,const std::string& receipt,const std::string& nativeAfter,
        const std::vector<ClaimConsumption>& consumption) {
        if (result.state != OperationState::Verified)
            throw std::invalid_argument("Uncertain or rejected work cannot consume claims");
        const auto sorted=Canonical(consumption);
        ClaimedOutcome out;
        const auto claims=ClaimJson(sorted);
        out.journal=OperationOutcomeWrite(task,expected,result,receipt,ClaimedNativeState(nativeAfter,sorted,8192));
        // The operation's saved intent must name these SAME claims and amounts.
        // A caller cannot select a different job's stock after seeing an effect.
        out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id="+
            SqlValue(result.id)+" AND JSON_COMPACT(JSON_EXTRACT(o.before_state,'$.native.claimed_consumption'))="+
            SqlValue(claims)+')';
        const std::string exactReceipt=out.journal.receiptQuery;
        for (const auto& use : sorted) {
            const auto& before=use.before;
            if (before.actor != task.actor || (before.task != task.id && before.task != task.root))
                throw std::invalid_argument("Claim must belong to the executing actor and commitment");
            ResourceClaim after=before; ++after.revision;
            const uint64_t remaining=before.quantity+before.copper-use.used;
            if (!remaining) after.state="consumed"; // Retain original amount as terminal audit data.
            else if (before.copper) after.copper=remaining;
            else after.quantity=remaining;
            out.changes.push_back({after,before.revision});
            out.journal.statements.front()+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+Predicate(before)+')';
            out.journal.statements.push_back("UPDATE living_activity_claim c SET c.quantity="+std::to_string(after.quantity)+
                ",c.copper="+std::to_string(after.copper)+",c.state="+SqlValue(after.state)+
                ",c.revision="+std::to_string(after.revision)+",c.updated_at_ms="+std::to_string(task.updatedAtMs)+
                " WHERE "+Predicate(before)+" AND EXISTS ("+exactReceipt+")");
            out.journal.receiptQuery+=" AND EXISTS (SELECT 1 FROM living_activity_claim c WHERE "+Predicate(after)+')';
        }
        return out;
    }
}

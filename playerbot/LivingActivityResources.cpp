#include "LivingActivityResources.h"
#include <algorithm>
#include <exception>
#include <limits>
#include <set>
#include <tuple>

namespace LivingActivity {
    namespace {
        bool TerminalClaim(const ResourceClaim& claim) {
            return claim.state == "consumed" || claim.state == "released";
        }
        template<class Map, class Key> uint64_t Lookup(const Map& values, const Key& key) {
            const auto found = values.find(key); return found == values.end() ? 0 : found->second;
        }
        template<class Map, class Key> void Add(Map& values, const Key& key, uint64_t amount, bool add) {
            if (add) values[key] += amount;
            else {
                auto found = values.find(key);
                // Every removal is from a previously validated indexed row.
                if (found == values.end() || found->second < amount) std::terminate();
                found->second -= amount;
                if (!found->second) values.erase(found);
            }
        }
        uint32_t Available(uint32_t native, uint64_t protectedAmount) {
            return protectedAmount >= native ? 0 : native - uint32_t(protectedAmount);
        }
    }
    bool ValidResourceClaim(const ResourceClaim& claim) {
        if (!IsUuid(claim.id) || !IsUuid(claim.task) || !claim.actor || !claim.revision ||
            claim.revision == std::numeric_limits<uint64_t>::max() ||
            claim.quantity > std::numeric_limits<uint32_t>::max() ||
            claim.copper > std::numeric_limits<uint32_t>::max()) return false;
        if (claim.state != "proposed" && claim.state != "held" && claim.state != "in_transfer" &&
            claim.state != "reconciling" && !TerminalClaim(claim)) return false;
        if (claim.location != "bags" && claim.location != "bank" && claim.location != "mail" &&
            claim.location != "auction" && claim.location != "guild_bank" &&
            claim.location != "trade" && claim.location != "money") return false;
        if (claim.copper) {
            if (claim.quantity || claim.itemGuid || claim.itemEntry ||
                (claim.location != "money" && claim.location != "mail" &&
                 claim.location != "auction" && claim.location != "trade")) return false;
        } else {
            if (!claim.quantity || !claim.itemEntry || claim.location == "money") return false;
            // Unknown legacy identity stays conservatively protected by entry
            // while reconciling; it can never be treated as an executable item.
            if (!claim.itemGuid && (claim.state == "held" || claim.state == "in_transfer")) return false;
        }
        if ((claim.location == "mail" || claim.location == "auction" ||
            claim.location == "guild_bank" || claim.location == "trade") &&
            !claim.nativeReference && claim.state != "proposed" && claim.state != "reconciling") return false;
        return true;
    }
    bool ProtectsResources(const ResourceClaim& claim) {
        return claim.state == "held" || claim.state == "in_transfer" || claim.state == "reconciling";
    }
    bool SameResourceClaim(const ResourceClaim& a, const ResourceClaim& b) {
        return std::tie(a.id,a.task,a.actor,a.itemGuid,a.itemEntry,a.quantity,a.copper,a.location,a.nativeReference,a.state,a.revision) ==
            std::tie(b.id,b.task,b.actor,b.itemGuid,b.itemEntry,b.quantity,b.copper,b.location,b.nativeReference,b.state,b.revision);
    }
    uint64_t ResourceProtection::ProtectedItem(uint32_t actor, uint32_t guid, uint32_t entry) const {
        if (!ready || !actor || !guid || !entry) return std::numeric_limits<uint64_t>::max();
        // GUID ownership follows the physical item, even between actors. An
        // uncertain entry-only claim conservatively protects matching stacks.
        const auto exact = Lookup(items, guid), uncertain = Lookup(uncertainEntries, std::make_pair(actor, entry));
        return exact > std::numeric_limits<uint64_t>::max() - uncertain ?
            std::numeric_limits<uint64_t>::max() : exact + uncertain;
    }
    uint64_t ResourceProtection::ProtectedMoney(uint32_t actor) const {
        return !ready || !actor ? std::numeric_limits<uint64_t>::max() : Lookup(money, actor);
    }
    uint32_t ResourceProtection::UnreservedItem(uint32_t actor, uint32_t guid, uint32_t entry, uint32_t nativeCount) const {
        return Available(nativeCount, ProtectedItem(actor, guid, entry));
    }
    uint32_t ResourceProtection::UnreservedMoney(uint32_t actor, uint32_t nativeCopper) const {
        return Available(nativeCopper, ProtectedMoney(actor));
    }
    const ResourceClaim* ResourceClaimBook::Inspect(const std::string& id) const {
        const auto found = records.find(id); return found == records.end() ? nullptr : &found->second;
    }
    void ResourceClaimBook::Index(const ResourceClaim& claim, bool add) {
        if (!ProtectsResources(claim)) return;
        if (claim.copper) {
            // Native mail/auction escrow is not also spendable wallet money.
            if (claim.location == "money") Add(protection.money, claim.actor, claim.copper, add);
        } else if (claim.itemGuid) Add(protection.items, claim.itemGuid, claim.quantity, add);
        else Add(protection.uncertainEntries, std::make_pair(claim.actor, claim.itemEntry), claim.quantity, add);
    }
    ClaimInstall ResourceClaimBook::RestoreBatch(const std::vector<ResourceClaim>& rows) {
        if (protection.ready) return ClaimInstall::NotReady;
        if (restoreFailed) return ClaimInstall::Invalid;
        if (rows.empty() || rows.size() > 64) { restoreFailed = true; return ClaimInstall::Invalid; }
        std::set<std::string> ids;
        size_t added = 0;
        for (const auto& row : rows) {
            const auto* old = Inspect(row.id);
            if (!ValidResourceClaim(row) || !ids.insert(row.id).second || (old && !SameResourceClaim(*old, row))) {
                restoreFailed = true; return ClaimInstall::Invalid;
            }
            added += old == nullptr;
        }
        if (added > capacity || records.size() > capacity - added) {
            restoreFailed = true; return ClaimInstall::Capacity;
        }
        for (const auto& row : rows) if (!Inspect(row.id)) {
            records.emplace(row.id, row); Index(row, true);
        }
        return added ? ClaimInstall::Installed : ClaimInstall::Duplicate;
    }
    bool ResourceClaimBook::FinishRestore() {
        if (restoreFailed) return false;
        if (!protection.ready) { protection.ready = true; ++protection.revision; }
        return true;
    }
    ClaimInstall ResourceClaimBook::InstallReceipt(const std::vector<ClaimReceiptChange>& changes) {
        if (!protection.ready) return ClaimInstall::NotReady;
        if (changes.empty() || changes.size() > 16 || protection.revision == std::numeric_limits<uint64_t>::max())
            return ClaimInstall::Invalid;
        std::set<std::string> ids;
        size_t added = 0, duplicates = 0;
        for (const auto& change : changes) {
            const auto& after = change.after; const auto* old = Inspect(after.id);
            if (!ValidResourceClaim(after) || !ids.insert(after.id).second ||
                change.expectedRevision >= std::numeric_limits<uint64_t>::max() - 1 ||
                after.revision != change.expectedRevision + 1) return ClaimInstall::Invalid;
            if (old && SameResourceClaim(*old, after)) { ++duplicates; continue; }
            if (old) {
                if (old->revision != change.expectedRevision) return ClaimInstall::Stale;
                if (TerminalClaim(*old) || old->task != after.task || old->actor != after.actor ||
                    old->itemEntry != after.itemEntry || bool(old->copper) != bool(after.copper)) return ClaimInstall::Invalid;
            } else {
                if (change.expectedRevision) return ClaimInstall::Stale;
                ++added;
            }
        }
        // A receipt is atomic. A partially installed batch indicates a broken
        // projection, not permission to fill in the other half speculatively.
        if (duplicates) return duplicates == changes.size() ? ClaimInstall::Duplicate : ClaimInstall::Invalid;
        if (added > capacity || records.size() > capacity - added) return ClaimInstall::Capacity;
        for (const auto& change : changes) {
            if (const auto* old = Inspect(change.after.id)) Index(*old, false);
            records[change.after.id] = change.after;
            Index(change.after, true);
        }
        ++protection.revision;
        return ClaimInstall::Installed;
    }
}

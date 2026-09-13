#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace LivingActivity {
// A planning preference, never an execution grant or receipt. Native code
// supplies a validated known recipe and a bounded snapshot of its materials.
enum class ProfessionCandidateReadiness { Carried, Banked, Obtainable, Unavailable };
struct ProfessionCandidateMaterial {
    uint32_t required=0, carried=0, banked=0;
    bool protectedStock=false, unlinkedMail=false, sourceAvailable=false;
};
inline ProfessionCandidateReadiness RankProfessionCandidate(
    const std::vector<ProfessionCandidateMaterial>& materials, std::string& blocker) {
    auto reject=[&](const char* why) {blocker=why; return ProfessionCandidateReadiness::Unavailable;};
    if (materials.empty() || materials.size()>8) return reject("profession_candidate_material_snapshot_invalid");
    auto rank=ProfessionCandidateReadiness::Carried;
    for (const auto& material:materials) {
        if (!material.required || material.required>10000) return reject("profession_candidate_material_snapshot_invalid");
        // Never infer a recipe from a matching mailbox attachment or purchase
        // a replacement for another obligation's protected possessions.
        if (material.unlinkedMail) return reject("profession_incoming_material_requires_reconciliation");
        if (material.protectedStock) return reject("profession_material_has_existing_commitment");
        if (material.carried>=material.required) continue;
        if (uint64_t(material.carried)+material.banked>=material.required)
            rank=std::max(rank,ProfessionCandidateReadiness::Banked);
        else if (material.sourceAvailable)
            rank=ProfessionCandidateReadiness::Obtainable;
        else return reject("profession_candidate_material_source_unavailable");
    }
    blocker.clear();return rank;
}
}

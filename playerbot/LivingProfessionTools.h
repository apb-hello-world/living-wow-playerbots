#pragma once
#include "LivingProfessionJob.h"
#include <algorithm>
#include <tuple>

namespace LivingActivity {
    // Native code supplies only compatible usable items. Commitments outrank
    // newly found alternatives, then carried/banked possessions, then sources.
    // A tool is retained, never added to the recipe's consumed reagents.
    struct ProfessionToolCandidate {
        uint32_t entry=0;
        bool committed=false,carried=false,banked=false,vendor=false,auction=false;
        auto Rank() const {return std::make_tuple(!committed,!carried,!banked,!vendor,!auction,entry);}
    };
    inline uint32_t ChooseProfessionTool(const std::vector<ProfessionToolCandidate>& candidates) {
        const ProfessionToolCandidate* best=nullptr;
        for (const auto& c:candidates) {
            if (!c.entry || !(c.committed || c.carried || c.banked || c.vendor || c.auction)) continue;
            if (!best || c.Rank()<best->Rank()) best=&c;
        }
        return best?best->entry:0;
    }
    inline bool ValidProfessionTools(const ProfessionJob& job,const std::vector<ProfessionReagent>& tools) {
        if (tools.size()>4) return false;
        uint32_t previous=0;
        for (const auto& tool:tools) {
            if (!tool.entry || tool.entry<=previous || tool.perAttempt!=1 || tool.entry==job.outputEntry ||
                std::any_of(job.reagents.begin(),job.reagents.end(),[&](const auto& r){return r.entry==tool.entry;})) return false;
            previous=tool.entry;
        }
        return true;
    }
}

#ifndef LIVING_SERVICE_SELECTION_H
#define LIVING_SERVICE_SELECTION_H
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>

namespace LivingActivity {
    // Attribution is supplied by the saved service executor, not chat text.
    // This changes preference only; native eligibility and target authority
    // still validate the selected point before anything moves.
    inline bool PreferNearbyService(const std::string& origin) {
        return origin=="profession_service_travel";
    }
    inline auto ServiceChoiceRank(double distance,int32_t entry,uint32_t map,double x,double y,double z) {
        const bool valid=std::isfinite(distance) && distance>=0 && std::isfinite(x) && std::isfinite(y) && std::isfinite(z);
        return std::make_tuple(!valid,valid?distance:std::numeric_limits<double>::infinity(),entry,map,
            std::isfinite(x)?x:0,std::isfinite(y)?y:0,std::isfinite(z)?z:0);
    }
}
#endif

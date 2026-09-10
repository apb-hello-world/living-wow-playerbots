#ifndef LIVING_ACTIVITY_CLAIM_CODEC_H
#define LIVING_ACTIVITY_CLAIM_CODEC_H
#include "LivingActivityResources.h"
#include <boost/property_tree/json_parser.hpp>
#include <limits>
#include <sstream>

namespace LivingActivity {
    inline std::string PersistedClaimProjection() {
        return "JSON_OBJECT('id',c.claim_id,'task',c.task_id,'actor',c.actor_guid,'item_guid',c.item_guid,"
            "'item_entry',c.item_entry,'quantity',c.quantity,'copper',c.copper,'location',c.location,"
            "'reference',c.native_reference,'state',c.state,'revision',c.revision)";
    }
    inline bool DecodeClaimProjection(const std::string& payload, ResourceClaim& claim, std::string& error) {
        error = "invalid_claim_projection";
        if (payload.empty() || payload.size() > 2048) return false;
        try {
            boost::property_tree::ptree p; std::istringstream input(payload);
            boost::property_tree::read_json(input,p);
            if (p.size() != 11) return false;
            for (const auto* field : {"id","task","actor","item_guid","item_entry","quantity","copper","location","reference","state","revision"})
                if (p.count(field) != 1 || !p.get_child(field).empty()) return false;
            auto number = [&](const char* field, uint64_t maximum) {
                const auto value = p.get<std::string>(field);
                if (value.empty() || value.size() > 20 || value.find_first_not_of("0123456789") != std::string::npos)
                    throw std::invalid_argument("claim_number");
                const auto result = std::stoull(value);
                if (result > maximum) throw std::invalid_argument("claim_number_range");
                return result;
            };
            ResourceClaim decoded;
            decoded.id = p.get<std::string>("id"); decoded.task = p.get<std::string>("task");
            decoded.actor = number("actor",std::numeric_limits<uint32_t>::max());
            decoded.itemGuid = number("item_guid",std::numeric_limits<uint32_t>::max());
            decoded.itemEntry = number("item_entry",std::numeric_limits<uint32_t>::max());
            decoded.quantity = number("quantity",std::numeric_limits<uint32_t>::max());
            decoded.copper = number("copper",std::numeric_limits<uint32_t>::max());
            decoded.nativeReference = number("reference",std::numeric_limits<uint64_t>::max());
            decoded.revision = number("revision",std::numeric_limits<uint64_t>::max());
            decoded.location = p.get<std::string>("location"); decoded.state = p.get<std::string>("state");
            if (!ValidResourceClaim(decoded)) { error = "invalid_claim_record"; return false; }
            claim = std::move(decoded); error.clear(); return true;
        } catch (const std::exception&) { return false; }
    }
}
#endif

#pragma once
#include "LivingEnchantCapture.h"
#include "LivingActivityClaimCodec.h"

namespace LivingActivity {
    inline bool ValidEnchantClaim(const Task& task,const ProfessionJob& job,const ResourceClaim& claim) {
        return job.operation==ProfessionOperation::EnchantItem && ValidResourceClaim(claim) && claim.task==task.root &&
            claim.actor==task.actor && claim.itemGuid==job.subjectItem && claim.quantity==1 && !claim.copper &&
            !claim.nativeReference && claim.state=="held" && (claim.location=="bags" || claim.location=="equipment");
    }
    inline std::string EnchantClaimJson(const ResourceClaim& c) {
        if (!ValidResourceClaim(c)) throw std::invalid_argument("native_enchant_claim_invalid");
        return "{\"id\":\""+c.id+"\",\"task\":\""+c.task+"\",\"actor\":"+std::to_string(c.actor)+
            ",\"item_guid\":"+std::to_string(c.itemGuid)+",\"item_entry\":"+std::to_string(c.itemEntry)+
            ",\"quantity\":"+std::to_string(c.quantity)+",\"copper\":"+std::to_string(c.copper)+
            ",\"location\":\""+c.location+"\",\"reference\":"+std::to_string(c.nativeReference)+
            ",\"state\":\""+c.state+"\",\"revision\":"+std::to_string(c.revision)+'}';
    }
    inline bool FindEnchantClaim(const Task& task,const ProfessionJob& job,const UnsettledClaimBatch& batch,
        ResourceClaim& claim,std::string& blocker) {
        claim={};
        if (!batch.complete || batch.claims.size()>16) {blocker="native_enchant_claim_batch_incomplete";return false;}
        for (const auto& c:batch.claims) if (c.itemGuid==job.subjectItem) {
            if (!claim.id.empty() || !ValidEnchantClaim(task,job,c)) {
                blocker="native_enchant_subject_claim_changed";return false;
            }
            claim=c;
        }
        blocker=claim.id.empty()?"native_enchant_subject_reservation_required":"";return !claim.id.empty();
    }
    inline std::string EnchantSubjectJson(const EnchantSubject& s) {
        const auto& v=s.item;
        std::string out="{\"actor\":"+std::to_string(v.actor)+",\"guid\":"+std::to_string(v.guid)+
            ",\"entry\":"+std::to_string(v.entry)+",\"count\":"+std::to_string(v.count)+
            ",\"bag\":"+std::to_string(v.bagGuid)+",\"slot\":"+std::to_string(v.slot)+",\"enchantments\":[";
        for (const auto& e:s.enchantments) {
            if (out.back()!='[') out+=',';
            out+='['+std::to_string(e.id)+','+std::to_string(e.duration)+','+std::to_string(e.charges)+']';
        }
        return out+"]}";
    }
    namespace EnchantCodec {
        using Tree=boost::property_tree::ptree;
        inline void Object(const Tree& p,std::initializer_list<const char*> names) {
            if (!p.data().empty() || p.size()!=names.size()) throw std::invalid_argument("native_enchant_object_invalid");
            for (const auto* name:names) if (p.count(name)!=1) throw std::invalid_argument("native_enchant_field_invalid");
        }
        inline uint32_t Number(const Tree& p) {
            const auto& s=p.data();
            if (!p.empty() || s.empty() || s.size()>10 || (s.size()>1 && s.front()=='0') ||
                s.find_first_not_of("0123456789")!=std::string::npos) throw std::invalid_argument("native_enchant_number_invalid");
            const auto n=std::stoull(s);
            if (n>UINT32_MAX) throw std::invalid_argument("native_enchant_number_overflow");
            return uint32_t(n);
        }
        inline std::string Json(const Tree& p) {
            std::ostringstream out;boost::property_tree::write_json(out,p,false);return out.str();
        }
        inline EnchantSubject Subject(const Tree& p) {
            Object(p,{"actor","guid","entry","count","bag","slot","enchantments"});EnchantSubject result;auto& v=result.item;
            v.actor=Number(p.get_child("actor"));v.guid=Number(p.get_child("guid"));v.entry=Number(p.get_child("entry"));
            v.count=Number(p.get_child("count"));v.bagGuid=Number(p.get_child("bag"));const auto slot=Number(p.get_child("slot"));
            if (slot>255) throw std::invalid_argument("native_enchant_slot_invalid");
            v.slot=uint8_t(slot);
            const auto& values=p.get_child("enchantments");
            if (!values.data().empty() || values.empty() || values.size()>16) throw std::invalid_argument("native_enchant_fields_bound");
            for (const auto& row:values) {
                if (!row.first.empty() || !row.second.data().empty() || row.second.size()!=3)
                    throw std::invalid_argument("native_enchant_fields_invalid");
                uint32_t n[3];size_t at=0;
                for (const auto& value:row.second) {
                    if (!value.first.empty()) throw std::invalid_argument("native_enchant_fields_invalid");
                    n[at++]=Number(value.second);
                }
                result.enchantments.push_back({n[0],n[1],n[2]});
            }
            return result;
        }
    }
    struct EnchantIntent {
        ResourceClaim claim;
        EnchantSpec spec;
        EnchantSubject before;
        uint32_t skill=0,money=0;
    };
    inline std::string EncodeEnchantIntent(const ProfessionJob& job,const EnchantIntent& intent) {
        return "{\"recipe\":"+std::to_string(job.recipe)+",\"skill\":"+std::to_string(intent.skill)+
            ",\"money\":"+std::to_string(intent.money)+",\"subject_claim\":"+EnchantClaimJson(intent.claim)+
            ",\"enchantment\":"+std::to_string(intent.spec.id)+",\"subject_before\":"+EnchantSubjectJson(intent.before)+'}';
    }
    inline bool DecodeEnchantIntent(const Task& task,const ProfessionJob& job,const std::string& json,
        EnchantIntent& result,std::string& blocker) {
        result={};
        try {
            if (json.empty() || json.size()>4096) throw std::invalid_argument("native_enchant_intent_bound");
            EnchantCodec::Tree p;std::istringstream in(json);boost::property_tree::read_json(in,p);
            EnchantCodec::Object(p,{"recipe","skill","money","subject_claim","enchantment","subject_before"});
            EnchantIntent value;
            value.skill=EnchantCodec::Number(p.get_child("skill"));value.money=EnchantCodec::Number(p.get_child("money"));
            value.spec.id=EnchantCodec::Number(p.get_child("enchantment"));value.before=EnchantCodec::Subject(p.get_child("subject_before"));
            if (EnchantCodec::Number(p.get_child("recipe"))!=job.recipe || !value.skill ||
                !DecodeClaimProjection(EnchantCodec::Json(p.get_child("subject_claim")),value.claim,blocker) ||
                !ValidEnchantClaim(task,job,value.claim) || !ValidEnchantSubject(value.before,task.actor,job.subjectItem,value.spec) ||
                value.before.item.entry!=value.claim.itemEntry ||
                (value.claim.location=="equipment" ? (value.before.item.bagGuid || value.before.item.slot>=19) :
                 (!value.before.item.bagGuid && (value.before.item.slot<23 || value.before.item.slot>=39))))
                throw std::invalid_argument("native_enchant_intent_identity_invalid");
            result=std::move(value);blocker.clear();return true;
        } catch (const std::exception&) {blocker="native_enchant_intent_invalid";return false;}
    }
}

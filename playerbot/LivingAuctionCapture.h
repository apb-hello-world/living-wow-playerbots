#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <utility>

namespace LivingActivity {
// Captures only native mail emitted synchronously by one purchase. This is
// evidence, never permission or an instruction to create mail. No live pointer
// or scope survives the native handler call.
struct AuctionMail {
    uint32_t id=0, sender=0, receiver=0, money=0, cod=0;
    uint32_t itemGuid=0, itemEntry=0, quantity=0, attachments=0;
    uint64_t deliveredAt=0, expiresAt=0;
    std::string subject;
};
class AuctionMailCapture {
public:
    AuctionMailCapture() : previous(current) { current=this; if(previous) invalid=true; }
    ~AuctionMailCapture() { current=previous; }
    AuctionMailCapture(const AuctionMailCapture&)=delete;
    AuctionMailCapture& operator=(const AuctionMailCapture&)=delete;
    static void Observe(AuctionMail mail) {
        if(!current) return;
        if(current->rows.size()>=4 || mail.subject.size()>120) {current->invalid=true;return;}
        current->rows.push_back(std::move(mail));
    }
    bool Valid() const {return !invalid;}
    const std::vector<AuctionMail>& Rows() const {return rows;}
private:
    inline static thread_local AuctionMailCapture* current=nullptr;
    AuctionMailCapture* previous;
    bool invalid=false;
    std::vector<AuctionMail> rows;
};
}

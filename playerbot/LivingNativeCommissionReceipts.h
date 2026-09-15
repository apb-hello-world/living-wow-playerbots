#pragma once
#include "LivingCommissionMail.h"
class Player;
struct Mail;
class Item;
namespace LivingActivity {
// Called only by successful native mailbox handlers, while their native save
// transaction is open. Ordinary mail takes only a cheap subject-prefix check.
CommissionMailObservation ObserveCommissionCollectionBefore(Player&,const Mail&,const Item&);
void RecordCommissionCollection(Player&,CommissionMailObservation,const NormalMailCapture&);
void RecordCommissionFeeCollection(Player&,const Mail&,uint32_t moneyBefore,uint32_t amount);
void RecordCommissionReturn(const AuctionMail&);
}

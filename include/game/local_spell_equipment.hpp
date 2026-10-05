#pragma once
#include "game/local_gameplay.hpp"
#include "game/local_auction_catalog.hpp"
namespace wowee::game {
inline bool localSpellEquipmentReady(const LocalRealmPlayer& p,const LocalWorldContent& c,const LocalSpellDefinition& spell) {
    const auto fits=[&](size_t slot,bool weaponOnly) {
        const auto id=p.equipment[slot];const auto* item=c.item(id);const auto* meta=localAuctionMetadata(id);
        if(!item||!meta||(weaponOnly&&meta->itemClass!=2))return false;
        uint64_t copies=0;for(const auto& s:p.inventory)if(s.itemId==id)copies+=s.count;
        const auto needed=uint64_t(std::count(p.equipment.begin(),p.equipment.begin()+slot+1,id));
        if(needed>copies)return false;
        size_t current=0;
        for(const auto& stack:p.inventory){
            if(stack.itemId==id){
                if(current+stack.count>=needed){
                    if(stack.instance.maxDurability>0&&stack.instance.curDurability==0)return false;
                    break;
                }
                current+=stack.count;
            }
        }
        if(spell.requiredItemClass<0)return true;
        return meta->itemClass==spell.requiredItemClass &&
            (!spell.requiredItemSubclasses||(meta->subClass<32&&(spell.requiredItemSubclasses&(1u<<meta->subClass)))) &&
            (!spell.requiredInventoryTypes||(item->inventoryType<32&&(spell.requiredInventoryTypes&(1u<<item->inventoryType))));
    };
    if(spell.requiresMainHand&&!fits(localEquipmentIndex(LocalEquipmentSlot::MainHand),true))return false;
    if(spell.requiresOffHand&&!fits(localEquipmentIndex(LocalEquipmentSlot::OffHand),true))return false;
    if(spell.requiredItemClass<0)return true;
    if(spell.requiredItemClass!=2&&spell.requiredItemClass!=4)return false;
    for(size_t i=0;i<p.equipment.size();++i) {
        const bool weaponSlot=i>=localEquipmentIndex(LocalEquipmentSlot::MainHand)&&i<=localEquipmentIndex(LocalEquipmentSlot::Ranged);
        if(spell.requiredItemClass==2&&!weaponSlot)continue;
        if(spell.requiredItemClass==4&&(i==localEquipmentIndex(LocalEquipmentSlot::MainHand)||i==localEquipmentIndex(LocalEquipmentSlot::Tabard)))continue;
        if(fits(i,false))return true;
    }
    return false;
}
}

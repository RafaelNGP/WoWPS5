#include "game/local_melee.hpp"
#include "game/local_cooldowns.hpp"
#include "game/local_spell_amount.hpp"
#include "game/local_feral_talents.hpp"
#include "game/local_spell_import.hpp"
#include "game/local_talents.hpp"
#include "game/local_warrior_talents_import.hpp"
#include <cassert>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <source_location>
using namespace wowee;
using namespace wowee::game;
static void close(double actual,double expected,std::source_location location=std::source_location::current()){
    if(!std::isfinite(actual)||std::abs(actual-expected)>.0001){
        std::cerr<<"line "<<location.line()<<": actual="<<actual<<" expected="<<expected<<'\n';std::abort();
    }
}
static void check(bool ok,std::source_location location=std::source_location::current()){
    if(!ok){std::cerr<<"check failed at line "<<location.line()<<'\n';std::abort();}
}
int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"Usage: local_warrior_talents_test DBC_DIRECTORY\n";return 2;}
    std::map<std::string,pipeline::DBCFile> tables;
    std::map<std::string,std::vector<uint8_t>> bytes;
    for(const auto* name:{"Spell","SpellRange","SpellCastTimes","SpellDuration","SpellIcon","SkillLineAbility",
                          "SkillLine","Talent","TalentTab","SpellRuneCost","SpellRadius"}){
        std::ifstream file(std::filesystem::path(argv[1])/(std::string(name)+".dbc"),std::ios::binary);
        bytes[name]={std::istreambuf_iterator<char>(file),{}};check(tables[name].load(bytes[name]));
    }
    detail::ClientSpellTables t;t.spells=&tables["Spell"];t.ranges=&tables["SpellRange"];
    t.casts=&tables["SpellCastTimes"];t.durations=&tables["SpellDuration"];
    detail::ClientSpellTables::buildIndex(t.spells,t.spellIndex);
    detail::ClientSpellTables::buildIndex(t.ranges,t.rangeIndex);
    detail::ClientSpellTables::buildIndex(t.casts,t.castIndex);
    detail::ClientSpellTables::buildIndex(t.durations,t.durationIndex);

    // 1. Every pinned rank decodes to its exact amounts; a flipped bit in any
    //    gameplay column of the record, or a wrong class/rank, rejects it.
    auto decode=[&](const LocalWarriorTalentRecord& r,LocalSpellDefinition& d,uint32_t classes=1,uint8_t rank=0){
        d={};d.id=r.spellId;d.clientSpell=true;d.allowableClasses=classes;
        const auto row=detail::ClientSpellTables::lookup(t.spellIndex,r.spellId);check(row>=0);
        detail::decodeClientSpell(t,uint32_t(row),d);d.talentId=r.talentId;d.talentRank=rank?rank:r.rank;
        return decodeClientWarriorTalent(t,uint32_t(row),d)&&d.unsupportedReason.empty();
    };
    unsigned records=0,mutations=0;
    for(const auto& r:kLocalWarriorTalentRecords){
        LocalSpellDefinition d;check(decode(r,d));check(d.passive);++records;
        const int rank=r.rank;
        switch(r.talentId){
            case 124: case 146: case 1660: check(d.passiveCastModifiers[0].operation==14&&!d.passiveCastModifiers[0].percentage&&
                d.passiveCastModifiers[0].amount==-10*rank);break;
            case 1542: check(d.passiveCastModifiers[0].operation==14&&d.passiveCastModifiers[0].amount==(rank==1?-20:-50));break;
            case 1864: check(d.passiveCastModifiers[0].operation==11&&d.passiveCastModifiers[0].percentage&&
                d.passiveCastModifiers[0].amount==-11*rank);break;
            case 136: check(d.passivePhysicalDamagePct==2*rank&&d.requiredItemClass==2&&d.requiredItemSubclasses==354);break;
            case 702: check(d.passivePhysicalDamagePct==2*rank&&d.requiredItemClass==2&&d.requiredItemSubclasses==41105);break;
            case 125: check(d.passiveWeaponArmorPenetrationPct==3*rank&&d.requiredItemSubclasses==48);break;
            case 138: check(d.passiveDodgePct==rank);break;
            case 130: check(d.passiveParryPct==rank);break;
            case 140: check(d.passiveEquipmentArmorPct==2*rank&&d.passiveMechanicDurationMask[0]==(1u<<11)&&
                d.passiveMechanicDurationPct[0]==-6*rank&&!d.passiveMechanicDurationNotStack[0]);break;
            case 641: check(d.passiveMechanicDurationMask[0]==((1u<<1)|(1u<<12))&&
                d.passiveMechanicDurationPct[0]==(rank==1?-7:rank==2?-14:-20));break;
            case 134: check(d.passiveTargetDodgeReductionPct==rank&&d.passiveMechanicDurationMask[0]==(1u<<3)&&
                d.passiveMechanicDurationPct[0]==-25*rank&&d.passiveMechanicDurationNotStack[0]);break;
            case 1653: check(d.passiveTotalStatPct[2]==3*rank&&d.passiveTotalStatPct[0]==2*rank&&d.passiveExpertise==2*rank);break;
            case 1862: check(d.passiveTotalStatPct[0]==2*rank&&d.passiveTotalStatPct[2]==2*rank&&d.passiveExpertise==2*rank);break;
            case 126: case 131: case 141: case 142: case 158: case 161: case 166: case 662: case 1655:
                check(d.spellFamily==4&&d.passiveCastModifiers[0].active);break;
            default: check(d.warriorProc!=0);break; // proc talents: local_warrior_procs_test checks each one
        }
        LocalSpellDefinition wrong;check(!decode(r,wrong,2));check(!decode(r,wrong,1,6));
        const auto row=detail::ClientSpellTables::lookup(t.spellIndex,r.spellId);
        if(std::getenv("WARRIOR_TALENTS_QUICK"))continue; // development only: skip the slow mutation sweep
        for(uint32_t col=1;col<234;++col){
            if(col>=131&&col<=203)continue;
            auto edited=bytes["Spell"];
            edited[20+size_t(row)*t.spells->getRecordSize()+col*4]^=1;
            check(tables["Spell"].load(edited));check(!decode(r,wrong));++mutations;
        }
        check(tables["Spell"].load(bytes["Spell"]));
    }
    check(records==120);

    // 2. The full importer admits them, and normal learning follows the tree.
    auto table=[&](const char* name){return &tables.at(name);};
    auto imported=importClientStarterSpells(table("Spell"),table("SpellRange"),table("SpellCastTimes"),table("SpellDuration"),
        table("SpellIcon"),table("SkillLineAbility"),table("SkillLine"),table("Talent"),table("SpellRuneCost"),table("SpellRadius"));
    detail::importClientTalents(imported,table("Talent"),table("TalentTab"),table("Spell"),table("SpellRange"),
        table("SpellCastTimes"),table("SpellDuration"),table("SpellIcon"),table("SpellRuneCost"),table("SpellRadius"));
    LocalWorldContent c;c.spells=std::move(imported.spells);
    std::sort(c.spells.begin(),c.spells.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    for(const auto& r:kLocalWarriorTalentRecords){const auto* d=c.spell(r.spellId);check(d&&d->unsupportedReason.empty());}
    LocalRealmPlayer p;p.guid=10;p.classId=1;p.race=1;p.level=80;p.health=p.maxHealth=100;
    std::string error;
    for(unsigned rank=0;rank<3;++rank)check(learnLocalTalent(p,c,124,rank,error));
    for(unsigned rank=0;rank<5;++rank)check(learnLocalTalent(p,c,130,rank,error));
    for(unsigned rank=0;rank<3;++rank)check(learnLocalTalent(p,c,641,rank,error));
    check(localTalentPointsSpent(p)==11);

    // 3. Rage costs (tenths in the DBC, whole rage in the local pool).
    const auto cost=[&](uint32_t spell){const auto* d=c.spell(spell);check(d&&d->unsupportedReason.empty());return localSpellBaseResourceCost(p,c,*d);};
    LocalRealmPlayer plain=p;plain.talents.clear();
    const auto plainCost=[&](uint32_t spell){return localSpellBaseResourceCost(plain,c,*c.spell(spell));};
    check(cost(78)+3==plainCost(78));                // Heroic Strike, Improved Heroic Strike 3
    check(cost(7386)==plainCost(7386));              // Sunder Armor untouched so far
    p.talents.push_back({146,3});p.talents.push_back({1542,2});p.talents.push_back({1660,3});
    std::sort(p.talents.begin(),p.talents.end());
    check(cost(7386)+3+3==plainCost(7386));          // Puncture 3 + Focused Rage 3
    check(cost(5308)+5+3==plainCost(5308));          // Execute: Improved Execute 2 + Focused Rage 3

    // 4. Intensify Rage: Recklessness's cooldown -33%.
    const auto* reck=c.spell(1719);check(reck&&reck->cooldownMs);
    const auto reckBase=localSpellRecoveryDuration(plain,c,*reck,false);
    p.talents.push_back({1864,3});std::sort(p.talents.begin(),p.talents.end());
    check(localSpellRecoveryDuration(p,c,*reck,false)==reckBase*67/100);

    // 5. Melee statistics: dodge, parry, expertise, stats, armor penetration.
    auto item=[&](uint32_t id){const auto* source=localMeleeItem(id);check(source!=nullptr);
        LocalItemDefinition d;d.id=id;d.inventoryType=source->inventoryType;c.items.push_back(d);
        std::sort(c.items.begin(),c.items.end(),[](const auto& a,const auto& b){return a.id<b.id;});return source;};
    LocalItemDefinition chest;chest.id=7930;chest.inventoryType=5;chest.armor=1000;c.items.push_back(chest);
    const auto* sword1h=item(25);const auto* mace1h=item(36);const auto* mace2h=item(911);
    auto wield=[&](LocalRealmPlayer& who,const LocalMeleeItem* weapon){
        who.equipment={};who.equipment[4]=chest.id;who.equipment[15]=weapon->id;
        who.inventory={{chest.id,1},{weapon->id,1}};
    };
    LocalRealmPlayer base;base.guid=11;base.classId=1;base.race=1;base.level=80;base.health=base.maxHealth=100;
    wield(base,sword1h);
    LocalRealmPlayer tal=base;
    const auto before=localMeleeStats(base,c);
    tal.talents={{130,5},{138,5}};
    auto after=localMeleeStats(tal,c);
    close(after.dodge,before.dodge+5);close(after.parry,before.parry+5);
    tal.talents={{1653,3}};after=localMeleeStats(tal,c);
    check(after.attributes[0]==int32_t(float(before.attributes[0])*1.06f));
    check(after.attributes[2]==int32_t(float(before.attributes[2])*1.09f));
    close(after.expertise,before.expertise+6*.25f);
    tal.talents={{134,2}};after=localMeleeStats(tal,c);close(after.targetDodgeReduction,2);
    // Toughness multiplies only BASE item armor: this chest carries some bonus
    // armor (equipment_bonus_armor), which stays outside the multiplier.
    tal.talents={{140,5}};close(localTalentEquipmentArmorMultiplier(tal,c),1.1f);
    {const auto gain=localMeleeArmor(tal,c)-localMeleeArmor(base,c);check(gain>0&&gain<=100);}
    LocalRealmPlayer druid=tal;druid.classId=11;close(localTalentEquipmentArmorMultiplier(druid,c),1.0);
    tal.talents={{125,5}};close(localMeleeStats(tal,c).armorPenetrationPct,before.armorPenetrationPct);
    wield(tal,mace1h);LocalRealmPlayer maceBase=tal;maceBase.talents.clear();
    check(localMeleeStats(tal,c).armorPenetrationPct==localMeleeStats(maceBase,c).armorPenetrationPct+15);

    // 6. Weapon specializations follow the equipped weapon.
    tal.talents={{136,3},{702,5}};
    wield(tal,sword1h);close(localWeaponTalentDamageMultiplier(tal,c),1.10);
    wield(tal,mace2h);close(localWeaponTalentDamageMultiplier(tal,c),1.06);

    // 7. Mechanic durations: summed 232s, strongest 234, never other mechanics.
    tal.talents={{140,5},{641,3}};
    check(localTalentMechanicDurationPct(tal,c,11)==-30);check(localTalentMechanicDurationPct(tal,c,12)==-20);
    check(localTalentMechanicDurationPct(tal,c,1)==-20);check(localTalentMechanicDurationPct(tal,c,7)==0);
    tal.talents={{134,2}};check(localTalentMechanicDurationPct(tal,c,3)==-50);
    tal.dead=true;check(localTalentMechanicDurationPct(tal,c,3)==0);
    LocalRealmPlayer mage=tal;mage.dead=false;mage.classId=8;check(localTalentMechanicDurationPct(mage,c,3)==0);

    // 8. Ability modifiers: Improved Thunder Clap, Overpower, Whirlwind, Cleave,
    //    Charge, Bloodrage, Demoralizing Shout and Booming Voice.
    LocalRealmPlayer w=base;
    const auto* clap=c.spell(47502);check(clap&&clap->unsupportedReason.empty()&&clap->targetDebuffMeleeHastePct==-10&&clap->targetDebuffMeleeHasteSlot==1);
    w.talents={{141,3}};
    const auto* itc=localTalentSpell(c,141,3);check(itc!=nullptr);
    check(localSpellBaseResourceCost(w,c,*clap)+uint32_t(-itc->passiveCastModifiers[0].amount/10)==localSpellBaseResourceCost(base,c,*clap));
    check(localSpellAmountAfterTalents(w,c,*clap,1000,false)==1300);
    check(int32_t(localSpellAmountModifier(w,c,*clap,10.f,12))==20);
    const auto* overpower=c.spell(7384);check(overpower!=nullptr);
    w.talents={{131,2}};check(localTalentCastModifier(w,c,*overpower,7,false)==50);
    const auto* whirlwind=c.spell(1680);check(whirlwind!=nullptr);
    w.talents={{1655,2}};check(localSpellAmountAfterTalents(w,c,*whirlwind,1000,false)==1200);
    const auto* cleave=c.spell(47520);check(cleave!=nullptr);
    w.talents={{166,3}};check(localSpellEffectAmountAfterTalents(w,c,*cleave,222,false)==uint32_t(222.f*2.2f));
    const auto* charge=c.spell(11578);check(charge&&charge->chargeRage==15);
    w.talents={{126,2}};check(localTalentCastModifier(w,c,*charge,8,false)/10==10);
    const auto* bloodrage=c.spell(2687);check(bloodrage&&bloodrage->energizeRage==20);
    w.talents={{142,2}};check(uint32_t(localSpellAmountModifier(w,c,*bloodrage,20.f,3))==30);
    const auto* demo=c.spell(47437);check(demo&&demo->targetDebuffAttackPower==-410);
    w.talents={{161,5}};check(uint32_t(localSpellAmountModifier(w,c,*demo,410.f,8))==574);
    const auto* shout=c.spell(47436);check(shout&&shout->durationMs);
    w.talents={{158,2}};check(localSpellDuration(w,c,*shout)==localSpellDuration(base,c,*shout)*3/2);
    check(localTalentCastModifier(w,c,*demo,6,true)==50);

    std::cout<<"PASS: "<<records<<" reviewed Warrior talent ranks (38 talents) decoded with exact amounts; "<<mutations
             <<" gameplay-column mutation rejections; rage costs, Intensify Rage cooldown, dodge/parry/expertise/stat/"
               "armor/armor-penetration/weapon-damage and mechanic-duration runtime\n";
}

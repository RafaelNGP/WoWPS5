#include "game/local_ward_import.hpp"
#include "game/local_npc_spell_import.hpp"
#include "game/local_pet_spell_import.hpp"
#include "game/local_ignite_import.hpp"
#include "game/local_area_aura_import.hpp"
#pragma once
#include <set>
#include "game/local_forms.hpp"
#include "game/local_combo.hpp"
#include "game/local_gameplay.hpp"
#include "game/local_spell_ranks.hpp"
#include "game/local_spell_columns.hpp"
#include "game/local_pet_import.hpp"
#include "game/local_proc_rules.hpp"
#include "game/local_ranged_import.hpp"
#include "game/local_progression_import.hpp"
#include "game/local_druid_progression_import.hpp"
#include "game/local_feral_progression_import.hpp"
#include "game/local_stormstrike_import.hpp"
#include "game/local_warrior_progression_import.hpp"
#include "game/local_warrior_talents_import.hpp"
#include "game/local_warrior_procs.hpp"
#include "game/local_arcane_import.hpp"
#include "game/local_clearcasting_import.hpp"
#include "game/local_ghost_wolf_import.hpp"
#include "game/local_auction_catalog.hpp"
#include "game/local_mount_models.hpp"
#include "pipeline/dbc_loader.hpp"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace wowee::game {
struct LocalSpellImport {
    std::vector<LocalSpellDefinition> spells;
    struct AuditRow {uint32_t id=0,classes=0;bool talent=false;std::string status;};
    std::vector<AuditRow> audit;
    std::vector<LocalRecipe> recipes;
    std::string diagnostic;
    // How many decoded rows were retired as proc children because another row's
    // aura-42 effect names them. Reported so the count is visible rather than
    // silent; the definitions stay, only the direct cast goes.
    size_t procChildrenRetired=0;
    size_t untrainedChainRanksRetired=0; // ranks of an untrained spell_ranks chain retired to triggeredOnly.
};

// How much of the client's own spell data this console will hold.
//
// Spell.dbc is 49,839 rows of 234 columns and is deliberately released again
// as soon as this import finishes, so what matters is what the import keeps.
// Record counts and decoded string lengths bound retained storage; this does
// not estimate total process memory or claim a measured console budget.
// Modifiers use three fixed 20-byte records per retained definition; the
// talent ID/rank index uses twelve bytes per installed talent rank. The
// 8192-definition installation limit bounds these additions. Import caps are
// applied in ascending spell id, so a truncated import is the same truncated
// import on every console and the content fingerprint still agrees.
//
// They are also generous rather than tight: only abilities this ruleset can
// actually cast are kept, and only recipes that name both reagents and a
// created item, which is a small fraction of either table.
inline constexpr size_t kLocalMaxImportedClassAbilities = 2048;
inline constexpr size_t kLocalMaxImportedRecipes = 2048;

// SkillLineCategory 7 is a class skill line - the lines a class trainer
// teaches from. 9 and 11 are the secondary skills and primary professions.
inline constexpr uint32_t kLocalSkillCategoryClass = 7;

namespace detail {

/// The four tables every decoded spell needs, plus the optional icon table,
/// with a sorted id index for each so a bulk import is a binary search per
/// lookup rather than a scan of the whole table per spell.
struct ClientSpellTables {
    const pipeline::DBCFile* spells = nullptr;
    const pipeline::DBCFile* ranges = nullptr;
    const pipeline::DBCFile* casts = nullptr;
    const pipeline::DBCFile* durations = nullptr;
    const pipeline::DBCFile* icons = nullptr;
    const pipeline::DBCFile* runeCosts = nullptr;
    const pipeline::DBCFile* radii = nullptr;
    // 2.38: SummonProperties.dbc (id, category, faction, type, slot, flags)
    // for the creature caster's SPELL_EFFECT_SUMMON; absent, summons are refused.
    const pipeline::DBCFile* summons = nullptr;
    bool ready = false;
    std::vector<std::pair<uint32_t, uint32_t>> spellIndex, rangeIndex, castIndex, durationIndex, iconIndex, runeCostIndex, radiusIndex, summonIndex;

    static void buildIndex(const pipeline::DBCFile* table,
                           std::vector<std::pair<uint32_t, uint32_t>>& index) {
        if (!table || !table->isLoaded()) return;
        index.reserve(table->getRecordCount());
        for (uint32_t row = 0; row < table->getRecordCount(); ++row)
            index.emplace_back(table->getUInt32(row, 0), row);
        std::sort(index.begin(), index.end());
    }
    static int32_t lookup(const std::vector<std::pair<uint32_t, uint32_t>>& index, uint32_t id) {
        const auto found = std::lower_bound(index.begin(), index.end(), id,
            [](const std::pair<uint32_t, uint32_t>& entry, uint32_t key) { return entry.first < key; });
        return found != index.end() && found->first == id ? int32_t(found->second) : -1;
    }
};

// Reviewed reactive shield profiles only. Lightning Shield's DBC trigger is a
// helper; its actual damage spell is selected by rank in the server script.
// Water Shield uses the DBC child directly. Both use a 3500 ms server proc ICD.
// References and supported scope: the source audit. Other proc families
// remain blocked until their server conditions and child effects are implemented.
struct ReactiveShieldProfile { uint32_t parent,child,next; bool water; };
inline const ReactiveShieldProfile* reactiveShieldProfile(uint32_t id) {
    static constexpr ReactiveShieldProfile profiles[]={
        {324,26364,325,false},{325,26365,905,false},{905,26366,945,false},{945,26367,8134,false},
        {8134,26369,10431,false},{10431,26370,10432,false},{10432,26363,25469,false},{25469,26371,25472,false},
        {25472,26372,49280,false},{49280,49278,49281,false},{49281,49279,0,false},
        {52127,52128,52129,true},{52129,52130,52131,true},{52131,52132,52134,true},{52134,52133,52136,true},
        {52136,52135,52138,true},{52138,52137,24398,true},{24398,23575,33736,true},{33736,33737,57960,true},{57960,57961,0,true}
    };
    for(const auto& candidate:profiles)if(candidate.parent==id)return &candidate;
    return nullptr;
}
inline bool decodeClientReactiveShield(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto* profile=reactiveShieldProfile(u(0));
    if(!profile)return false;
    if(u(71)!=6 || u(95)!=42 || u(86)!=1 || u(89) || u(73) ||
       u(116)!=(profile->water?profile->child:26545) || u(34)!=0x222a8 ||
       !u(35) || u(35)>100 || !u(36) || u(36)>255 || u(49)>1 || u(32) || u(4)&64u)return false;
    if(profile->water) {
        if(u(72)!=6 || u(96)!=85 || u(87)!=1 || u(90) || u(117) || u(111) ||
           u(75)>1 || t.spells->getFloat(row,78)!=0)return false;
    } else if(u(72))return false;
    const auto child=ClientSpellTables::lookup(t.spellIndex,profile->child);
    if(child<0)return false;
    const auto c=[&](uint32_t col){return t.spells->getUInt32(child,col);};
    const auto i=[&](uint32_t col){return t.spells->getInt32(child,col);};
    const auto f=[&](uint32_t col){return t.spells->getFloat(child,col);};
    // A leaf is not admitted as a trainer spell and cannot recursively trigger.
    if(c(71)!=(profile->water?30u:2u) || c(72) || c(73) || c(86)!=(profile->water?1u:6u) ||
       c(89) || c(95) || c(98) || c(110) || c(116) || c(34) || c(36) || c(40) ||
       c(12) || c(13) || c(42) || c(43) || c(44) || c(45) || c(204) || c(226) ||
       (c(4)&(64u|0x404u)) || (c(5)&0x44u) || i(68)>=0 || c(50) || c(51) ||
       i(80)<0 || i(80)>99999 || c(74)>1 || !std::isfinite(f(77)) || f(77)<0 || f(77)>10000 || f(119)!=0)return false;
    for(uint32_t col=20;col<=27;++col)if(c(col))return false;
    for(uint32_t col=52;col<60;++col)if(i(col)>0)return false;
    const auto cast=ClientSpellTables::lookup(t.castIndex,c(28));
    const auto range=ClientSpellTables::lookup(t.rangeIndex,c(46));
    if(cast<0 || range<0 || t.casts->getInt32(cast,1)!=0 || t.casts->getInt32(cast,2)!=0)return false;
    LocalProcDefinition proc;
    proc.effect=profile->water?LocalProcEffect::RestoreMana:LocalProcEffect::DamageAttacker;
    proc.spellId=profile->child;proc.flags=u(34);proc.chance=uint8_t(u(35));proc.charges=uint8_t(u(36));
    proc.allowTriggered=true;proc.attributesMask=LocalProcTriggeredCanProc;proc.spellTypeMask=1;
    proc.cooldownMs=3500;proc.amount=uint32_t(i(80)+1);proc.amountPerLevel=f(77);
    proc.baseLevel=c(39);proc.maxLevel=c(37);proc.schoolMask=c(225);
    proc.spellFamily=c(spell335::SpellFamily);
    for(unsigned k=0;k<3;++k)proc.spellFamilyFlags[k]=c(spell335::SpellFamilyFlags+k);
    proc.range=t.ranges->getFloat(range,profile->water?4:3);
    if(!std::isfinite(proc.range) || proc.range<0 || proc.range>100)return false;
    if(d.supercededBySpell && d.supercededBySpell!=profile->next)return false;
    d.supercededBySpell=profile->next;d.proc=proc;
    return true;
}

// Complete trainable Molten Armor profiles. Every source gameplay column in
// the parent and triggered child must match; aura 197 and 220 are not dropped.
inline bool matchesClientTalentSource(const ClientSpellTables&,uint32_t,
    std::initializer_list<std::pair<uint32_t,uint32_t>>);
inline bool decodeClientMoltenArmor(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    uint32_t child=0,level=0,damage=0,next=0;
    switch(d.id){case 30482:child=34913;level=62;damage=75;next=43045;break;
        case 43045:child=43043;level=71;damage=130;next=43046;break;
        case 43046:child=43044;level=79;damage=170;break;default:return false;}
    if(d.allowableClasses!=128 || (d.supercededBySpell&&d.supercededBySpell!=next))return false;
    if(!matchesClientTalentSource(t,row,{{4,65536u},{8,524288u},{11,268435456u},{28,1u},{31,8u},
        {34,139944u},{35,100u},{38,level},{39,level},{40,30u},{46,1u},{68,4294967295u},
        {71,6u},{72,6u},{73,6u},{74,1u},{75,1u},{76,1u},{80,4294967295u},{81,4294967290u},
        {82,34u},{86,1u},{87,1u},{88,1u},{95,42u},{96,197u},{97,220u},{112,1792u},{115,4u},
        {116,child},{204,28u},{205,133u},{206,1500u},{208,3u},{209,262144u},{213,1u},{214,1u},
        {216,1065353216u},{217,1065353216u},{218,1065353216u},{225,4u},{232,d.id==43046?30u:0u}}))return false;
    const auto childRow=ClientSpellTables::lookup(t.spellIndex,child);
    if(childRow<0 || !matchesClientTalentSource(t,uint32_t(childRow),{{4,150994944u},{5,168u},{6,16388u},
        {8,16512u},{28,1u},{35,101u},{38,level},{39,level},{46,6u},{68,4294967295u},{71,2u},
        {74,1u},{80,damage-1},{86,6u},{208,3u},{210,8u},{213,1u},{216,1065353216u},
        {217,1065353216u},{218,1065353216u},{225,4u}}))return false;
    const auto duration=ClientSpellTables::lookup(t.durationIndex,30),cast=ClientSpellTables::lookup(t.castIndex,1);
    const auto self=ClientSpellTables::lookup(t.rangeIndex,1),range=ClientSpellTables::lookup(t.rangeIndex,6);
    if(duration<0||cast<0||self<0||range<0||t.durations->getInt32(duration,1)!=1800000||
       t.durations->getInt32(duration,2)||t.durations->getInt32(duration,3)!=1800000||
       t.casts->getInt32(cast,1)||t.casts->getInt32(cast,2)||t.casts->getInt32(cast,3)||
       t.ranges->getFloat(self,1)!=0||t.ranges->getFloat(self,2)!=0||
       t.ranges->getFloat(self,3)!=0||t.ranges->getFloat(self,4)!=0||t.ranges->getUInt32(self,5)||
       t.ranges->getFloat(range,1)!=0||t.ranges->getFloat(range,2)!=0||
       t.ranges->getFloat(range,4)!=100||t.ranges->getUInt32(range,5))return false;
    const float childRange=t.ranges->getFloat(range,3);
    if(childRange!=100)return false;
    d.spiritCritRatingPct=35;d.incomingCritReductionPct=5;d.mageArmorGroup=true;
    d.supercededBySpell=next;
    LocalProcDefinition p;p.effect=LocalProcEffect::DamageAttacker;p.spellId=child;
    // mage.cpp CheckProc applies the Molten Shields gate at event selection.
    p.flags=139944;p.chance=100;p.amount=damage;p.baseLevel=level;p.schoolMask=4;p.spellFamily=3;
    p.spellFamilyFlags[1]=8;p.range=childRange;p.hitMask=LocalProcHitNormal|LocalProcHitCritical|LocalProcHitAbsorb;
    p.spellTypeMask=1;p.allowTriggered=true;p.attributesMask=LocalProcTriggeredCanProc;d.procCanCrit=true;d.proc=p;
    return true;
}

// Earth Shield's scripted heal uses effect zero of the PARENT as its amount.
// Child 379 has zero base healing; treating it as an ordinary leaf loses healing.
// The second effect is mandatory school-filtered pushback protection, not ignored.
inline bool decodeClientEarthShield(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    constexpr uint32_t ranks[]={974,32593,32594,49283,49284};
    size_t rank=0;while(rank<std::size(ranks)&&ranks[rank]!=d.id)++rank;
    if(rank==std::size(ranks))return false;
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    const uint32_t next=rank+1<std::size(ranks)?ranks[rank+1]:0;
    if(d.allowableClasses!=64 || u(71)!=6 || u(72)!=6 || u(73) || u(95)!=4 || u(96)!=149 ||
       u(86)!=21 || u(87)!=21 || u(89) || u(90) || u(34)!=0x2a2a8 || u(35)!=100 || u(36)!=6 ||
       u(32)!=0x80000 || u(33) || u(49)>1 || u(110) || u(111)!=126 || u(113) || u(114) ||
       u(74)!=1 || u(75)!=1 || f(77)!=0 || f(78)!=0 || i(80)<0 || i(80)>99999 || i(81)!=29 ||
       u(92) || u(93) || u(98) || u(99) || u(104) || u(105) || u(116) || u(117) ||
       f(119)!=0 || f(120)!=0 || u(208)!=11 || u(209) || u(210)!=1024 || u(211) ||
       (u(4)&64) || u(9)!=32 || (d.supercededBySpell&&d.supercededBySpell!=next))return false;
    for(unsigned k=122;k<131;++k)if(u(k))return false;
    const auto child=ClientSpellTables::lookup(t.spellIndex,379);if(child<0)return false;
    const auto c=[&](uint32_t col){return t.spells->getUInt32(child,col);};
    // Legacy child proc flags describe event eligibility, not an attached aura.
    // No other child effects, costs, scaling or recursive triggers are admitted.
    if(c(71)!=10 || c(72) || c(73) || c(74)!=1 || t.spells->getInt32(child,80)!=-1 ||
       t.spells->getFloat(child,77)!=0 || c(86)!=1 || c(89) || c(95) || c(98) || c(110) || c(116) ||
       c(34)!=40 || c(36) || c(40) || c(42) || c(43) || c(44) || c(45) || c(204) || c(226) ||
       c(208)!=11 || c(209) || c(210) || c(211)!=64 || c(225)!=8 || c(46)!=1 || c(28)!=1)return false;
    for(unsigned k=12;k<28;++k)if(c(k))return false;
    for(unsigned k=50;k<68;++k)if(c(k))return false;
    const auto childCast=ClientSpellTables::lookup(t.castIndex,c(28));
    const auto childRange=ClientSpellTables::lookup(t.rangeIndex,c(46));
    if(childCast<0 || childRange<0 || t.casts->getInt32(childCast,1) || t.casts->getInt32(childCast,2) ||
       t.ranges->getFloat(childRange,2)!=0 || t.ranges->getFloat(childRange,4)!=0)return false;
    LocalProcDefinition proc;proc.effect=LocalProcEffect::HealOwner;proc.spellId=379;
    proc.flags=u(34);proc.chance=100;proc.charges=6;proc.cooldownMs=3500;
    proc.amount=uint32_t(i(80)+1);proc.baseLevel=u(39);proc.maxLevel=u(37);proc.schoolMask=c(225);
    proc.spellFamily=c(208);for(unsigned k=0;k<3;++k)proc.spellFamilyFlags[k]=c(209+k);
    proc.allowTriggered=true;proc.attributesMask=LocalProcTriggeredCanProc;proc.spellTypeMask=1;proc.pushbackPercent=30;
    d.proc=proc;d.supercededBySpell=next;return true;
}

inline bool matchesClientTalentSource(const ClientSpellTables&,uint32_t,
        std::initializer_list<std::pair<uint32_t,uint32_t>>);

// WotLK 23881 is a single-rank talent, despite a stale SkillLineAbility
// supersession pointer. Its dummy applies 23885 before target hit processing;
// the heal script replaces 23880's legacy flat amount with 1% current max health.
// Verify all three source records before admitting either parent or internal aura.
inline bool decodeClientBloodthirst(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d,
                                   LocalSpellDefinition* internalAura=nullptr) {
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    if(d.id!=23881 || d.allowableClasses!=1 || u(71)!=2 || u(72)!=3 || u(73) ||
       u(74)!=1 || u(75)!=1 || t.spells->getInt32(row,80)!=49 || u(81) ||
       u(86)!=6 || u(87) || u(89) || u(90) || u(95) || u(96) ||
       u(4)!=327696 || u(5)!=134218240 || u(41)!=1 || u(42)!=200 ||
       u(1)!=971 || u(29) || u(30)!=4000 || u(206)!=1500 || u(28)!=1 ||
       u(46)!=2 || u(68)!=2 || u(69)!=173555 || u(70) || u(208)!=4 ||
       u(209) || u(210)!=1024 || u(211) || u(213)!=2 || u(214)!=2 || u(225)!=1 ||
       (d.supercededBySpell && d.supercededBySpell!=23892))return false;
    for(unsigned k=6;k<19;++k)if(u(k))return false;
    for(unsigned k=31;k<=34;++k)if(u(k))return false;
    if(u(36)||u(40)||u(49))return false;
    for(unsigned k=77;k<80;++k)if(u(k))return false;
    for(unsigned k=92;k<131;++k)if(u(k))return false;
    const auto auraRow=ClientSpellTables::lookup(t.spellIndex,23885);
    const auto healRow=ClientSpellTables::lookup(t.spellIndex,23880);
    if(auraRow<0 || healRow<0)return false;
    // Compare every gameplay column too: changes to otherwise unused source
    // coefficients/attributes must not silently reuse this reviewed script.
    if(!matchesClientTalentSource(t,row,{
        {1,971u},{4,327696u},{5,134218240u},{19,1u},{28,1u},
        {30,4000u},{35,101u},{38,40u},{39,40u},{41,1u},
        {42,200u},{46,2u},{68,2u},{69,173555u},{71,2u},
        {72,3u},{74,1u},{75,1u},{80,49u},{86,6u},
        {205,133u},{206,1500u},{208,4u},{210,1024u},{213,2u},
        {214,2u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},
        {229,1065353216u}
    }))return false;
    if(!matchesClientTalentSource(t,uint32_t(auraRow),{
        {4,262160u},{28,1u},{34,20u},{35,100u},{36,3u},
        {38,40u},{39,40u},{40,31u},{41,1u},{46,1u},
        {68,4294967295u},{71,6u},{86,1u},{95,42u},{116,23880u},
        {208,4u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},
        {229,1065353216u},{230,1065353216u}
    }))return false;
    if(!matchesClientTalentSource(t,uint32_t(healRow),{
        {4,262160u},{6,536870912u},{28,1u},{35,101u},{38,40u},
        {39,40u},{41,1u},{46,2u},{68,4294967295u},{71,10u},
        {74,1u},{80,29u},{86,1u},{208,4u},{216,1065353216u},
        {217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}
    }))return false;
    for(const auto child:{auraRow,healRow}) {
        const auto c=[&](uint32_t col){return t.spells->getUInt32(child,col);};
        const bool aura=child==auraRow;
        if(c(4)!=262160 || c(5) || c(6)!=(aura?0u:536870912u) || c(28)!=1 ||
           c(34)!=(aura?20u:0u) || c(35)!=(aura?100u:101u) || c(36)!=(aura?3u:0u) ||
           c(40)!=(aura?31u:0u) || c(41)!=1 || c(46)!=(aura?1u:2u) || c(49) ||
           t.spells->getInt32(child,68)!=-1 || c(69) || c(70) || c(71)!=(aura?6u:10u) ||
           c(72) || c(73) || c(74)!=(aura?0u:1u) || c(75) || c(76) ||
           c(80)!=(aura?0u:29u) || c(81) || c(82) || c(86)!=1 || c(87) || c(88) ||
           c(95)!=(aura?42u:0u) || c(116)!=(aura?23880u:0u) ||
           c(208)!=4 || c(209) || c(210) || c(211) || c(213) || c(214) || c(225)!=1 || c(226))return false;
        for(unsigned k=7;k<28;++k)if(c(k))return false;
        for(unsigned k=29;k<34;++k)if(c(k))return false;
        for(unsigned k=42;k<46;++k)if(c(k))return false;
        for(unsigned k=50;k<68;++k)if(c(k))return false;
        for(unsigned k=77;k<80;++k)if(c(k))return false;
        for(unsigned k=83;k<86;++k)if(c(k))return false;
        for(unsigned k=89;k<116;++k)if(k!=95 && c(k))return false;
        for(unsigned k=117;k<131;++k)if(c(k))return false;
        if(c(204)||c(205)||c(206)||c(207))return false;
    }
    const auto duration=ClientSpellTables::lookup(t.durationIndex,31);
    const auto cast=ClientSpellTables::lookup(t.castIndex,1);
    const auto selfRange=ClientSpellTables::lookup(t.rangeIndex,1);
    const auto meleeRange=ClientSpellTables::lookup(t.rangeIndex,2);
    if(duration<0||cast<0||selfRange<0||meleeRange<0||
       t.durations->getInt32(duration,1)!=8000||t.durations->getInt32(duration,2)||
       t.durations->getInt32(duration,3)!=8000||t.casts->getInt32(cast,1)||t.casts->getInt32(cast,2)||
       t.ranges->getFloat(selfRange,2)!=0||t.ranges->getFloat(selfRange,4)!=0||
       t.ranges->getFloat(meleeRange,1)!=0||t.ranges->getFloat(meleeRange,3)!=5)return false;
    d.meleeSpecialProfile=1;d.triggeredAuraSpellId=23885;d.supercededBySpell=0;
    if(internalAura) {
        LocalSpellDefinition aura;aura.id=23885;aura.allowableClasses=1;aura.clientSpell=true;
        aura.triggeredOnly=true;aura.durationMs=8000;aura.buffSelfOnly=true;aura.range=0;
        aura.resourceType=1;aura.schoolMask=1;aura.spellFamily=4;aura.baseLevel=40;
        aura.name=t.spells->getString(auraRow,136);aura.iconId=t.spells->getUInt32(auraRow,133);
        aura.proc.effect=LocalProcEffect::HealOwnerPctMaxHealth;aura.proc.spellId=23880;
        aura.proc.flags=20;aura.proc.chance=100;aura.proc.charges=3;aura.proc.amount=1;
        aura.proc.schoolMask=1;aura.proc.spellFamily=4;aura.proc.spellTypeMask=7;
        aura.proc.hitMask=LocalProcHitNormal|LocalProcHitCritical|LocalProcHitAbsorb;
        if(!validLocalProc(aura))return false;
        *internalAura=std::move(aura);
    }
    return true;
}

// Aura 15 is a melee damage shield, not a charged/chance-based proc. Aura 97
// absorbs damage by spending mana. Restrict admission to reviewed class ranks;
// a lookalike NPC spell or an unimplemented secondary effect stays blocked.
enum class SimpleShieldKind : uint8_t { None, Thorns, Mana };
struct SimpleShieldProfile { uint32_t id,next; SimpleShieldKind kind; };
inline SimpleShieldKind decodeClientSimpleShield(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    constexpr SimpleShieldProfile profiles[]={
        {467,782,SimpleShieldKind::Thorns},{782,1075,SimpleShieldKind::Thorns},
        {1075,8914,SimpleShieldKind::Thorns},{8914,9756,SimpleShieldKind::Thorns},
        {9756,9910,SimpleShieldKind::Thorns},{9910,26992,SimpleShieldKind::Thorns},
        {26992,53307,SimpleShieldKind::Thorns},{53307,0,SimpleShieldKind::Thorns},
        {1463,8494,SimpleShieldKind::Mana},{8494,8495,SimpleShieldKind::Mana},
        {8495,10191,SimpleShieldKind::Mana},{10191,10192,SimpleShieldKind::Mana},
        {10192,10193,SimpleShieldKind::Mana},{10193,27131,SimpleShieldKind::Mana},
        {27131,43019,SimpleShieldKind::Mana},{43019,43020,SimpleShieldKind::Mana},{43020,0,SimpleShieldKind::Mana}
    };
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    const SimpleShieldProfile* profile=nullptr;
    for(const auto& candidate:profiles)if(candidate.id==u(0)){profile=&candidate;break;}
    if(!profile)return SimpleShieldKind::None;
    const bool mana=profile->kind==SimpleShieldKind::Mana;
    if(d.allowableClasses!=(mana?128u:1024u) || u(71)!=6 || u(72) || u(73) ||
       u(95)!=(mana?97u:15u) || u(86)!=(mana?1u:21u) || u(89) || u(98) || u(116) ||
       u(34)!=(mana?0x222a8u:0u) || u(36) || u(49)>1 || u(32) || u(33) ||
       (u(4)&64u) || u(110)!=(mana?127u:0u) || u(113) || u(74)>1 ||
       i(80)<0 || i(80)>99999 || f(77)!=0 || f(119)!=0 ||
       (d.supercededBySpell && d.supercededBySpell!=profile->next))return SimpleShieldKind::None;
    if(mana) {
        const float multiplier=f(spell335::EffectValueMultiplier);
        if(!std::isfinite(multiplier) || multiplier<1 || multiplier>10)return SimpleShieldKind::None;
        const auto milli=uint32_t(multiplier*1000);
        if(float(milli)!=multiplier*1000)return SimpleShieldKind::None;
        d.manaPerAbsorbMilli=milli;
    } else {
        d.proc.effect=LocalProcEffect::MeleeDamageShield;
        d.proc.spellId=d.id;d.proc.amount=uint32_t(i(80)+1);d.proc.schoolMask=u(225);
        d.proc.flags=0x8;d.proc.chance=100;d.proc.range=4;
        d.proc.spellFamily=d.spellFamily;d.proc.spellFamilyFlags=d.spellFamilyFlags;
    }
    d.supercededBySpell=profile->next;
    return profile->kind;
}

// Reviewed single-enemy Frostbolt/Frost Shock profiles. The zero-valued
// Frostbolt healing modifier is retained explicitly; nonzero versions stay
// unsupported. Additional effects and altered proc/target conditions reject
// the complete spell instead of discarding the extra mechanics.
inline bool decodeClientNpcSnare(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    constexpr uint32_t bolt[]={116,205,837,7322,8406,8407,8408,10179,10180,10181,25304,27071,27072,38697,42841,42842};
    constexpr uint32_t shock[]={8056,8058,10472,10473,25464,49235,49236};
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    bool frostbolt=false;uint32_t next=0;bool found=false;
    for(size_t k=0;k<std::size(bolt);++k)if(d.id==bolt[k]){found=frostbolt=true;next=k+1<std::size(bolt)?bolt[k+1]:0;}
    for(size_t k=0;k<std::size(shock);++k)if(d.id==shock[k]){found=true;next=k+1<std::size(shock)?shock[k+1]:0;}
    if(!found)return false;
    if(u(208)!=(frostbolt?3u:11u)||u(209)!=(frostbolt?32u:0x80000000u)||u(210)||u(211)||
       u(34)||u(36)||u(32)||u(33)||u(49)>1||u(225)!=16||
       u(71)!=6||u(95)!=33||i(80)!=(frostbolt?-41:-51)||u(110)||u(113)||
       u(72)!=2||u(96)||u(73)!=(frostbolt?6u:0u)||
       (d.supercededBySpell&&d.supercededBySpell!=next))return false;
    for(unsigned e=0;e<(frostbolt?3u:2u);++e) {
        if(u(86+e)!=6||u(89+e)||u(92+e)||u(104+e)||u(116+e)||f(119+e)!=0)return false;
        if(e!=1&&(u(74+e)>1||f(77+e)!=0||u(98+e)))return false;
    }
    if(frostbolt&&(u(97)!=118||i(82)!=-1||u(112)!=127||u(115)))return false;
    d.snarePercent=frostbolt?40:50;d.snareZeroHealingMarker=frostbolt;d.supercededBySpell=next;
    // SpellMgr.cpp:3406-3462 at the pinned reference: the nonzero snare
    // CalcValue makes Frost Shock binary. Mage family flag 0x20 explicitly
    // excludes Frostbolt, and ALWAYS_HIT excludes either profile. This is
    // attribution of these completely validated profiles, not a universal
    // classifier for unsupported scripted/periodic-trigger spells.
    d.sourceBinary=!frostbolt&&!d.sourceAlwaysHit;
    return true;
}

// Exact player combo profiles. Other abilities with these effects still need review.
inline bool decodeClientComboProfile(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto kind=localComboProfile(d.id);if(kind==LocalComboProfile::None)return false;
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    std::array<uint32_t,3> effects{},auras{},flags{};
    bool rogue=false,finisher=false,behind=false;
    switch(kind){
    case LocalComboProfile::SinisterStrike:rogue=true;effects={121,80,0};flags={8388610,0,0};break;
    case LocalComboProfile::Backstab:rogue=true;behind=true;effects={121,31,80};flags={8388612,0,0};break;
    case LocalComboProfile::Claw:effects={58,80,0};flags={0,0,262144};break;
    case LocalComboProfile::Shred:behind=true;effects={58,80,31};flags={32768,0,0};break;
    case LocalComboProfile::Rake:effects={2,6,80};auras={0,3,0};flags={4096,0,0};break;
    case LocalComboProfile::Eviscerate:rogue=true;finisher=true;effects={2,3,0};flags={8519680,0,0};break;
    case LocalComboProfile::Rip:finisher=true;effects={6,0,0};auras={3,0,0};flags={8388608,0,2097152};break;
    case LocalComboProfile::FerociousBite:finisher=true;effects={2,0,0};flags={8388608,0,0};break;
    default:return false;
    }
    bool valid=u(208)==(rogue?8u:7u)&&u(41)==3&&u(225)==1&&u(12)==(rogue?0u:1u)&&!u(13)&&!u(14)&&!u(15)&&
        (!u(34)||(kind==LocalComboProfile::Claw&&u(34)==20&&u(35)==40))&&!u(36)&&!u(32)&&!u(33)&&!u(212)&&u(28)==1&&
        bool(u(5)&0x100000u)==finisher&&bool(u(6)&0x100000u)==behind&&(u(5)&0x200u);
    for(unsigned e=0;e<3;++e){
        valid=valid&&u(71+e)==effects[e]&&u(95+e)==auras[e]&&u(209+e)==flags[e];
        if(!effects[e])continue;
        valid=valid&&u(86+e)==(effects[e]==3?1u:6u)&&!u(89+e)&&!u(92+e)&&!u(104+e)&&!u(116+e)&&!u(110+e)&&!u(113+e);
        if(effects[e]==80)valid=valid&&u(80+e)==0&&u(74+e)==1&&f(77+e)==0&&f(119+e)==0;
        if(effects[e]==3)valid=valid&&u(80+e)==0&&!u(74+e)&&f(77+e)==0&&f(119+e)==0;
    }
    if(kind==LocalComboProfile::FerociousBite)valid=valid&&u(131)==6587&&std::isfinite(f(216))&&f(216)>=0&&f(216)<=100;
    if(!valid){if(d.unsupportedReason.empty())d.unsupportedReason="Unreviewed combo attack profile";return false;}
    d.comboProfile=uint8_t(kind);d.comboFinisher=finisher;d.requiresBehind=behind;
    d.allowableClasses=1u<<(rogue?3:10);
    if(kind==LocalComboProfile::FerociousBite)d.extraEnergyMultiplier=f(216);
    return true;
}

/// P06 . The reference's per-form grant table is the switch in
/// AuraEffect::HandleShapeshiftBoosts (SpellAuraEffects.cpp:1356-1425), which
/// casts these on the player when the form is applied and removes them when it
/// is lost. previously every one of their amounts lived here as a constant in
/// kLocalForms; five of them are now imported.
///
/// The five admitted are exactly the boost spells carrying an amount kLocalForms
/// did not already reproduce, plus 21178, which serves both bear forms and owns
/// the +45% threat both bear rows hard-code. The shape is pinned per slot; the
/// AMOUNTS are read, not pinned, so a different client's numbers flow through.
/// The three amounts kLocalForms already carries - aura 142 armour, aura 10
/// threat, aura 87 damage taken - are checked AGAINST the table rather than
/// stored twice, which turns that transcription into a measured fact.
inline bool decodeClientFormBoost(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto* boost=localFormBoost(d.id);
    if(!boost)return false;
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    const auto reject=[&]{d.unsupportedReason="Unreviewed form boost effect profile";return true;};
    // Every boost is a passive self-aura: no cost, no cooldown, no duration, no
    // proc, no reagent, no equipment requirement, no chain and no period.
    if(!(u(4)&0x40u)||u(42)||u(204)||u(226)||u(29)||u(30)||u(40)||u(spell335::ProcFlags)||
       u(spell335::ProcCharges)||u(spell335::InterruptFlags)||u(spell335::AuraInterruptFlags)||
       u(50)||u(51)||i(68)>=0||u(3))return reject();
    for(uint32_t k=0;k<8;++k)if(i(52+k)>0)return reject();
    // Its class is the class of every form the switch casts it for, and every
    // one of those forms must be a form this build models.
    uint32_t classes=0;
    for(uint8_t form=1;form<=32;++form) {
        if(!(boost->forms&(uint64_t(1)<<(form-1))))continue;
        const auto* profile=localFormProfileByForm(form);
        if(!profile)return reject();
        classes|=1u<<(profile->clazz-1);
    }
    if(!classes||(classes&(classes-1)))return reject(); // exactly one class
    unsigned slots=0;
    for(uint32_t k=0;k<3;++k) {
        if(!u(71+k))continue;
        ++slots;
        // Self-applied aura, no secondary target, no trigger, no period, and
        // DieSides in {0,1} so SpellInfo.cpp:432-445 never rolls.
        // Column 98+k is EffectAmplitude (no period), 92+k EffectRadiusIndex,
        // 104+k EffectChainTarget, 116+k EffectTriggerSpell, 83+k EffectMechanic,
        // and 122+e*3+w the three EffectSpellClassMask words - a boost is a
        // plain self aura and carries none of them.
        if(u(71+k)!=6||u(86+k)!=1||u(89+k)||u(116+k)||u(98+k)||u(74+k)>1||
           u(83+k)||u(92+k)||u(104+k)||!std::isfinite(f(77+k)))return reject();
        for(uint32_t w=0;w<3;++w)if(u(spell335::EffectClassMask+k*3+w))return reject();
        const int32_t amount=i(80+k)+int32_t(u(74+k));
        const float perLevel=f(77+k);
        const int32_t misc=i(110+k);
        const auto matchesEveryForm=[&](auto&& pick,int32_t expected) {
            for(uint8_t form=1;form<=32;++form) {
                if(!(boost->forms&(uint64_t(1)<<(form-1))))continue;
                if(int32_t(pick(*localFormProfileByForm(form)))!=expected)return false;
            }
            return true;
        };
        switch(u(95+k)) {
            case 99: // SPELL_AURA_MOD_ATTACK_POWER, flat and level-scaled.
                if(misc||amount<0||amount>65535||perLevel<0||perLevel>16)return reject();
                d.passiveAttackPower=uint16_t(amount);d.passiveAttackPowerPerLevel=perLevel;break;
            case 137: // SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE, misc 2 is STAT_STAMINA.
                if(misc!=2||amount<0||amount>100||perLevel!=0)return reject();
                d.passiveTotalStatPct[2]=uint8_t(amount);break;
            case 52: // SPELL_AURA_MOD_WEAPON_CRIT_PERCENT, no weapon constraint here.
                if(misc||amount<0||amount>100||perLevel!=0)return reject();
                d.passiveMeleeCritPct=uint8_t(amount);d.requiredItemClass=-1;break;
            case 142: // MOD_BASE_RESISTANCE_PCT misc 1 == kLocalForms armorPercent - 100.
                if(misc!=1||perLevel!=0||amount<0||amount>100000||
                   !matchesEveryForm([](const LocalFormProfile& p){return p.armorPercent;},100+amount))return reject();
                break;
            case 10: // MOD_THREAT misc 127 (all schools) == kLocalForms threatPercent - 100.
                if(misc!=127||perLevel!=0||amount<-100||amount>1000||
                   !matchesEveryForm([](const LocalFormProfile& p){return p.threatPercent;},100+amount))return reject();
                break;
            case 87: // MOD_DAMAGE_PERCENT_TAKEN misc 127 == kLocalForms takenPercent - 100.
                if(misc!=127||perLevel!=0||amount<-100||amount>1000||
                   !matchesEveryForm([](const LocalFormProfile& p){return p.takenPercent;},100+amount))return reject();
                break;
            case 49: // MOD_DODGE_PERCENT with an amount of exactly zero: a no-op anchor.
                if(misc||perLevel!=0||amount!=0)return reject();
                break;
            default: return reject();
        }
    }
    if(!slots)return reject();
    // HandleShapeshiftBoosts CASTS these; a player never learns one, and a class
    // trainer must not offer "Bear Form (Passive)" for sale. triggeredOnly is
    // the flag that already means exactly that: it keeps the spell out of the
    // trainer set (local_gameplay.cpp:2087), out of the auto-learned set
    // (:2507-2512), out of a reloaded spellbook (:2471) and refuses a direct
    // cast (:3406) - the same treatment Ghost Wolf's 67116 gets by being
    // refused outright. Stance Mastery is NOT given this: that one really is a
    // warrior trainer spell.
    d.passive=true;d.triggeredOnly=true;d.buffSelfOnly=true;d.allowableClasses=classes;
    d.requiredForms=boost->forms;d.excludedForms=0;
    d.unsupportedReason.clear();
    return true;
}
/// P06 . The three passives the reference reads when a form is entered
/// to give resource back: Furor (SpellAuraEffects.cpp:2100-2132) and Stance
/// Mastery / Tactical Mastery (:2193-2221). Nine rank spells, one rejection
/// reason before this: every one carries its amount on a SPELL_AURA_DUMMY
/// effect, which the generic loop has no rule for.
///
/// Only the dummy amount is carried. Furor's second effect is aura 107 with
/// SPELLMOD_EFFECT2 (operation 12), and Tactical Mastery's are two aura 108
/// SPELLMOD_THREAT increases (operation 2). Both are validated here so an
/// unexpected profile still rejects, and NEITHER is written into
/// passiveCastModifiers: nothing in this build reads operation 12, and
/// localTalentCastModifier's sign filter (local_talents.hpp:115) admits only
/// reductions for operation 2, so a threat increase would sit in the definition
/// where no consumer could ever reach it. the source audit records both.
inline bool decodeClientFormResourceTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto* entry=localFormResourceTalent(d.id);
    if(!entry)return false;
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    const auto reject=[&]{d.unsupportedReason="Unreviewed form entry-resource profile";return true;};
    const bool furor=entry->kind==LocalFormResourceKind::Furor;
    // Passive, no cost, no cooldown, no proc, no reagent, no equipment or totem
    // requirement, and the family/icon pair the reference itself keys on.
    if(!(u(4)&0x40u)||u(41)||u(42)||u(204)||u(226)||u(29)||u(30)||u(3)||
       u(spell335::ProcFlags)||u(spell335::ProcCharges)||u(spell335::InterruptFlags)||
       u(spell335::AuraInterruptFlags)||u(50)||u(51)||i(68)>=0||u(69)||u(70))return reject();
    for(uint32_t k=0;k<8;++k)if(i(52+k)>0)return reject();
    if(u(spell335::SpellFamily)!=(furor?7u:4u)||u(133)!=(furor?238u:139u))return reject();
    uint32_t dummies=0;
    for(uint32_t k=0;k<3;++k) {
        if(!u(71+k))continue;
        if(u(71+k)!=6||u(89+k)||u(116+k)||u(98+k)||u(74+k)>1||u(83+k)||u(92+k)||u(104+k)||f(77+k)!=0)return reject();
        // A secondary target-none aura inherits the explicit self target the
        // first effect establishes, exactly as decodeClientCastModifierTalent
        // allows; anything else is a target form this profile does not cover.
        if(u(86+k)!=1&&!(k>0&&u(86+k)==0&&u(71)==6&&u(86)==1))return reject();
        const int32_t amount=i(80+k)+int32_t(u(74+k));
        switch(u(95+k)) {
            case 4: // SPELL_AURA_DUMMY: the amount the reference reads.
                if(i(110+k)||amount<1||amount>100||dummies++)return reject();
                if(furor)d.furorChancePct=uint8_t(amount);
                else d.retainedRage=uint8_t(amount);
                break;
            case 107: // SPELLMOD_EFFECT2, flat. Validated, deliberately not carried.
                if(!furor||i(110+k)!=12||amount<1||amount>100)return reject();
                break;
            case 108: // SPELLMOD_THREAT, percentage. Validated, deliberately not carried.
                if(furor||i(110+k)!=2||amount<1||amount>1000)return reject();
                break;
            default: return reject();
        }
    }
    if(!dummies)return reject();
    d.passive=true;d.buffSelfOnly=true;d.unsupportedReason.clear();
    return true;
}

/// The effect loop of decodeClientSpell for a creature caster (the generated
/// SmartAI family). It mirrors tools/local_realm/generate_npc_spell_profiles.py
/// column for column: shapes, kinds and bounds admitted there are admitted
/// here and nowhere else; anything wider fails closed. Returns the target
/// shape (LocalSpellDefinition::npcTargetShape) and sets npcPositive.
template<class Unavailable>
inline void decodeCreatureEffects(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d,Unavailable&& unavailable) {
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto i=[&](uint32_t col){return t.spells->getInt32(row,col);};
    const auto f=[&](uint32_t col){return t.spells->getFloat(row,col);};
    enum Shape:uint8_t{Enemy=0,Self=1,Aoe=2,AoeTarget=3,Cone=4,AoeAlly=5,AoeDest=6,NoShape=255};
    const auto shapeOf=[&](uint32_t a,uint32_t b)->uint8_t{
        if(b==0){switch(a){case 6:case 25:return Enemy;case 1:case 21:case 20:case 33:return Self;case 24:case 54:case 104:return Cone;case 16:return AoeDest;default:break;}}
        if(a==22&&b==15)return Aoe;if(a==18&&b==16)return Aoe;if(a==53&&b==16)return AoeTarget;if(a==63&&b==16)return AoeDest;
        if(a==22&&b==30)return AoeAlly;if(a==18&&b==31)return AoeAlly;
        return NoShape;
    };
    // 2.38: the destination of a destination-only effect (a summon, a
    // persistent area aura), mirroring the generator's spell_dest: the
    // caster, the summon spot, a direction at the radius, a random point, or
    // the explicit target's position (LocalNpcDest).
    const auto destOf=[&](uint32_t a,uint32_t b)->uint8_t{
        const auto byA=[](uint32_t x)->uint8_t{switch(x){case 18:return kLocalNpcDestCaster;case 32:return kLocalNpcDestSummon;case 47:return kLocalNpcDestFront;
            case 48:return kLocalNpcDestBack;case 49:return kLocalNpcDestRight;case 50:return kLocalNpcDestLeft;case 41:return kLocalNpcDestFrontRight;
            case 42:return kLocalNpcDestBackRight;case 43:return kLocalNpcDestBackLeft;case 44:return kLocalNpcDestFrontLeft;case 72:return kLocalNpcDestRandom;
            case 73:return kLocalNpcDestRadius;case 53:case 63:case 16:case 28:return kLocalNpcDestTarget;default:return 0;}};
        if(b==0)return byA(a);
        if(a==22&&byA(b))return byA(b);
        if(a==18&&b==86)return kLocalNpcDestRadius;
        if(a==18&&(b==16||b==28))return kLocalNpcDestCaster;
        if((a==53||a==63||a==16)&&(b==16||b==28))return kLocalNpcDestTarget;
        return 0;
    };
    const auto casterDest=[](uint8_t dest){return dest&&dest!=kLocalNpcDestTarget;};
    // SpellEffectInfo::CalcValue's creature scaling lists.
    const auto effectScales=[&](uint32_t type,uint32_t aura){
        switch(type){case 2:case 3:case 8:case 9:case 10:case 58:case 77:case 121:case 140:case 142:case 143:return true;default:break;}
        if(type==6)switch(aura){case 3:case 4:case 8:case 15:case 43:case 53:case 64:case 69:case 179:return true;default:break;}
        return false;
    };
    // Spell.dbc proc columns: a proc trigger aura owns them (SpellMgr::
    // LoadSpellProcs' generated entry); a control aura spends charges on
    // damage taken; the generator has already refused every other carrier
    // and every spell_proc row this decoder does not read.
    const uint32_t procFlags=u(34),procChance=u(35),procCharges=u(36);
    constexpr uint32_t procTakenFlags=0x8|0x20|0x80|0x200|0x2000|0x20000|0x80000|0x100000;
    constexpr uint32_t procDoneFlags=0x4|0x10|0x40|0x100|0x1000|0x10000|0x40000|0x400000|0x800000;
    constexpr uint32_t procLocalFlags=procTakenFlags|procDoneFlags|0x2|0x1|0x1000000;
    bool procAura=false;
    for(uint32_t k=0;k<3;++k)if(u(71+k)==6&&(u(95+k)==42||u(95+k)==43))procAura=true;
    if(procAura) {
        if(!(procFlags&procLocalFlags)||(procFlags&~procLocalFlags)||procChance>100||procCharges>99){unavailable("Unsupported creature proc definition");return;}
        for(uint32_t c=0;c<9;++c)if(u(122+c)){unavailable("Unsupported creature proc class mask");return;}
        d.npcProcFlags=procFlags;d.npcProcChance=uint8_t(procChance);d.npcProcCharges=uint8_t(procCharges);
    } else if(procFlags||procCharges) {
        bool control=false;
        for(uint32_t k=0;k<3;++k)if(u(71+k)==6)switch(u(95+k)){case 12:case 26:case 7:case 5:case 56:control=true;break;default:break;}
        if(!control||procChance>100||procCharges>99||(procFlags&~procTakenFlags)||!(procFlags&0x100000)){unavailable("Unsupported creature proc definition");return;}
        d.npcProcBreakChance=uint8_t(procChance);d.npcProcBreakCharges=uint8_t(procCharges);
    }
    uint8_t shape=NoShape,cosmeticShape=NoShape;bool anyReal=false;
    uint8_t modSchool=0;
    unsigned directAmountEffects=0;
    d.directEffectSlot=d.periodicEffectSlot=255;
    bool cosmeticAura=false,utilityOnly=true;
    uint8_t kindsSeen[3]={0,0,0};unsigned kindCount=0;
    const auto seen=[&](uint8_t kind){for(unsigned q=0;q<kindCount;++q)if(kindsSeen[q]==kind)return true;if(kindCount<3)kindsSeen[kindCount++]=kind;return false;};
    uint32_t chainKinds[3]={0,0,0};bool realEffect[3]={false,false,false};
    bool unitAuras=false;unsigned cosmeticFree=0;
    for(uint32_t k=0;k<3;++k) {
        auto type=u(71+k);if(!type)continue;
        auto ta=u(86+k),tb=u(89+k);
        // APPLY_AREA_AURA_PARTY: a creature's party is itself (Unit::GetPartyMembers).
        if(type==35){type=6;ta=1;tb=0;}
        const auto aura=(type==6||type==27)?u(95+k):0u;
        const int32_t base=i(80+k),dice=i(74+k),misc=i(110+k);const float perLevel=f(77+k);
        const uint32_t amplitude=u(98+k),chain=u(104+k),trigger=u(116+k);
        uint8_t effectShape=shapeOf(ta,tb);
        // 2.38: a destination-only effect takes its place from the pair; a
        // cosmetic effect at a destination or with no target imposes no shape.
        uint8_t dest=0;
        if(type==28||type==27) {
            dest=destOf(ta,tb);
            if(!dest){unavailable("Unsupported creature destination");return;}
            effectShape=casterDest(dest)?Aoe:AoeDest;
        } else if((type==3||type==46||type==114)&&effectShape==NoShape&&((ta==0&&tb==0)||destOf(ta,tb))) {
            ++cosmeticFree;chainKinds[k]=chain;continue;
        }
        if(effectShape==NoShape){unavailable("Unsupported creature target shape");return;}
        if(base<-100000||base>100000||dice<0||dice>100000||!std::isfinite(perLevel)||std::abs(perLevel)>10000){unavailable("Invalid creature effect amount");return;}
        if(trigger&&!(type==64||(type==6&&(aura==23||aura==42)))){unavailable("Unsupported creature effect trigger");return;}
        const int32_t amount=base+(dice>=1?1:0);
        const uint32_t low=uint32_t(std::max(0,base+1)),high=low+(dice>1?uint32_t(dice-1):0);
        const int32_t amountHigh=amount+(dice>1?dice-1:0);
        float destRadius=0;
        if(type==28||type==27) {
            // The radius is the placement distance of a directional or random
            // destination, the spread of several summons, the object's size.
            if(u(92+k)) {
                const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+k));
                if(!t.radii||radius<0){unavailable("Area radius record missing");return;}
                const auto rb=t.radii->getFloat(radius,1),rp=t.radii->getFloat(radius,2),rm=t.radii->getFloat(radius,3);
                if(!std::isfinite(rb)||rb<0||rb>100||rp!=0||!std::isfinite(rm)||rm<rb){unavailable("Unsupported creature area radius");return;}
                destRadius=rb;
            }
        } else if(effectShape==Aoe||effectShape==AoeTarget||effectShape==Cone||effectShape==AoeAlly||effectShape==AoeDest) {
            const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+k));
            if(!t.radii||radius<0){unavailable("Area radius record missing");return;}
            const auto rb=t.radii->getFloat(radius,1),rp=t.radii->getFloat(radius,2),rm=t.radii->getFloat(radius,3);
            if(!std::isfinite(rb)||rb<=0||rb>100||rp!=0||!std::isfinite(rm)||rm<rb){unavailable("Unsupported creature area radius");return;}
            if(!d.npcAreaRadius)d.npcAreaRadius=rb;
            if(effectShape==Cone) {
                float degrees=ta==24?24.f:ta==54?54.f:104.f;
                if(const auto* g=localNpcSpellGeometry(d.id);g&&g->coneDegrees)degrees=float(std::abs(g->coneDegrees));
                d.npcConeDegrees=degrees;
            }
        }
        if(chain>1) {
            if(effectShape!=Enemy){unavailable("Unsupported creature chain shape");return;}
            const float multiplier=f(216+k);
            if(!std::isfinite(multiplier)||multiplier<=0||multiplier>10||(d.npcChainTargets&&d.npcChainTargets!=chain)){unavailable("Unsupported creature chain multiplier");return;}
            d.npcChainTargets=uint8_t(std::min<uint32_t>(chain,255));d.npcChainMultiplier=multiplier;
            if(const auto* g=localNpcSpellGeometry(d.id);g&&g->jumpDistance)d.npcJumpDistance=float(g->jumpDistance);
        }
        chainKinds[k]=chain;
        enum Kind:uint8_t{KDamage=1,KWeapon,KWeaponPct,KLeech,KHeal,KInterrupt,KKnockback,KCosmetic,KTrigger,KPeriodicTrigger,KPeriodic,KPeriodicLeech,
            KPeriodicHeal,KSlow,KArmor,KArmorPct,KControl,KDamageTakenFlat,KDamageTakenPct,KHealingPct,KHaste,KDamagePct,KDamageFlat,KAttackPower,
            KSpeed,KResistance,KCastSpeed,KHitChance,KDisarm,KDodge,KParry,KBlock,KAbsorb,KSchoolImmunity,KDamageImmunity,KMechanicImmunity,
            KMaxHealth,KMaxHealthPct,KDamageShield,KProcTrigger,KProcDamage,KHealPct,KHealMax,KEnergize,KEnergizePct,KPowerBurn,KPowerDrain,
            KDispel,KDispelMechanic,KCharge,KThreatPct,KKillCredit,KCreateItem,KExtraAttacks,KSummon,
            // 2.39
            KInstakillSelf,KSelfControl,KInvisible};
        uint8_t kind=0;
        // Who takes the effect: 1 = the creature side (self/ally shapes), 2 =
        // the player side (enemy shapes), 3 = either, 4 = a utility on a player,
        // 5 = nobody (a summon at a destination).
        uint8_t side=0;
        switch(type) {
            case 28:{
                // Spell::EffectSummonType: the entry (MiscValue), the SummonProperties
                // row (MiscValueB), the count from BasePoints for the listed
                // properties, the spell's duration (none: DEAD_DESPAWN).
                if(d.npcSummonEntry||misc<=0){unavailable("Unsupported creature summon");return;}
                const auto props=t.summons?ClientSpellTables::lookup(t.summonIndex,u(113+k)):-1;
                if(props<0){unavailable("Unsupported creature summon properties");return;}
                const uint32_t category=t.summons->getUInt32(props,1),ptype=t.summons->getUInt32(props,3),pflags=t.summons->getUInt32(props,5);
                if(category>2||!(ptype==0||ptype==1||ptype==2||ptype==3||ptype==6||ptype==7||ptype==8)||(pflags&0x10u)){unavailable("Unsupported creature summon properties");return;}
                uint32_t count=1;
                switch(u(113+k)){case 64:case 61:case 1101:case 66:case 648:case 2301:case 1061:case 1261:case 629:case 181:case 715:case 1562:case 833:case 1161:case 713:
                    if(dice>1||perLevel!=0){unavailable("Unsupported creature summon count");return;}
                    count=amount>0?uint32_t(amount):1u;break;default:break;}
                if(count>10||(!d.durationMs&&!d.indefiniteDuration&&u(40))){unavailable("Unsupported creature summon count");return;}
                d.npcSummonEntry=uint32_t(misc);d.npcSummonCount=uint8_t(count);d.npcSummonCategory=uint8_t(category);d.npcSummonType=uint8_t(ptype);
                d.npcSummonOwnerFaction=category!=0||(pflags&0x1000u);d.npcSummonDest=dest;d.npcSummonRadius=destRadius;
                kind=KSummon;side=5;break;}
            case 2:kind=KDamage;side=2;break;
            case 17:case 58:case 121:if(amount<0){unavailable("Unsupported creature weapon effect");return;}kind=KWeapon;side=2;break;
            case 31:if(perLevel!=0||amount<=0||amount>1000||amountHigh>1000){unavailable("Unsupported creature weapon percentage");return;}kind=KWeaponPct;side=2;break;
            case 9:{const float m=f(101+k);if(!std::isfinite(m)||m<0||m>10){unavailable("Unsupported creature leech multiplier");return;}kind=KLeech;side=2;break;}
            case 10:case 75:kind=KHeal;side=1;break;
            case 136:if(dice>1||amount<=0||amount>100){unavailable("Unsupported creature heal percentage");return;}kind=KHealPct;side=1;break;
            case 67:kind=KHealMax;side=1;break;
            case 30:case 137:if(misc!=0||amount<0||(type==137&&(dice>1||amount>100))){unavailable("Unsupported creature energize");return;}kind=type==30?KEnergize:KEnergizePct;side=1;break;
            case 68:if(!d.durationMs){unavailable("Interrupt without a lockout duration");return;}kind=KInterrupt;side=2;break;
            case 98:case 144:if(misc<0||misc>10000||amount<0||amount>10000||(misc<=1&&amount<=1)){unavailable("Unsupported creature knockback");return;}kind=KKnockback;side=2;break;
            case 3:case 46:case 114:kind=KCosmetic;side=3;break;
            case 62:{const float m=f(101+k);if(misc!=0||amount<0||!std::isfinite(m)||m<0||m>10){unavailable("Unsupported creature power burn");return;}kind=KPowerBurn;side=2;break;}
            case 8:{const float m=f(101+k);if(misc!=0||amount<0||!std::isfinite(m)||m<0||m>10){unavailable("Unsupported creature power drain");return;}kind=KPowerDrain;side=2;break;}
            case 38:if(misc<1||misc>4||dice>1||amount<=0||amount>10){unavailable("Unsupported creature dispel");return;}kind=KDispel;side=2;break;
            case 108:if(misc<=0||misc>31||dice>1||amount<=0||amount>10){unavailable("Unsupported creature mechanic dispel");return;}kind=KDispelMechanic;side=2;break;
            case 96:kind=KCharge;side=2;break;
            case 125:if(dice>1||amount<-100||amount>1000){unavailable("Unsupported creature threat modifier");return;}kind=KThreatPct;side=2;break;
            case 134:if(misc<=0){unavailable("Unsupported creature kill credit");return;}kind=KKillCredit;side=4;break;
            // Spell::EffectCreateItem: EffectItemType (column 107 + k; 2.39 fix - the
            // misc value was read before), CalcValue clamped to at least one item.
            case 24:if(int32_t(u(107+k))<=0||dice>1||amount<0||amount>20){unavailable("Unsupported creature item creation");return;}kind=KCreateItem;side=4;break;
            // 2.39 Spell::EffectInstaKill on the caster (Unit::Kill(me, me)).
            case 1:if(effectShape!=Self){unavailable("Unsupported creature instakill shape");return;}kind=KInstakillSelf;side=1;break;
            // 2.39 Spell::EffectScriptEffect: the ids its switch handles do
            // something; every other id only starts its spell_scripts rows,
            // which the generator refuses - nothing happens here.
            case 77:switch(d.id){case 22539:case 22972:case 22975:case 22976:case 22977:case 22978:case 22979:case 22980:case 22981:case 22982:case 22983:case 22984:case 22985:
                    case 31666:case 32307:case 41931:case 52173:case 54640:case 57347:case 57349:case 58418:case 58420:case 58428:case 60243:case 61263:
                        unavailable("Coded script effect");return;
                    default:kind=KCosmetic;side=3;break;}break;
            case 19:if(dice>1||amount<=0||amount>10){unavailable("Unsupported creature extra attacks");return;}kind=KExtraAttacks;side=1;break;
            // 2.39: a trigger on the creature itself casts the triggered spell
            // on itself (a self buff, or a caster-centred area around it).
            case 64:kind=KTrigger;side=effectShape==Self?1:2;break;
            case 27:
                // Spell::EffectPersistentAA: a dynamic object of the radius for
                // the duration; its aura kind (below) lands on the enemies inside.
                if(d.npcGroundAura||destRadius<=0||!d.durationMs||!(tb==0||tb==16||tb==28)){unavailable("Unsupported creature ground aura");return;}
                d.npcGroundAura=true;d.npcGroundDest=dest;d.npcGroundRadius=destRadius;
                [[fallthrough]];
            case 6:switch(aura) {
                case 3:if(!(amplitude>0&&amplitude<=d.durationMs)){unavailable("Unsupported creature periodic shape");return;}kind=KPeriodic;side=2;break;
                case 53:{const float m=f(101+k);if(!(amplitude>0&&amplitude<=d.durationMs)||!std::isfinite(m)||m<0||m>10){unavailable("Unsupported creature leech shape");return;}kind=KPeriodicLeech;side=2;break;}
                case 8:if(!(amplitude>0&&amplitude<=d.durationMs)||amount<=0){unavailable("Unsupported creature periodic heal");return;}kind=KPeriodicHeal;side=1;break;
                case 33:if(dice>1||amount<=-100||amount>=0){unavailable("Unsupported creature slow amount");return;}kind=KSlow;side=3;break;
                case 31:if(dice>1||amount<=0||amount>1000){unavailable("Unsupported creature speed amount");return;}kind=KSpeed;side=1;break;
                case 22:if(misc<=0||misc>127||perLevel>1000||perLevel<-1000||std::abs(amount)>100000||amountHigh>100000||((misc&1)&&misc!=1)){unavailable("Unsupported creature resistance amount");return;}
                    // A zero amount (the creature Sunder Armor 15502: -1 + 1)
                    // applies and stacks without changing anything: cosmetic here.
                    if(amount==0&&amountHigh==0&&perLevel==0){kind=KCosmetic;side=3;break;}
                    kind=misc==1?KArmor:KResistance;side=3;break;
                case 101:if(misc!=1||dice>1||amount<=-100||amount>1000||amount==0){unavailable("Unsupported creature armor percentage");return;}kind=KArmorPct;side=3;break;
                // 2.39: a stun / root the creature applies to itself.
                case 12:case 26:if(effectShape==Self){kind=KSelfControl;side=1;break;}[[fallthrough]];
                case 7:case 5:case 27:kind=KControl;side=2;break;
                // 2.39: an invisible creature (unseen, unaggroed) for the duration.
                case 18:if(effectShape!=Self){unavailable("Unsupported creature invisibility shape");return;}kind=KInvisible;side=1;break;
                case 14:if(misc<=0||misc>127||dice>1||amount==0||std::abs(amount)>100000){unavailable("Unsupported creature damage-taken amount");return;}kind=KDamageTakenFlat;side=3;break;
                case 87:if(misc<=0||misc>127||dice>1||amount<=-100||amount>1000||amount==0){unavailable("Unsupported creature damage-taken percentage");return;}kind=KDamageTakenPct;side=3;break;
                case 118:if(dice>1||amount<-100||amount>=0){unavailable("Unsupported creature healing percentage");return;}kind=KHealingPct;side=3;break;
                case 138:case 193:if(dice>1||amount<-100||amount>1000||amount==0){unavailable("Unsupported creature haste amount");return;}kind=KHaste;side=3;break;
                case 65:if(dice>1||amount<=-100||amount>1000||amount==0){unavailable("Unsupported creature cast speed amount");return;}kind=KCastSpeed;side=3;break;
                case 54:if(dice>1||amount<-100||amount>100||amount==0){unavailable("Unsupported creature hit chance amount");return;}kind=KHitChance;side=3;break;
                case 67:kind=KDisarm;side=2;break;
                case 49:case 47:case 51:if(dice>1||amount<-100||amount>100||amount==0){unavailable("Unsupported creature avoidance amount");return;}kind=aura==49?KDodge:aura==47?KParry:KBlock;side=3;break;
                case 79:if(misc<=0||misc>127||dice>1||amount<-99||amount>1000||amount==0){unavailable("Unsupported creature damage percentage");return;}kind=KDamagePct;side=3;break;
                case 13:if(misc<=0||misc>127||dice>1||amount==0||std::abs(amount)>100000){unavailable("Unsupported creature damage bonus");return;}kind=KDamageFlat;side=3;break;
                case 99:if(dice>1||amount==0||std::abs(amount)>100000){unavailable("Unsupported creature attack power amount");return;}kind=KAttackPower;side=3;break;
                case 69:if(misc<=0||misc>127||amount<=0||amountHigh>1000000){unavailable("Unsupported creature absorb amount");return;}kind=KAbsorb;side=1;break;
                case 39:case 40:if(misc<=0||misc>127){unavailable("Unsupported creature immunity school");return;}kind=aura==39?KSchoolImmunity:KDamageImmunity;side=1;break;
                case 77:if(misc<=0||misc>31){unavailable("Unsupported creature immunity mechanic");return;}kind=KMechanicImmunity;side=1;break;
                case 34:if(dice>1||amount==0||std::abs(amount)>1000000){unavailable("Unsupported creature health amount");return;}kind=KMaxHealth;side=1;break;
                case 133:if(dice>1||amount<-99||amount>1000||amount==0){unavailable("Unsupported creature health percentage");return;}kind=KMaxHealthPct;side=1;break;
                case 15:if(amount<=0||amountHigh>100000||!u(225)){unavailable("Unsupported creature damage shield");return;}kind=KDamageShield;side=1;break;
                case 42:if(!trigger||trigger==d.id||!procFlags){unavailable("Unsupported creature proc trigger");return;}kind=KProcTrigger;side=1;break;
                case 43:if(!procFlags||amount<=0||!u(225)){unavailable("Unsupported creature proc damage");return;}kind=KProcDamage;side=1;break;
                case 23:if(!amplitude){unavailable("Unsupported creature periodic trigger interval");return;}kind=KPeriodicTrigger;side=3;break;
                case 29:case 61:case 4:case 56:case 84:case 85:case 88:case 124:case 36:case 30:case 41:case 11:kind=KCosmetic;side=3;break;
                case 137:if(effectShape!=Self&&effectShape!=AoeAlly){unavailable("Unsupported creature stat percentage shape");return;}kind=KCosmetic;side=3;break;
                default:break;
            }break;
            default:break;
        }
        if(!kind){unavailable("Unsupported creature effect "+std::to_string(type)+((type==6||type==27)?" / aura "+std::to_string(aura):""));return;}
        if(kind!=KCosmetic&&seen(kind)){unavailable("Repeated creature effect");return;}
        if(type==27) {
            // The ground aura lands on players alone (a periodic trigger would
            // need the object as caster).
            if(side==1||kind==KPeriodicTrigger){unavailable("Unsupported creature ground aura kind");return;}
        } else if(type==6&&kind!=KCosmetic)unitAuras=true;
        const bool friendlyShape=effectShape==Self||effectShape==AoeAlly;
        if(side==5){}
        else if(side==1&&!friendlyShape) {
            if(!(ta==25&&tb==0)){unavailable("Friendly creature effect on a hostile shape");return;}
            effectShape=Self;
        }
        if((side==2||side==4)&&friendlyShape){unavailable("Hostile creature effect on a friendly shape");return;}
        const bool needsDuration=kind==KPeriodic||kind==KPeriodicLeech||kind==KPeriodicHeal||kind==KSlow||kind==KSpeed||kind==KArmor||kind==KArmorPct||
            kind==KResistance||kind==KControl||kind==KDamageTakenFlat||kind==KDamageTakenPct||kind==KHealingPct||kind==KHaste||kind==KCastSpeed||
            kind==KHitChance||kind==KDisarm||kind==KDodge||kind==KParry||kind==KBlock||kind==KDamagePct||kind==KDamageFlat||kind==KAttackPower||
            kind==KAbsorb||kind==KSchoolImmunity||kind==KDamageImmunity||kind==KMechanicImmunity||kind==KMaxHealth||kind==KMaxHealthPct||
            kind==KDamageShield||kind==KProcTrigger||kind==KProcDamage||kind==KPeriodicTrigger||kind==KSelfControl||kind==KInvisible;
        // 2.39: a periodic trigger the creature keeps until removed (SpellDuration -1) is permanent.
        const bool permanentTrigger=kind==KPeriodicTrigger&&d.indefiniteDuration&&effectShape==Self;
        if(needsDuration&&!d.durationMs&&!permanentTrigger){unavailable("Unsupported creature aura duration");return;}
        if(kind==KCosmetic){if(cosmeticShape!=NoShape&&cosmeticShape!=effectShape)cosmeticShape=NoShape;else cosmeticShape=effectShape;}
        else {
            if(shape!=NoShape&&shape!=effectShape){unavailable("Mixed creature target shapes");return;}
            shape=effectShape;anyReal=true;realEffect[k]=true;
            if(kind!=KKillCredit&&kind!=KCreateItem)utilityOnly=false;
        }
        const bool scalable=effectScales(type,aura);
        const auto directSlot=[&](){d.directEffectSlot=++directAmountEffects==1?uint8_t(k):255;};
        const auto range=[&](){return LocalSpellDefinition::NpcAmount{amount,amountHigh,perLevel,scalable,true};};
        switch(kind) {
            case KDamage:directSlot();if(u(3)==kLocalMechanicBleed||u(83+k)==kLocalMechanicBleed)d.directIgnoresArmor=true;d.damage+=low;d.damageMax+=high;d.damagePerLevel+=perLevel;break;
            case KWeapon:
                d.npcWeaponEffect=true;d.npcWeaponScales=type!=17;d.npcWeaponBonus=uint32_t(amount);
                d.npcWeaponBonusMax=d.npcWeaponBonus+(dice>1?uint32_t(dice-1):0);d.npcWeaponBonusPerLevel=perLevel;break;
            case KWeaponPct:d.npcWeaponPercent=uint16_t(amount);d.npcWeaponPercentFirst=!d.npcWeaponEffect;break;
            case KLeech:directSlot();d.damage+=low;d.damageMax+=high;d.damagePerLevel+=perLevel;d.npcLeech=true;d.npcLeechMultiplier=f(101+k);break;
            case KHeal:directSlot();d.heal+=low;d.healMax+=high;d.healPerLevel+=perLevel;d.npcHealAmount={int32_t(low),int32_t(high),perLevel,scalable,true};break;
            case KHealPct:d.npcHealPct={amount,amount,0.f,false,true};break;
            case KHealMax:d.npcHealMax=true;break;
            case KEnergize:d.npcEnergize=range();break;
            case KEnergizePct:d.npcEnergizePct={amount,amount,0.f,false,true};break;
            case KInterrupt:d.npcInterrupt=true;break;
            case KKnockback:d.npcKnockbackSpeedXY=float(misc)/10.f;d.npcKnockbackZ={amount,amountHigh,perLevel,false,true};break;
            case KCosmetic:if(type==6)cosmeticAura=true;break;
            case KPowerBurn:d.npcPowerBurn=range();d.npcPowerBurnMultiplier=f(101+k);break;
            case KPowerDrain:d.npcPowerDrain=range();d.npcPowerDrainMultiplier=f(101+k);break;
            case KDispel:d.npcDispelType=uint8_t(misc);d.npcDispelCount=uint8_t(amount);break;
            case KDispelMechanic:d.npcDispelMechanic=uint8_t(misc);d.npcDispelMechanicCount=uint8_t(amount);break;
            case KCharge:d.npcCharge=true;break;
            case KThreatPct:d.npcThreatPct=int16_t(amount);break;
            case KKillCredit:d.npcKillCredit=uint32_t(misc);break;
            case KCreateItem:d.npcCreateItem=u(107+k);d.npcCreateItemCount=uint8_t(std::max(1,amount));break;
            case KExtraAttacks:d.npcExtraAttacks=uint8_t(amount);break;
            case KTrigger:d.npcTriggerSpellId=trigger;break;
            case KPeriodicTrigger:d.npcPeriodicTriggerSpellId=trigger;d.npcPeriodicTriggerIntervalMs=amplitude;break;
            case KInstakillSelf:d.npcInstakillSelf=true;break;
            case KSelfControl:if(d.npcSelfControl){unavailable("Mixed creature self controls");return;}d.npcSelfControl=aura==12?1:2;break;
            case KInvisible:d.npcInvisible=true;break;
            case KPeriodic:case KPeriodicLeech:
                if(d.periodicHeal){unavailable("Mixed creature periodic effects");return;}
                d.periodicEffectSlot=uint8_t(k);
                if(u(3)==kLocalMechanicBleed||u(83+k)==kLocalMechanicBleed)d.periodicIgnoresArmor=true;
                d.periodicDamage=low;d.periodicDamageMax=high;d.periodicDamagePerLevel=perLevel;d.periodicIntervalMs=amplitude;
                if(kind==KPeriodicLeech){d.npcPeriodicLeech=true;d.npcLeechMultiplier=f(101+k);}
                break;
            case KPeriodicHeal:
                if(d.periodicDamage){unavailable("Mixed creature periodic effects");return;}
                d.periodicEffectSlot=uint8_t(k);d.periodicHeal=low;d.periodicHealMax=high;d.periodicHealPerLevel=perLevel;d.periodicIntervalMs=amplitude;
                d.npcPeriodicHealAmount={int32_t(low),int32_t(high),perLevel,scalable,true};break;
            case KSlow:
                // On the creature itself the slow is its own movement speed.
                if(friendlyShape)d.npcSpeed={amount,amount,0.f,false,true};else{d.npcSlowPercent=uint8_t(-amount);d.npcSlowPerLevel=perLevel;}
                break;
            case KSpeed:d.npcSpeed={amount,amount,0.f,false,true};break;
            case KArmor:d.npcArmorAmount=amount;d.npcArmorPerLevel=perLevel;d.npcArmorAmountMax=amountHigh;break;
            case KResistance:d.npcResistance=range();d.npcResistanceSchool=uint8_t(misc);break;
            case KArmorPct:d.npcArmorPercent=int8_t(std::clamp(amount,-99,127));d.npcArmorPercentPerLevel=perLevel;d.npcArmorPercentWide=int16_t(amount);break;
            case KControl:
                if(d.npcPlayerControl){unavailable("Mixed creature controls");return;}
                d.npcPlayerControl=aura==12?1:aura==26?2:aura==7?3:aura==5?4:5;
                d.npcBreakOnDamage=(u(spell335::AuraInterruptFlags)&0x2u)!=0;break;
            case KDamageTakenFlat:case KDamageTakenPct:
                if(modSchool&&modSchool!=uint8_t(misc)){unavailable("Mixed creature damage-taken schools");return;}
                modSchool=uint8_t(misc);d.npcDamageTakenSchool=modSchool;
                (kind==KDamageTakenFlat?d.npcDamageTakenFlat:d.npcDamageTakenPct)={amount,amount,perLevel,false,true};break;
            case KHealingPct:d.npcHealingPct={amount,amount,perLevel,false,true};break;
            case KHaste:d.npcHaste={amount,amount,perLevel,false,true};break;
            case KCastSpeed:d.npcCastSpeed={amount,amount,perLevel,false,true};break;
            case KHitChance:d.npcHitChance={amount,amount,perLevel,false,true};break;
            case KDisarm:d.npcDisarm=true;break;
            case KDodge:d.npcDodge={amount,amount,perLevel,false,true};break;
            case KParry:d.npcParry={amount,amount,perLevel,false,true};break;
            case KBlock:d.npcBlock={amount,amount,perLevel,false,true};break;
            case KDamagePct:d.npcDamagePct={amount,amount,perLevel,false,true};d.npcDamagePctSchool=uint8_t(misc);break;
            case KDamageFlat:d.npcDamageFlat={amount,amount,perLevel,false,true};d.npcDamageFlatSchool=uint8_t(misc);break;
            case KAttackPower:d.npcAttackPower={amount,amount,perLevel,false,true};break;
            case KAbsorb:d.npcAbsorb=range();d.npcAbsorbSchool=uint8_t(misc);break;
            case KSchoolImmunity:d.npcSchoolImmunity=uint8_t(misc);break;
            case KDamageImmunity:d.npcDamageImmunity=uint8_t(misc);break;
            case KMechanicImmunity:d.npcMechanicImmunity|=1u<<uint32_t(misc);break;
            case KMaxHealth:d.npcMaxHealth={amount,amount,perLevel,false,true};break;
            case KMaxHealthPct:d.npcMaxHealthPct={amount,amount,perLevel,false,true};break;
            case KDamageShield:d.npcDamageShield=range();break;
            case KProcTrigger:d.npcProcSpellId=trigger;break;
            case KProcDamage:d.npcProcDamage=range();break;
            case KSummon:break;
        }
    }
    if(shape==NoShape){if(cosmeticShape==NoShape){if(!cosmeticFree){unavailable("No supported creature effect");return;}shape=Self;}else shape=cosmeticShape;}
    if(d.npcGroundAura&&unitAuras){unavailable("Mixed creature ground and unit auras");return;}
    if((d.npcSummonEntry||d.npcGroundAura)&&(d.npcNextSwing||d.npcChannel||d.npcChainTargets)){unavailable("Unsupported creature summon shape");return;}
    if(d.npcChainTargets)for(uint32_t k=0;k<3;++k)if(u(71+k)&&realEffect[k]&&chainKinds[k]!=d.npcChainTargets){unavailable("Unsupported creature chain shape");return;}
    const bool weapon=d.npcWeaponEffect||d.npcWeaponPercent;
    if(d.damage&&weapon){unavailable("Mixed creature weapon and school damage");return;}
    if(d.npcLeech&&weapon){unavailable("Mixed creature leech and weapon damage");return;}
    if(weapon&&shape!=Enemy&&shape!=Cone&&shape!=Aoe){unavailable("Unsupported creature weapon shape");return;}
    if(d.npcNextSwing&&(shape==AoeTarget||shape==AoeAlly||shape==AoeDest)){unavailable("Unsupported creature next-swing shape");return;}
    if(d.npcChannel&&(d.npcChainTargets||!d.durationMs||d.npcNextSwing||shape==AoeTarget||shape==AoeDest)){unavailable("Unsupported creature channel shape");return;}
    if((shape==AoeTarget||shape==AoeDest)&&weapon){unavailable("Unsupported creature weapon shape");return;}
    if(u(212)>255){unavailable("Unsupported creature target count");return;}
    d.npcTargetShape=shape;d.npcPositive=shape==Self||shape==AoeAlly;d.npcCosmetic=!anyReal||cosmeticAura;
    d.npcUtility=anyReal&&utilityOnly;
    d.npcMaxTargets=uint8_t(u(212));
    d.healingSelfOnly=d.npcPositive;d.buffSelfOnly=false;
}

/// Decode one Spell.dbc row into the local ruleset's shape. Sets
/// `unsupportedReason` for anything this simulation cannot honestly run and
/// returns whether the spell came through supported.
///
/// This is the original starter-spell decoder, moved out whole so the class
/// abilities selected from SkillLineAbility.dbc go through exactly the same
/// rules. Build 12340 column layout, matching Data/expansions/wotlk/dbc_layouts.json.
// creatureCaster: the row is decoded for a creature. SpellInfo::CalcPowerCost
// never reads ManaCostPerlevel (column 43), so it does not block such a row.
// spell_warl_create_healthstone::iTypes, first column: the healthstone of
// each Create Healthstone rank (Minor ... Fel).
// The mechanics a creature control on a player can carry (charm, disorient,
// fear, root, sleep, stun, freeze, knockout, polymorph, horror, sap): a class
// buff's immunity to them is modelled; snare, silence and the rest are not.
inline bool localControlMechanic(int32_t m) {
    switch(m){case 1:case 2:case 5:case 7:case 10:case 12:case 13:case 14:case 17:case 24:case 30:return true;default:return false;}
}
inline uint32_t localHealthstoneItem(uint32_t spellId) {
    switch(spellId) {
    case 6201: return 5512; case 6202: return 5511; case 5699: return 5509; case 11729: return 5510;
    case 11730: return 9421; case 27230: return 22103; case 47871: return 36889; case 47878: return 36892;
    default: return 0;
    }
}
inline bool decodeClientSpell(const ClientSpellTables& t, uint32_t row, LocalSpellDefinition& d, bool creatureCaster=false) {
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto i=[&](uint32_t col){return t.spells->getInt32(row,col);};
    const auto f=[&](uint32_t col){return t.spells->getFloat(row,col);};
    const auto unavailable=[&](const std::string& reason) {
        if(d.unsupportedReason.empty()) d.unsupportedReason=reason;
    };
    const auto sourceName=t.spells->getString(row,136);
    if(!sourceName.empty()&&sourceName.size()<=96) d.name=sourceName;
    d.iconId=u(133);d.resourceType=uint8_t(u(41));d.mana=u(42);d.manaPercent=u(204);
    // Keep presentation metadata in this compact cache; local casting must not
    // load the full Spell.dbc name cache again.
    d.visualId=u(131);d.schoolMask=u(225);
    d.sourceDamageClass=uint8_t(u(213));d.sourceCantCrit=(u(6)&0x20000000u)!=0;
    d.sourceNotAProc=(u(7)&0x200u)!=0;
    d.sourceDoNotConsumeResources=(u(10)&0x20u)!=0;
    d.sourceIgnoreCasterModifiers=(u(7)&0x20000000u)!=0;
    d.sourceCantReflect=(u(4)&(0x10u|0x20000000u|0x40u))||(u(5)&0x80u)||u(213)!=1;
    d.sourceAlwaysHit=(u(7)&0x40000u)!=0;
    d.sourceProjectileSpeed=f(47);
    // P04 immunity and resistance inputs, carried raw and ahead of every
    // reviewed-profile early return so the ranged and form profiles carry them
    // too: the effect slots IsEffect() would walk, the Dispel column, and the
    // three attribute bits the predicates read (SharedDefines.h:399, :470, :518).
    d.effectMask=0;for(unsigned k=0;k<3;++k)if(u(71+k))d.effectMask|=uint8_t(1u<<k);
    if(u(2)>11)unavailable("Spell dispel type is outside the client's dispel table");
    else d.dispelType=uint8_t(u(2));
    d.sourceNoImmunities=(u(4)&0x20000000u)!=0;
    d.sourceNoSchoolImmunities=(u(6)&0x04000000u)!=0;
    d.sourceNoCastLog=(u(8)&0x1u)!=0;
    // P04 stacking inputs : Effect and EffectApplyAuraName per slot,
    // NO_THREAT, the channel bits and DOT_STACKING_RULE (SharedDefines.h:433,
    // :424/:428, :488). Effect ids run to 164 and aura names to 316 in 12340.
    for(unsigned k=0;k<3;++k) {
        if(u(71+k)>255)unavailable("Spell effect is outside the client's effect table");
        else d.sourceEffect[k]=uint8_t(u(71+k));
        if(u(95+k)>65535)unavailable("Aura name is outside the client's aura table");
        else d.effectAura[k]=uint16_t(u(95+k));
    }
    d.sourceNoThreat=(u(5)&0x400u)!=0;d.sourceChanneled=(u(5)&0x44u)!=0;d.sourceDotStackingRule=(u(7)&0x80u)!=0;
    d.sourceAbilityOrTrade=(u(4)&(0x10u|0x20u))!=0;
    // P05 shared combat inputs : SpellLevel (column 39), the three
    // attribute bits the melee-class hit roll, the block and the initial threat
    // read (SharedDefines.h:391, :484, :498), and the two custom attributes
    // SpellMgr::LoadSpellInfoCustomAttributes computes from the effect and aura
    // columns (SpellMgr.cpp:3293-3299, :3341-3358, :3499-3502; spell_custom_attr
    // sets neither on any row at the pin).
    d.spellLevel=uint16_t(std::min<uint32_t>(u(39),65535));
    d.sourceNoActiveDefense=(u(4)&0x200000u)!=0;d.sourceCompletelyBlocked=(u(7)&0x8u)!=0;
    d.sourceSuppressTargetProcs=(u(7)&0x20000u)!=0;
    d.sourceDirectDamage=false;d.sourceNoInitialThreat=false;
    for(unsigned k=0;k<3;++k) {
        switch(u(71+k)) {
            case 2:case 10:case 17:case 31:case 58:case 121:d.sourceDirectDamage=true;break;            // SCHOOL_DAMAGE, HEAL, the four weapon-damage effects
            case 8:case 9:case 24:case 30:case 62:case 67:case 75:case 136:case 137:d.sourceNoInitialThreat=true;break; // drain, leech, create item, energize, burn, heal max / mechanical / pct, energize pct
            default:break;
        }
        if(u(71+k)==6)switch(u(95+k)){case 19:case 44:case 82:case 128:case 140:d.sourceNoInitialThreat=true;break;default:break;} // invisibility detect, track creatures, water breathing, possess pet, ranged haste
    }
    if(u(spell335::SpellFamily)==9&&u(1)==47)d.sourceNoInitialThreat=true; // hunter aspects
    // P05 range, facing and target inputs : FacingCasterFlags (column 19,
    // bit 0 = SPELL_FACING_FLAG_INFRONT, SpellDefines.h:136, read at
    // Spell.cpp:7360), TargetCreatureType (column 17, SpellInfo.cpp:1906-1919)
    // and SPELL_ATTR1_ONLY_PEACEFUL_TARGETS (AttributesEx 0x100,
    // SharedDefines.h:415). Carried raw and ahead of every reviewed-profile
    // early return, so the ranged and form profiles carry them too.
    d.sourceFacingFlags=uint8_t(std::min<uint32_t>(u(19),255));
    d.targetCreatureType=u(17);
    d.sourceOnlyPeacefulTargets=(u(5)&0x100u)!=0;
    // P05 line of sight : the two attributes Spell::CheckCast:6092-6093
    // lets a cast past the LOS test with. AttributesEx2 is column 6 and
    // SPELL_ATTR2_IGNORE_LINE_OF_SIGHT is 0x4 (SharedDefines.h:446);
    // AttributesEx5 is column 9 and SPELL_ATTR5_ALWAYS_AOE_LINE_OF_SIGHT is
    // 0x04000000 (SharedDefines.h:581). Carried raw, like the columns above.
    d.sourceIgnoreLineOfSight=(u(6)&0x4u)!=0||(u(9)&0x04000000u)!=0;
    // P06 : SPELL_ATTR0_ONLY_OUTDOORS, Attributes column 4 bit 0x8000
    // (SharedDefines.h:385), read by Spell::CheckCast at :5902-5904. Carried
    // raw and ahead of every reviewed-profile early return so the form profile
    // carries it too.
    d.sourceOnlyOutdoors=(u(4)&0x8000u)!=0;
    if(d.id==75||d.id==5019)return decodeLocalRangedAuto(t,row,d);
    decodeClientOmenEventMetadata(t,row,d);
    if(u(213)>3)unavailable("Invalid source spell damage class");
    d.spellFamily=u(spell335::SpellFamily);
    for(unsigned k=0;k<3;++k)d.spellFamilyFlags[k]=u(spell335::SpellFamilyFlags+k);
    d.mageArmorGroup=d.spellFamily==3 && (d.spellFamilyFlags[0]&0x12040000u) ? 1 : 0;
    d.interruptFlags=u(spell335::InterruptFlags);d.noPushback=(u(10)&0x8000u)!=0;
    d.noCategoryCooldownMods=(u(10)&0x80000000u)!=0;
    // P04 control metadata. SpellMechanic.dbc holds 31 rows, so anything above
    // that is not a mechanic this client's data can name. AuraInterruptFlags is
    // carried raw: four bits present in the client data have no name in the
    // reference's own enumeration, and validating against that enumeration would
    // discard the 34 rows that use them for no behavioural gain.
    if(u(3)>31)unavailable("Spell mechanic is outside the client's mechanic table");
    else d.mechanic=uint8_t(u(3));
    for(unsigned k=0;k<3;++k) {
        if(u(83+k)>31)unavailable("Effect mechanic is outside the client's mechanic table");
        else d.effectMechanic[k]=uint8_t(u(83+k));
    }
    d.auraInterruptFlags=u(spell335::AuraInterruptFlags);
    d.sourceDamageDoesNotBreakAuras=(u(8)&0x4000u)!=0;
    // Column 214, established by its own distribution: {0 none, 1 silence,
    // 2 pacify} exactly, 1 on every ordinary spell cast and 2 on melee
    // abilities. Column 212 is a different field and is not this enumeration.
    if(u(214)>2)unavailable("Invalid source spell prevention type");
    else d.preventionType=uint8_t(u(214));
    if(u(spell335::StackAmount)>255) unavailable("Aura stack limit exceeds local capacity");
    else d.maxAuraStacks=uint8_t(std::max(1u,u(spell335::StackAmount)));
    const auto* form=localFormProfile(d.id);
    if(form&&d.id!=2645){
        bool valid=u(71)==6&&u(95)==36&&u(110)==form->form&&u(86)==1&&!u(89)&&!u(116);
        if(form->clazz==11)valid=valid&&u(72)==6&&u(96)==77&&u(111)==17&&u(87)==1&&!u(90)&&!u(117)&&
            (d.id==768?(u(73)==6&&u(97)==23&&u(88)==1&&!u(118)):!u(73));
        else if(d.id==1784)valid=valid&&u(72)==6&&u(96)==16&&u(87)==1&&u(73)==6&&u(97)==33&&i(82)==-31&&u(88)==1;
        else valid=valid&&(d.id==2457?(u(72)==6&&u(96)==280&&i(81)==9&&u(87)==1):!u(72))&&!u(73);
        if(valid){
            d.formId=form->form;d.allowableClasses=1u<<(form->clazz-1);
            // Battle Stance's second effect is aura 280,
            // SPELL_AURA_MOD_ARMOR_PENETRATION_PCT, and the profile above has
            // asserted its exact amount since the profile was written. It is now
            // carried instead of discarded, and reaches
            // Unit::CalcArmorReducedDamage's own cap (Unit.cpp:2256-2267)
            // through localFormArmorPenetrationPct. EffectDieSides is 1, so
            // CalcValue's +1 gives +10%.
            if(d.id==2457)d.passiveArmorPenetrationPct=uint8_t(i(81)+int32_t(u(75)));
        }
        else unavailable("Unreviewed shapeshift effect profile");
    }
    decodeClientGhostWolf(t,row,d);
    const bool formBoost=decodeClientFormBoost(t,row,d);
    if(formBoost&&!d.unsupportedReason.empty())return false;
    const bool formResource=decodeClientFormResourceTalent(t,row,d);
    if(formResource&&!d.unsupportedReason.empty())return false;
    if(d.id==34913||d.id==43043||d.id==43044)unavailable("Molten Armor retaliation is an internal triggered spell");
    if(d.id==12536||d.id==16870)unavailable("Clearcasting aura is an internal triggered spell");
    if(d.id==67116)unavailable("Ghost Wolf minimum speed is an internal form passive");
    const bool ward=decodeClientWard(t,row,d);
    const bool molten=decodeClientMoltenArmor(t,row,d);
    const bool reactive=decodeClientReactiveShield(t,row,d);
    const bool earthShield=decodeClientEarthShield(t,row,d);
    const bool bloodthirst=decodeClientBloodthirst(t,row,d);
    const auto simpleShield=decodeClientSimpleShield(t,row,d);
    const bool snare=decodeClientNpcSnare(t,row,d);
    const bool combo=decodeClientComboProfile(t,row,d);
    const bool summonPet=decodeClientSummonPetProfile(t,row,d);
    // 2.38: a creature caster's persistent area auras and summons are the
    // creature effect decoder's (decodeCreatureEffects); the reviewed raid
    // area aura profile speaks for player spells alone.
    const bool areaAura=creatureCaster?false:decodeClientAreaAuraProfile(t,row,d);
    // Higher Claw ranks carry legacy proc flags, but their exact profile has
    // no aura/trigger effect that could install a proc. No generic bypass.
    // Drain Soul: a channel whose SPELL_AURA_CHANNEL_DEATH_ITEM (86) yields a
    // Soul Shard; its caster-side proc aura is the execute-range bonus script.
    bool deathItemChannel=false;
    if(!creatureCaster&&(u(5)&0x44u))for(uint32_t e=0;e<3;++e)if(u(71+e)==6&&u(95+e)==86&&u(86+e)==6)deathItemChannel=true;
    // A hunter Aspect (SPELL_SPECIFIC_ASPECT, until cancelled): its proc
    // effects are talent hooks (Improved Aspect of the Hawk, the Cheetah's daze)
    // and are set aside; Cheetah's daze is the combat rule in the runtime.
    const bool aspectSpell=!creatureCaster&&d.spellFamily==9&&u(40)==21&&localSpellSpecific(d)==LocalSpellSpecific::Aspect;
    // Creature tracking (SPELL_AURA_TRACK_CREATURES on the caster, until
    // cancelled; Gas Cloud Tracking 30645 is a quest workaround, not one).
    bool trackerSpell=false;
    if(!creatureCaster&&u(40)==21&&d.id!=30645)for(uint32_t e=0;e<3;++e)
        if(u(71+e)==6&&u(95+e)==44&&u(86+e)==1&&i(110+e)>=1&&i(110+e)<=12)trackerSpell=true;
    // Prowl: the cat's stealth, held until cancelled or broken (localStealthed).
    const bool prowlSpell=!creatureCaster&&d.id==kLocalProwlSpell&&u(40)==21&&u(71)==6&&u(95)==16&&u(86)==1;
    // A death knight presence (SPELL_SPECIFIC_PRESENCE): one at a time, until changed.
    const bool presenceSpell=!creatureCaster&&u(40)==21&&localSpellSpecific(d)==LocalSpellSpecific::Presence;
    // Righteous Fury: the paladin's holy threat, until cancelled.
    const bool furySpell=!creatureCaster&&d.spellFamily==10&&u(40)==21&&u(71)==6&&u(95)==10&&u(86)==1&&u(110)==2;
    const bool untilCancelled=aspectSpell||trackerSpell||prowlSpell||presenceSpell||furySpell;
    // A shaman totem: one SPELL_EFFECT_SUMMON whose SummonProperties row is a
    // totem slot (63 fire, 81 earth, 82 water, 83 air; Title 4).
    const bool totemSummon=!creatureCaster&&d.spellFamily==11&&u(71)==28&&!u(72)&&!u(73)&&u(110)&&
        (u(113)==63||u(113)==81||u(113)==82||u(113)==83);
    if(totemSummon)d.totemEntry=u(110);
    // Sap ranks 1-2: a lone stun whose legacy taken-damage proc flags restate
    // AURA_INTERRUPT_FLAG_TAKE_DAMAGE (rank 3 carries the same break without
    // them). No proc effect or trigger exists, so the break is the whole rule.
    const bool damageBrokenControl=!creatureCaster&&d.spellFamily==8&&u(71)==6&&u(95)==12&&u(86)==6&&
        !u(72)&&!u(73)&&!u(116)&&!u(spell335::ProcCharges)&&(u(spell335::AuraInterruptFlags)&kLocalAuraInterruptTakeDamage);
    // Blind: SPELL_AURA_MOD_CONFUSE on one hostile unit. The confused creature
    // is held as an incapacitation (it neither moves nor acts), so the slow
    // that rides beside the confuse - the reference's pace for the wander -
    // has nothing to slow and is set aside with it. Only that whole shape: a
    // confuse beside a strike (Scatter Shot) keeps its own older handling.
    bool confuseControl=false,confuseOther=false;
    for(unsigned k=0;k<3;++k) {
        if(!u(71+k))continue;
        if(u(71+k)==6&&u(95+k)==5&&u(86+k)==6&&!u(89+k))confuseControl=true;
        else if(u(71+k)==6&&u(95+k)==56&&u(86+k)==6&&!u(89+k)&&k==1&&d.spellFamily==3&&u(110+k)&&localMountDisplay(u(110+k))){} // Polymorph's transform
        else if(!(u(71+k)==6&&u(95+k)==33&&u(86+k)==6))confuseOther=true;
    }
    confuseControl=confuseControl&&!confuseOther&&!creatureCaster;
    // Fear: SPELL_AURA_MOD_FEAR on one hostile unit beside the run-speed aura
    // the reference gives the fleeing target. Held as an incapacitation like
    // Blind, so the speed has nothing to act on and is set aside. Its taken-
    // damage proc flags are the reference's damage cap (CalculateAmount gives
    // a proc-flagged MOD_FEAR/STUN/ROOT 10% of the target's maximum health,
    // spent by each damaging hit until it runs out), not a proc to install.
    // Death Coil: a shadow HEALTH_LEECH on one hostile unit, then MOD_FEAR
    // (horror) on it with nothing else; its fear has no damage cap (no proc
    // flags) and no run speed.
    const bool horrorCoil=!creatureCaster&&d.spellFamily==5&&u(71)==9&&u(86)==6&&!u(89)&&u(72)==6&&u(96)==7&&u(87)==6&&!u(90)&&
        !u(117)&&!u(73)&&!u(spell335::ProcFlags)&&!u(spell335::ProcCharges)&&std::isfinite(f(101))&&f(101)>0&&f(101)<=10;
    // Incinerate: one direct fire hit and nothing else; its ProcCharges column
    // is meaningless without an aura (charges belong to auras).
    const bool incinerate=!creatureCaster&&d.spellFamily==5&&(d.spellFamilyFlags[1]&0x40u)&&u(133)==2128&&
        u(71)==2&&u(86)==6&&!u(89)&&!u(72)&&!u(73)&&!u(spell335::ProcFlags);
    if(incinerate)d.immolateBonus=true;
    // Entangling Roots: SPELL_AURA_MOD_ROOT on one hostile unit beside a DoT
    // on it; its taken-damage proc flags are the 10% damage cap, as Fear's.
    const bool rootControl=!creatureCaster&&u(71)==6&&u(95)==26&&u(86)==6&&!u(89)&&!u(116)&&
        u(72)==6&&u(96)==3&&u(87)==6&&!u(90)&&!u(117)&&!u(73)&&!u(spell335::ProcCharges)&&
        !(u(spell335::AuraInterruptFlags)&kLocalAuraInterruptTakeDamage);
    const bool fearControl=!creatureCaster&&u(71)==6&&u(95)==7&&u(86)==6&&!u(89)&&!u(116)&&
        u(72)==6&&u(96)==31&&u(87)==6&&!u(90)&&!u(117)&&!u(73)&&!u(spell335::ProcCharges)&&
        !(u(spell335::AuraInterruptFlags)&kLocalAuraInterruptTakeDamage);
    // Inner Fire: armor whose charges each melee or ranged hit taken spends
    // (spell_proc -588: SpellTypeMask damage, nothing triggered).
    const bool innerFire=!creatureCaster&&d.spellFamily==6&&u(71)==6&&u(95)==22&&u(86)==1&&(i(110)&1)&&!u(116)&&
        u(spell335::ProcCharges)&&u(spell335::ProcCharges)<=100&&u(spell335::ProcChance)==100&&!(u(spell335::ProcFlags)&~0x2a8u);
    // Frost Armor: armor and a PROC_TRIGGER_SPELL putting Chilled on a creature
    // that lands a melee hit on the mage (ProcFlags 0x28, spell_proc -168):
    // Chilled is MOD_DECREASE_SPEED (33) and MELEE_SLOW (138) on that creature,
    // beside an empty healing modifier.
    uint32_t chillEffect=3;
    if(!creatureCaster&&d.spellFamily==3&&u(spell335::ProcFlags)==0x28u&&!u(spell335::ProcCharges)&&u(spell335::ProcChance)==100)
        for(uint32_t e=0;e<3;++e)if(u(71+e)==6&&u(95+e)==42&&u(86+e)==1&&!u(89+e)&&u(116+e)) {
            const auto child=ClientSpellTables::lookup(t.spellIndex,u(116+e));
            if(child<0)break;
            const auto cu=[&](uint32_t col){return t.spells->getUInt32(uint32_t(child),col);};
            const auto ci=[&](uint32_t col){return t.spells->getInt32(uint32_t(child),col);};
            const auto durationRow=ClientSpellTables::lookup(t.durationIndex,cu(40));
            const int32_t duration=durationRow>=0?t.durations->getInt32(uint32_t(durationRow),1):0;
            int32_t haste=0,speed=0;bool other=duration<=0||duration>60000;
            for(uint32_t k=0;k<3;++k)if(cu(71+k)) {
                const int32_t amount=ci(80+k)+(cu(74+k)?1:0);
                if(cu(71+k)!=6||cu(86+k)!=6||cu(89+k))other=true;
                else if(cu(95+k)==33&&amount<0&&amount>-100)speed=amount;
                else if(cu(95+k)==138&&amount<0&&amount>-100)haste=amount;
                else if(!(cu(95+k)==118&&!amount))other=true;
            }
            if(!other&&haste&&speed){chillEffect=e;d.chillSpell=u(116+e);d.chillDurationMs=uint32_t(duration);d.chillHastePct=int8_t(haste);d.chillSpeedPct=int8_t(speed);}
            break;
        }
    const bool chillArmor=chillEffect<3;
    // Frost Nova: frost damage and a root on the enemies around the mage
    // (22/15 on both effects); its taken-damage proc flags are the root's 10%
    // damage cap, as Entangling Roots'.
    const bool frostNova=!creatureCaster&&d.spellFamily==3&&(d.spellFamilyFlags[0]&0x40u)&&u(71)==2&&u(86)==22&&u(89)==15&&
        u(72)==6&&u(96)==26&&u(87)==22&&u(90)==15&&!u(73)&&!u(116)&&!u(117)&&u(spell335::ProcFlags)==0xa22a8u&&!u(spell335::ProcCharges);
    // Psychic Scream, Howl of Terror: MOD_FEAR on the enemies around the
    // caster (22/15) beside the fleeing run speed, up to MaxAffectedTargets;
    // its taken-damage proc flags are the fear's 10% damage cap, as Fear's.
    const bool areaFear=!creatureCaster&&(d.spellFamily==5||d.spellFamily==6)&&u(71)==6&&u(95)==7&&u(86)==22&&u(89)==15&&
        u(72)==6&&u(96)==31&&u(87)==22&&u(90)==15&&!u(73)&&!u(116)&&!u(117)&&u(spell335::ProcFlags)==0xa22a8u&&!u(spell335::ProcCharges)&&u(40)&&u(212)<=10;
    // Intimidating Shout: a TRIGGER_SPELL of 20511 (a lone stun that damage
    // breaks) on its target, then the same area fear and run speed.
    const bool intimidatingShout=!creatureCaster&&d.spellFamily==4&&u(71)==64&&u(86)==6&&!u(89)&&u(116)&&
        u(72)==6&&u(96)==7&&u(87)==22&&u(90)==15&&u(73)==6&&u(97)==31&&u(88)==22&&u(91)==15&&!u(117)&&!u(118)&&
        u(spell335::ProcFlags)==0xa22a8u&&!u(spell335::ProcCharges)&&u(40)&&u(212)<=10&&[&]{
            const auto child=ClientSpellTables::lookup(t.spellIndex,u(116));
            if(child<0)return false;
            const auto cu=[&](uint32_t col){return t.spells->getUInt32(uint32_t(child),col);};
            return cu(71)==6&&cu(95)==12&&cu(86)==6&&!cu(89)&&!cu(72)&&!cu(73)&&!cu(spell335::ProcFlags)&&
                (cu(spell335::AuraInterruptFlags)&kLocalAuraInterruptTakeDamage)&&cu(40)==u(40);}();
    // Vanish: two TRIGGER_SPELLs on the rogue (11327's stealth bonus, 18461's
    // purge) and SANCTUARY; its proc columns mean nothing without an aura of
    // its own (charges belong to auras), the script does the rest.
    const bool vanishSpell=!creatureCaster&&d.spellFamily==8&&u(71)==64&&u(86)==1&&u(116)&&u(72)==64&&u(87)==1&&u(117)==18461&&
        u(73)==79&&u(88)==1&&!u(118)&&!u(40);
    // Spell Reflection: REFLECT_SPELLS on the warrior, one charge (spent by
    // the reflection itself, not by a proc).
    const bool spellReflection=!creatureCaster&&d.spellFamily==4&&u(71)==6&&u(95)==28&&u(86)==1&&!u(89)&&!u(116)&&!u(72)&&!u(73)&&
        u(spell335::ProcCharges)==1&&i(80)+int32_t(u(74))>0&&i(80)+int32_t(u(74))<=100;
    // Retaliation: a lone empty DUMMY on the warrior whose script answers melee
    // hits taken (ProcFlags 0x28) with 20240, a plain WEAPON_DAMAGE strike.
    const bool retaliation=!creatureCaster&&d.id==20230&&u(71)==6&&u(95)==4&&u(86)==1&&!u(72)&&!u(73)&&
        u(spell335::ProcFlags)==0x28u&&u(spell335::ProcCharges)&&u(spell335::ProcCharges)<=100&&[&]{
            const auto child=ClientSpellTables::lookup(t.spellIndex,20240);
            if(child<0)return false;
            const auto cu=[&](uint32_t col){return t.spells->getUInt32(uint32_t(child),col);};
            return cu(71)==58&&cu(86)==6&&!cu(80)&&!cu(72)&&!cu(73);}();
    // Recklessness: a critical-chance spell modifier on warrior specials, 20%
    // more damage taken and fear immunity; its charges are spent by the
    // specials it modifies (spell_proc 1719), not by a proc of its own.
    const bool recklessness=!creatureCaster&&d.id==1719&&u(71)==6&&u(95)==107&&u(110)==7&&u(86)==1&&u(72)==6&&u(96)==87&&u(111)==127&&
        u(73)==6&&u(97)==77&&u(112)==5&&u(spell335::ProcCharges)&&u(spell335::ProcCharges)<=10&&(u(122)||u(123));
    // Lay on Hands: HEAL_MAX_HEALTH on a friendly target, and from rank 2 an
    // ENERGIZE of mana on it.
    const bool layOnHands=!creatureCaster&&d.spellFamily==10&&u(71)==67&&u(86)==21&&!u(89)&&!u(73)&&
        (!u(72)||(u(72)==30&&u(87)==21&&!u(90)&&!u(111)&&i(81)>=0&&i(81)<100000));
    // Enrage: PERIODIC_ENERGIZE of rage on the druid, an ENERGIZE of rage and
    // an empty INTERRUPT_REGEN, in a bear form.
    const bool druidEnrage=!creatureCaster&&d.id==5229&&u(71)==6&&u(95)==24&&u(86)==1&&u(110)==1&&u(98)>=1000&&
        u(72)==30&&u(87)==1&&u(111)==1&&u(73)==6&&u(97)==94&&u(88)==1&&!i(82);
    // Seals of Light, Wisdom and Justice (rank 1 ids; spell_proc 20165, 20166,
    // 20164): a PROC_TRIGGER_SPELL on the paladin beside an empty modifier.
    const uint8_t sealKind=creatureCaster||d.spellFamily!=10||u(71)!=6||u(95)!=42||u(86)!=1||u(spell335::ProcFlags)!=0x14u?0:
        u(116)==20167?2:u(116)==20168?3:u(116)==20170?4:0;
    // Prayer of Healing: one HEAL at the friendly target and its party around it
    // (21/37), with the radius of that area.
    const bool partyHeal=!creatureCaster&&d.spellFamily==6&&u(71)==10&&u(86)==21&&u(89)==37&&!u(72)&&!u(73);
    if(partyHeal) {
        const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92));
        const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
        if(std::isfinite(r)&&r>0&&r<=40){d.partyHealRadius=r;d.chainMultiplierPermille=1000;}else unavailable("Unreviewed party heal radius");
    }
    // Cloak of Shadows: the spell-hit reduction, the server-side 35729 (a
    // magic dispel of the rogue) and an empty physical damage-taken hook.
    const bool cloakSpell=!creatureCaster&&d.spellFamily==8&&u(71)==6&&u(95)==186&&u(86)==1&&u(110)==126&&u(72)==64&&u(117)==35729&&
        u(73)==6&&u(97)==87&&u(112)==1&&i(82)+int32_t(u(76))==0&&u(40);
    if((u(spell335::ProcFlags)||u(spell335::ProcCharges))&&!aspectSpell&&d.id!=kLocalProwlSpell&&!reactive&&!earthShield&&!molten&&!combo&&simpleShield!=SimpleShieldKind::Mana&&!creatureCaster&&!deathItemChannel&&!damageBrokenControl&&!fearControl&&!rootControl&&!incinerate&&!innerFire&&!chillArmor&&!frostNova&&!areaFear&&!intimidatingShout&&!vanishSpell&&!spellReflection&&!retaliation&&!recklessness&&!sealKind&&d.id!=1784&&d.id!=21084&&d.id!=20154&&
       !(d.id==6346&&u(spell335::ProcCharges)==1)&&!(d.id==22812&&!u(spell335::ProcChance))) // Fear Ward's immunity charge, Barkskin's inert proc (classBuff) // Stealth: its damage and attack breaks are the form rule
        unavailable("This proc family or its trigger conditions are not implemented");
    // Spell.dbc column 38 is BaseLevel and column 39 is SpellLevel
    // (DBCStructure.h:1679-1680). previously both this field and d.spellLevel
    // were read from column 39, so the definition carried SpellLevel twice and
    // the real BaseLevel nowhere. They are two different rules in the reference:
    // SpellEffectInfo::CalcValue (SpellInfo.cpp:414-431) clamps the caster level
    // up to BaseLevel and down to MaxLevel and then subtracts
    // max(BaseLevel, SpellLevel), while Unit::CalculateLevelPenalty
    // (Unit.cpp:3211-3220) and the pet-availability tests (Spell.cpp:6333, :6355;
    // Pet.cpp:1950) read SpellLevel alone.
    d.baseLevel=u(38);d.maxLevel=u(37);d.cooldownMs=u(29);d.cooldownCategory=u(1);d.categoryCooldownMs=u(30);d.globalCooldownMs=u(206);
    // A creature takes no ordinary spell cooldown at the pin (Spell::SendSpellCooldown
    // returns before adding one), so its category metadata is not checked.
    // A category recovery without a category (Anti-Magic Shell) is never
    // applied by the reference (Spell::SendSpellCooldown keys it by category).
    if(!creatureCaster&&(d.id==48707||(d.spellFamily==7&&u(71)==6&&u(95)==13&&u(110)==1&&!u(72)))&&d.categoryCooldownMs&&!d.cooldownCategory)d.categoryCooldownMs=0; // Anti-Magic Shell, Tiger's Fury
    if(d.cooldownCategory>100000||d.spellFamily>1000||(!creatureCaster&&d.categoryCooldownMs&&!d.cooldownCategory))unavailable("Invalid spell cooldown category metadata");
    if(d.resourceType==1||d.resourceType==6) d.mana=(d.mana+9)/10; // displayed rage/runic units
    // Life Tap: POWER_HEALTH with no cost, one DUMMY effect on the caster and
    // the warlock family (its SpellScript, spell_warl_life_tap, does the work).
    const bool lifeTap=!creatureCaster&&u(41)==0xFFFFFFFEu&&!u(42)&&!u(43)&&!u(44)&&!u(204)&&d.spellFamily==5&&
        u(71)==3&&u(86)==1&&!u(89)&&!u(72)&&!u(73)&&u(74)==1&&i(80)>=0&&i(80)<100000&&std::isfinite(f(77))&&f(77)>=0&&f(77)<100;
    // Bloodrage: POWER_HEALTH costing a percent of base health, an ENERGIZE of
    // rage on the warrior and a TRIGGER_SPELL of its rage-over-time aura.
    const bool bloodrage=!creatureCaster&&d.spellFamily==4&&u(41)==0xFFFFFFFEu&&!u(42)&&!u(43)&&!u(44)&&u(204)>0&&u(204)<100&&
        u(71)==30&&u(86)==1&&!u(89)&&u(110)==1&&u(72)==64&&u(87)==1&&!u(90)&&u(117)&&!u(73)&&!d.durationMs;
    if(u(41)!=0&&u(41)!=1&&u(41)!=3&&u(41)!=5&&u(41)!=6&&!lifeTap&&!bloodrage)
        unavailable("This power system is not implemented");
    if(lifeTap)d.resourceType=0; // no cost: health is spent by the effect itself
    if(bloodrage){d.resourceType=1;d.mana=0;d.healthCostBasePct=uint8_t(u(204));d.manaPercent=0;} // health, not rage, pays
    if(!creatureCaster&&(d.spellFamily==15||d.spellFamily==9)){bool periodic=false;const auto ap=localSpellApBonus(d.id,periodic);(periodic?d.periodicApPer100k:d.apBonusPer100k)=ap;}
    if(u(226)) {
        const auto runeRow=ClientSpellTables::lookup(t.runeCostIndex,u(226));
        if(runeRow<0) unavailable("SpellRuneCost record missing or incompatible");
        else {
            bool valid=true;
            for(uint32_t kind=0;kind<3;++kind) {
                const auto cost=t.runeCosts->getUInt32(runeRow,1+kind);
                if(cost>2) valid=false;
                else d.runeCost[kind]=uint8_t(cost);
            }
            const auto gain=t.runeCosts->getUInt32(runeRow,4);
            if(gain>1000||gain%10) valid=false;
            else d.runicPowerGain=uint16_t(gain/10);
            if(!valid) unavailable("Invalid or unsupported rune cost/power gain");
        }
    }
    if(u(41)==5&&(d.mana||d.manaPercent)) unavailable("Rune spell has a non-rune resource cost");
    if((u(43)&&!creatureCaster)||u(44)||u(45)) unavailable("Scaling or periodic resource costs are not implemented");
    // A creature's next-swing special replaces its next main-hand swing
    // (Unit::AttackerStateUpdate casts CURRENT_MELEE_SPELL instead).
    // ON_NEXT_SWING (Heroic Strike, Raptor Strike, Cleave, Maul): a player's
    // is struck at once as a weapon attack; the realm has no swing queue.
    if(u(4)&0x404u){if(creatureCaster)d.npcNextSwing=true;else if(u(spell335::SpellFamily)==4)d.nextSwing=true;}
    d.sourceNoAttackDodge=(u(11)&0x00800000u)!=0;d.sourceNoAttackParry=(u(11)&0x01000000u)!=0;d.sourceNoAttackMiss=(u(11)&0x02000000u)!=0;
    // A player channel is admitted when its effect is single-target periodic
    // damage or leech (checked after the effects below).
    if(u(5)&0x44u){if(creatureCaster)d.npcChannel=true;else d.channel=true;}
    // A form boost's applicability is HandleShapeshiftBoosts's switch, not this
    // column: 21178 and 7381 carry Stances = 0 and are still cast for forms 5/8
    // and form 19. decodeClientFormBoost has already written the switch's mask.
    if(!formBoost) {
        d.requiredForms=uint64_t(u(12))|(uint64_t(u(13))<<32);
        d.excludedForms=uint64_t(u(14))|(uint64_t(u(15))<<32);
    }
    d.notShapeshifted=(u(4)&0x10000u)!=0;d.allowWithoutForm=(u(6)&0x80000u)!=0;
    // A charge's ExcludeTargetAuraSpell (65219) names an aura this realm never
    // applies, so the requirement always holds here.
    const bool chargeSpell=!creatureCaster&&(u(71)==96||u(72)==96||u(73)==96);
    if(!creatureCaster)d.outOfCombatOnly=(u(4)&0x10000000u)!=0;
    if(!creatureCaster&&u(21)==2&&!u(20)&&!u(22)&&!u(23)&&!u(24)&&!u(25)&&!u(26)&&!u(27))d.targetMaxHealthPct=20;
    else if(!creatureCaster&&u(20)==1&&d.spellFamily==4&&!u(21)&&!u(22)&&!u(23)&&!u(24)&&!u(25)&&!u(26)&&!u(27))d.requiresDefenseState=true;
    else if(!creatureCaster&&u(20)==10&&d.spellFamily==4&&!u(21)&&!u(22)&&!u(23)&&!u(24)&&!u(25)&&!u(26)&&!u(27))d.requiresVictoryRush=true;
    // Stealth's and Vanish's CasterAuraStateNot 12 (Faerie Fire) never holds here: no
    // creature in this realm casts it on a player.
    else if((u(20)&&!(u(20)==5&&d.spellFamily==10&&(d.id==20271||d.id==53408||d.id==53407)))||u(21)||(u(22)&&!((d.id==1784||d.id==kLocalProwlSpell||vanishSpell)&&u(22)==12))||u(23)||u(24)||u(25)||
            (u(26)&&!localRealmMarkerAura(u(26))&&!(u(26)==50334&&d.spellFamily==7))||(u(27)&&!chargeSpell&&!localRealmMarkerAura(u(27)))) unavailable("Aura requirements are not implemented");
    // The exclusion markers the realm itself applies (Forbearance family).
    if(!creatureCaster&&localRealmMarkerAura(u(26)))d.excludeCasterAuraSpell=u(26);
    if(!creatureCaster&&u(26)==50334&&d.spellFamily==7)d.excludeCasterAuraSpell=50334; // Tiger's Fury: not while Berserk
    if(!creatureCaster&&localRealmMarkerAura(u(27)))d.excludeTargetAuraSpell=u(27);
    // spell_pal_immunities (498 Divine Protection, 642 Divine Shield, 1022 Hand
    // of Protection and its ranks): the Forbearance check and the three auras
    // added after the hit.
    if(!creatureCaster&&d.spellFamily==10&&(d.id==498||d.id==642||d.id==1022||d.id==5599||d.id==10278)) {
        d.forbearanceCheck=true;d.afterHitAuras={25771,61987,61988};
    }
    // Spell::PrepareTriggersExecutedOnHit: a family spell whose excluded aura
    // is negative adds that aura to its target on the hit (Weakened Soul after
    // Power Word: Shield, Hypothermia after Ice Block, the Avenging Wrath
    // marker after Avenging Wrath); DoTriggersOnSpellHit adds Divine Shield's
    // exclude aura beside 61987. Every realm marker is negative.
    // spell_sha_bloodlust / spell_sha_heroism: Sated or Exhaustion after the
    // hit (10 min), and neither lands on a target holding either.
    if(!creatureCaster&&d.spellFamily==11&&(d.id==2825||d.id==32182)){d.afterHitAuras={d.id==2825?57724u:57723u,0,0};d.skipIfHoldsAuras={57724,57723};}
    if(!creatureCaster&&d.spellFamily&&!d.forbearanceCheck&&!d.skipIfHoldsAuras[0]) {
        const uint32_t pre=d.excludeTargetAuraSpell?d.excludeTargetAuraSpell:d.excludeCasterAuraSpell;
        if(pre)d.afterHitAuras={pre,pre==61987?61988u:0u,0};
    }
    if(d.excludeCasterAuraSpell==50334)d.afterHitAuras={0,0,0}; // Berserk is a talent's, never added by Tiger's Fury
    // SPELL_ATTR0_ONLY_STEALTHED: the rogue openers also require Stealth's
    // form; Prowl (a cat-form aura) is not implemented, so Ravage and Pounce wait.
    if(!creatureCaster&&(u(4)&0x20000u)&&!(u(12)&(1u<<29)))d.onlyStealthed=true; // Ravage, Pounce: under Prowl
    d.requiresMainHand=(u(7)&0x400u)!=0;d.requiresOffHand=(u(7)&0x1000000u)!=0;
    if(i(68)>=0) {
        if(i(68)!=2&&i(68)!=4)unavailable("Unsupported spell equipment class");
        else {d.requiredItemClass=int8_t(i(68));d.requiredItemSubclasses=u(69);d.requiredInventoryTypes=u(70);}
    // SpellInfo::IsRangedWeaponSpell: a ranged weapon requirement (bow, gun,
    // crossbow, thrown) makes a hunter spell's attack-power term ranged.
    d.apBonusRanged=!creatureCaster&&d.spellFamily==9&&d.requiredItemClass==2&&(d.requiredItemSubclasses&((1u<<2)|(1u<<3)|(1u<<16)|(1u<<18)));
    }
    for(uint32_t reagent=0;reagent<8;++reagent) if(i(52+reagent)>0) {
        if(creatureCaster||i(60+reagent)<=0||i(60+reagent)>200)unavailable("Spell reagents are not implemented");
        else {d.reagentItems[reagent]=uint32_t(i(52+reagent));d.reagentCounts[reagent]=uint16_t(i(60+reagent));}
    }
    if(u(50)||u(51)) unavailable("Totem requirements are not implemented");
    const auto castRow=ClientSpellTables::lookup(t.castIndex,u(28));
    // SpellInfo::CalcCastTime returns 0 for a non-positive base before any
    // ranged adjustment: the "-1000000" row hunters' shots use is instant.
    bool negativeCast=false;
    if(castRow<0) unavailable("Cast-time record missing");
    else if(t.casts->getInt32(castRow,1)>60000||t.casts->getInt32(castRow,2)!=0)
        unavailable("Variable or invalid cast time is not implemented");
    else if(t.casts->getInt32(castRow,1)<0){negativeCast=true;d.castTimeMs=0;}
    else d.castTimeMs=uint32_t(t.casts->getInt32(castRow,1));
    // SpellInfo::CalcCastTime: a creature's USES_RANGED_SLOT spell (not
    // auto-repeat) takes 500 ms more; a creature's cast-speed and ranged-haste
    // multipliers are 1.
    if(creatureCaster&&!negativeCast&&(u(4)&0x2u)&&!(u(6)&0x20u)&&d.castTimeMs<=59500)d.castTimeMs+=500;
    const auto durationRow=ClientSpellTables::lookup(t.durationIndex,u(40));
    if(u(40)&&durationRow<0) unavailable("Duration record missing");
    else if(durationRow>=0) {
        const auto duration=t.durations->getInt32(durationRow,1);
        if(duration>0&&duration<=3600000) d.durationMs=uint32_t(duration);
        // A rogue/druid finisher outside the reviewed combo profiles
        // (SPELL_ATTR1_REQ_COMBO_POINTS1/2: Rupture, Slice and Dice): it spends
        // the points, and Aura::CalcMaxDuration adds (max - base) / 5 per point.
        if(!creatureCaster&&!d.comboProfile&&(u(5)&0x500000u)&&(d.spellFamily==8||d.spellFamily==7)) {
            d.comboFinisher=true;
            // A zero base (Kidney Shot rank 1) is a finisher that lasts only
            // through its points: 1 s each up to the maximum.
            const auto maxDuration=t.durations->getInt32(durationRow,3);
            if(duration>=0&&maxDuration>duration&&maxDuration<=3600000)d.comboDurationMaxMs=uint32_t(maxDuration);
        }
        // SpellDuration.dbc -1 is not a lease: the aura lives until it is
        // replaced or cancelled. durationMs stays zero and the source fact is
        // carried on its own flag instead of being rounded into a timer.
        else if(duration==-1&&t.durations->getInt32(durationRow,3)==-1) d.indefiniteDuration=true;
        if(t.durations->getInt32(durationRow,2)!=0) unavailable("Variable duration is not implemented");
    }
    if(snare&&(!d.durationMs||d.durationMs>600000))unavailable("Invalid NPC snare duration");
    const bool chainLightning=d.spellFamily==11 && d.spellFamilyFlags[0]==2 && !d.spellFamilyFlags[1] && !d.spellFamilyFlags[2] && u(71)==2;
    const bool chainHeal=d.spellFamily==11 && d.spellFamilyFlags[0]==256 && !d.spellFamilyFlags[1] && !d.spellFamilyFlags[2] && u(71)==10;
    if(!creatureCaster&&(u(104)>1||u(105)>1||u(106)>1)) {
        if((chainLightning||chainHeal)&&u(104)==3&&!u(72)&&!u(73)&&!(u(6)&0x1000u)&&
           std::isfinite(f(216))&&f(216)>0&&f(216)<=1) {
            d.chainTargets=3;d.chainMultiplierPermille=uint16_t(std::lround(f(216)*1000));
            d.chainRadius=chainHeal?12.5f:10.0f;
        }
        // A weapon strike that jumps (Cleave, Multi-Shot): one weapon effect,
        // two or three targets at full damage, the next enemy beside the last.
        else if((u(71)==58||u(71)==121||u(71)==31||u(71)==17)&&u(104)>=2&&u(104)<=3&&!u(72)&&!u(73)&&
                std::isfinite(f(216))&&f(216)>0&&f(216)<=1) {
            d.chainTargets=uint8_t(u(104));d.chainMultiplierPermille=uint16_t(std::lround(f(216)*1000));
            d.chainRadius=u(213)==3?10.0f:8.0f;
        } else unavailable("Unreviewed chain-target profile is not implemented");
    }
    bool pbaoeShape=!creatureCaster&&!u(104)&&!u(212),pbaoeDamage=false;
    for(uint32_t e=0;e<3;++e)if(u(71+e)) {
        // Around the caster: source caster + enemy area (22/15), or the
        // destination at the caster + enemy area at it (18/16, Blood Boil).
        if(!((u(86+e)==22&&u(89+e)==15)||(u(86+e)==18&&u(89+e)==16)))pbaoeShape=false;
        if(u(71+e)==2||u(71+e)==58||u(71+e)==121)pbaoeDamage=true;
    }
    // Cone of Cold: a slow, frost damage and an empty healing-taken hook, each
    // on the enemies in the cone in front of the mage (TARGET_UNIT_CONE_ENEMY_104).
    const bool coneOfCold=!creatureCaster&&d.spellFamily==3&&(d.spellFamilyFlags[0]&0x200u)&&u(71)==6&&u(95)==33&&u(86)==104&&!u(89)&&
        u(72)==2&&u(87)==104&&!u(90)&&u(73)==6&&u(97)==118&&u(88)==104&&!u(91)&&i(82)+int32_t(u(76))==0&&i(80)+1<0&&i(80)+1>-100&&
        !u(116)&&!u(117)&&!u(118)&&!u(spell335::ProcFlags)&&!u(spell335::ProcCharges)&&!u(212);
    // Fan of Knives: one WEAPON_PERCENT_DAMAGE at the enemies around the rogue.
    const bool fanOfKnives=!creatureCaster&&d.spellFamily==8&&(d.spellFamilyFlags[1]&0x40000u)&&u(71)==31&&u(86)==18&&u(89)==16&&
        !u(72)&&!u(73)&&!u(212)&&!u(spell335::ProcFlags);
    // Arcane Explosion's reviewed profile, and every other point-blank area
    // attack in its shape: each effect on the enemies around the caster
    // (TARGET_SRC_CASTER + TARGET_UNIT_SRC_AREA_ENEMY) with a fixed radius.
    const bool arcaneExplosion=(d.spellFamily==3&&d.spellFamilyFlags[0]==4096&&!d.spellFamilyFlags[1]&&!d.spellFamilyFlags[2]&&
        u(71)==2&&!u(72)&&!u(73)&&u(86)==22&&u(89)==15&&!u(104)&&!u(212))||(pbaoeShape&&pbaoeDamage)||coneOfCold||fanOfKnives;
    if(arcaneExplosion) {
        const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92));
        if(!t.radii||radius<0)unavailable("Area radius record missing");
        else {
            const auto base=t.radii->getFloat(radius,1),perLevel=t.radii->getFloat(radius,2),maximum=t.radii->getFloat(radius,3);
            if(!std::isfinite(base)||base<=0||base>30||perLevel!=0||!std::isfinite(maximum)||maximum<base)
                unavailable("Unsupported variable area radius");
            else d.areaRadius=base;
        }
    }
    d.schoolMask=u(225);
    if(d.schoolMask>127)unavailable("Invalid spell school mask");
    d.directIgnoresArmor=d.periodicIgnoresArmor=(u(8)&0x100u)!=0;
    bool harm=false,healing=false,buff=false;
    uint32_t healingTarget=0,buffTarget=0;
    unsigned directAmountEffects=0;
    d.directEffectSlot=d.periodicEffectSlot=255;
    const auto directSlot=[&](uint32_t effect) {
        // A combined amount cannot safely identify one source effect. Keep
        // that distinction explicit instead of applying a slot mod twice.
        d.directEffectSlot=++directAmountEffects==1?uint8_t(effect):255;
    };
    // A player attack's secondary rider the realm does not model (Mortal
    // Strike's healing reduction, Icy Touch's disease, Shield Bash's daze) is
    // set aside when the spell still lands a supported direct hit; without one
    // the first such rider is the refusal, as before.
    std::vector<std::string> setAside;
    const auto rider=[&](uint32_t effect,const std::string& reason){
        const auto type=u(71+effect);
        if(!creatureCaster&&((u(86+effect)==6&&!u(89+effect))||(u(86+effect)==22&&u(89+effect)==15))&&(type==6||type==64)){setAside.push_back(reason);return true;}
        return false;
    };
    // A class stat buff: every effect an aura on the caster, a friendly target
    // or the caster's party/raid, and at least one of them primary stats (29)
    // or attack power (99). Party/raid forms land on the caster here (and on a
    // friendly target when cast at one); other auras in it are set aside.
    bool classBuffSpell=false;
    // A proc that can never fire: Spell.dbc ProcChance 0 with no spell_proc
    // row at the pin (Barkskin's 50411 trigger).
    const bool inertProc=!u(spell335::ProcChance)&&!u(spell335::ProcCharges)&&d.id==22812;
    // Hand of Salvation: its periodic trigger is 53055, MODIFY_THREAT_PERCENT.
    const bool salvation=!creatureCaster&&d.spellFamily==10&&u(71)==6&&u(95)==227&&u(116)==53055&&u(98)>=1000&&[&]{
        const auto r=ClientSpellTables::lookup(t.spellIndex,53055);
        return r>=0&&t.spells->getUInt32(uint32_t(r),71)==125;}();
    // Fear Ward: one charge spent by PROC_HIT_IMMUNE (spell_proc 6346).
    const bool immunityCharge=d.id==6346&&u(spell335::ProcCharges)==1;
    if(!creatureCaster&&(d.durationMs||untilCancelled)) {
        bool any=false,shape=true;
        for(uint32_t e=0;e<3;++e) if(u(71+e)) {
            const auto ty=u(71+e),au=u(95+e),tg=u(86+e);
            if(untilCancelled&&ty==6&&tg==1&&(au==42||au==87||au==168)&&i(80+e)==-1)continue; // talent proc hook, empty modifier
            if(prowlSpell&&ty==6&&tg==1&&au==33)continue; // its slow
            if(presenceSpell&&ty==6&&tg==1&&(au==10||au==107||au==118))continue; // threat, global cooldown, marker
            if((ty!=6&&ty!=35&&ty!=65)||u(89+e)||(u(116+e)&&!(inertProc&&au==42)&&!(chillArmor&&e==chillEffect)&&!(salvation&&e==0))||(tg!=1&&tg!=21&&tg!=25&&tg!=20&&tg!=22&&tg!=30&&tg!=31&&tg!=56&&tg!=57&&tg!=0))shape=false;
            if((au==29&&i(110+e)>=-1&&i(110+e)<=4)||au==99||(tg==1&&(au==31||au==49||au==124)&&i(80+e)>=0)||(tg==1&&au==22&&(i(110+e)&1)&&i(80+e)>=0)||(trackerSpell&&au==44)||(prowlSpell&&au==16)||(presenceSpell&&(au==79||au==142||au==138))||
               (!creatureCaster&&d.spellFamily==4&&(((d.spellFamilyFlags[0]&0x1000u)&&tg==1&&(au==51||au==150))||((d.spellFamilyFlags[1]&0x80u)&&au==230))))any=true;
        }
        // Immunity and damage / healing percentages (Divine Shield, Divine
        // Protection, Hand of Protection, Avenging Wrath, Shield Wall) admit a
        // class buff only when every aura in it is one the realm models, so a
        // spell is never imported with its penalty and without its benefit
        // (Divine Plea, Aspect of the Viper: their mana regeneration is not).
        if(!any&&shape) {
            bool modelled=true,percentage=false;
            for(uint32_t e=0;e<3;++e)if(u(71+e)) {
                const auto au=u(95+e);const int32_t misc=i(110+e),amount=i(80+e)+int32_t(u(74+e)?1:0);
                if(au==39&&misc>0&&misc<=127)percentage=true;
                else if(u(86+e)==1&&(au==79||au==87||au==136||au==118)&&misc==127)percentage=true;
                else if(au==25&&u(86+e)!=1){} // Hand of Protection's pacify on its target
                else if(au==108&&!amount){} // Avenging Wrath's empty spell modifier
                else if(au==12&&u(86+e)==1&&d.id==kLocalIceBlockSpell){} // Ice Block's own stun (localPlayerControl)
                else if(au==77&&localControlMechanic(misc))percentage=true; // Fear Ward: mechanic immunity
                else if(au==77&&misc==11&&d.spellFamily==10)percentage=true; // Hand of Freedom: snares
                else if(au==149&&u(86+e)==1&&misc==127&&amount>0&&amount<=100)percentage=true; // Barkskin: no pushback
                else if(au==42&&inertProc){} // Barkskin's proc: chance 0, no spell_proc row
                else if(au==4&&!amount&&d.id==48792){} // Icebound Fortitude's empty dummy
                else if((au==69||au==267)&&d.id==48707&&misc==126&&amount>0&&amount<=100)percentage=true; // Anti-Magic Shell
                else if((au==192||au==65)&&(d.id==2825||d.id==32182)&&amount>0&&amount<=100)percentage=true; // Bloodlust, Heroism
                else if(au==61&&(d.id==2825||d.id==32182)){} // their MOD_SCALE: the model's size only
                else if(au==140&&u(86+e)==1&&!misc&&amount>0&&amount<=100)percentage=true; // Rapid Fire
                else if(au==66&&u(86+e)==1&&d.spellFamily==9)percentage=true; // Feign Death
                else if(au==137&&misc==-1&&amount>0&&amount<=100)percentage=true; // Blessing of Kings
                else if(au==166&&!amount){} // Kings' empty attack power percent
                else if(au==85&&!misc&&amount>0&&amount<=100000)percentage=true; // Blessing of Wisdom: mana per 5
                else if(au==21&&u(86+e)==1&&!misc&&amount>0&&amount<=100&&u(98+e)>=1000&&u(98+e)<5000)percentage=true; // Viper: mana %
                // Divine Plea: its SPELLMOD_DAMAGE on the paladin's heals (Holy Light,
                // Flash of Light, Holy Shock: its class mask), as healing done.
                else if(au==108&&d.id==54428&&u(86+e)==1&&!misc&&amount<0&&amount>-100)percentage=true;
                else if(au==103&&u(86+e)==1&&!misc&&amount<0)percentage=true; // Fade
                else if(au==227&&salvation)percentage=true; // Hand of Salvation: threat down each second
                else if(au==87&&salvation&&misc==127&&!amount){} // its empty damage-taken hook (the glyph's)
                else if(au==134&&u(86+e)==1&&!misc&&amount>0&&amount<=100&&d.spellFamily==3)percentage=true; // Mage Armor: regeneration while casting
                else if((au==22||au==143)&&misc>0&&misc<=126&&!(misc&1)&&amount>0&&amount<=1000)percentage=true; // resistances (Shadow Protection, Mage Armor)
                else if(au==13&&u(86+e)==1&&misc==1&&amount>0&&amount<=10000&&d.spellFamily==7)percentage=true; // Tiger's Fury
                else if(au==10&&u(86+e)==1&&misc>0&&misc<=127&&amount>0&&amount<=1000)percentage=true; // Righteous Fury: school threat
                else if(au==28&&spellReflection)percentage=true; // Spell Reflection
                else if(au==4&&retaliation&&!amount)percentage=true; // Retaliation
                else if(au==107&&recklessness&&misc==7&&amount>0&&amount<=100)percentage=true; // Recklessness
                else if(au==24&&!misc&&d.spellFamily==7&&amount>0&&amount<=1000&&u(98+e)>=1000&&u(98+e)<=d.durationMs)percentage=true; // Innervate
                else modelled=false;
            }
            any=percentage&&modelled;
        }
        classBuffSpell=any&&shape&&((!u(spell335::ProcFlags)&&!u(spell335::ProcCharges))||untilCancelled||inertProc||immunityCharge||innerFire||chillArmor||spellReflection||retaliation||recklessness);
        if(classBuffSpell&&immunityCharge)d.classBuffImmunityCharge=true;
    }
    // A ground area: SPELL_EFFECT_PERSISTENT_AREA_AURA (27) at the destination
    // (28) or the caster (18), with a fixed radius.
    if(!creatureCaster)for(uint32_t e=0;e<3&&!d.groundRadius;++e)if(u(71+e)==27&&(u(86+e)==28||u(86+e)==18||u(86+e)==16)) {
        const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+e));
        const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
        if(std::isfinite(r)&&r>0&&r<=30){d.groundRadius=r;d.groundAtCaster=u(86+e)==18;}
    }
    if(creatureCaster){decodeCreatureEffects(t,row,d,unavailable);harm=!d.npcPositive;healing=d.npcPositive;}
    else for(uint32_t effect=0;effect<3;++effect) {
        const auto type=u(71+effect); if(!type) continue;
        if(molten||ward){buff=true;buffTarget=1;continue;}
        if(summonPet)continue; // Reviewed controlled-summon shape; verified above.
        // Reviewed raid area-aura shape; every carried effect is verified
        // above. Its implicit target is the caster on all three effects, so
        // the cast itself lands on the paladin and nowhere else.
        if(areaAura){buffTarget=kSourceTargetUnitCaster;continue;}
        if(d.formId||formBoost||formResource)continue; // Exact outer profile verified above; local form rules own its effects.
        if(bloodthirst && effect==1){harm=true;continue;} // Reviewed destination dummy arms internal aura.
        if(totemSummon)continue; // The creature it puts down is the totem rules' (totems.json).
        if(deathItemChannel&&type==6&&(u(95+effect)==86||(u(95+effect)==42&&u(86+effect)==1))) {
            if(u(95+effect)==86)d.soulShardOnKill=true;
            continue;
        }
        if(d.groundRadius&&type==27) {
            // The area's own periodic damage; its dummy, snare and slow ride along.
            // Death and Decay ticks through a periodic dummy (aura 226) its script turns into shadow damage.
            if((u(95+effect)==3||(u(95+effect)==226&&d.spellFamily==15))&&d.durationMs&&u(98+effect)>0&&u(98+effect)<=d.durationMs&&!d.periodicDamage) {
                const int32_t gb=i(80+effect),gd=i(74+effect);
                if(gb>=0&&gb<100000&&gd>=0&&gd<100000){d.periodicEffectSlot=uint8_t(effect);d.periodicDamage=uint32_t(gb+1);
                    d.periodicDamageMax=uint32_t(gb+std::max(1,gd));d.periodicIntervalMs=u(98+effect);harm=true;}
            }
            continue;
        }
        // SPELL_AURA_MOD_DECREASE_SPEED on one hostile target (Hamstring, Wing
        // Clip, Concussive Shot, Curse of Exhaustion): its slow percentage
        // through the creature snare the Frost spells use.
        if(confuseControl&&type==6&&u(95+effect)==33&&u(86+effect)==6&&!u(89+effect)&&!u(116+effect))continue;
        if(confuseControl&&type==6&&u(95+effect)==56&&effect==1){d.controlTransformEntry=u(110+effect);continue;} // Polymorph's model
        if(fearControl&&effect==1)continue; // Fear's run speed (verified above).
        // Steady Shot: its second effect is the dazed-target bonus, a DUMMY with
        // no target the hunter branch of EffectSchoolDMG reads; creatures are
        // never dazed by a hunter here, so only the weapon terms are taken.
        if(!creatureCaster&&d.spellFamily==9&&(d.spellFamilyFlags[1]&0x1u)&&effect==1&&type==3&&!u(87)&&u(71)==2){d.steadyShot=true;continue;}
        // Strangulate: an empty dummy aura (no script at the pin) beside the silence.
        if(!creatureCaster&&d.id==47476&&type==6&&u(95+effect)==4&&!i(80+effect)&&!u(74+effect))continue;
        // Seal of Righteousness: two dummy auras and an empty spell modifier on
        // the paladin; the script does the work (spell_pal_seal_of_righteousness).
        // Effect 2's amount names the judgement it unleashes.
        if(!creatureCaster&&d.spellFamily==10&&(d.id==21084||d.id==20154)) {
            if(type!=6||u(86+effect)!=1)unavailable("Unreviewed seal row");
            else if(u(95+effect)==4&&effect==2&&u(80+effect)+1==20187){ // Judgement of Righteousness: 1 + 0.2 AP (spell_bonus_data)
                d.sealJudgementSpell=20187;d.sealJudgementBase=1;d.sealJudgementApPer100k=20000;}
            if(!d.sealOfRighteousness){d.sealOfRighteousness=true;d.classBuff=true;buff=true;buffTarget=1;
                if(!d.sealJudgementSpell){d.sealJudgementSpell=54158;d.sealJudgementBase=1;d.sealJudgementApPer100k=16000;}} // the generic judgement: 0.16 AP
            continue;
        }
        if(sealKind) {
            if(type==6&&u(86+effect)==1&&(u(95+effect)==42||(u(95+effect)==108&&i(80+effect)+int32_t(u(74+effect))==0))){
                if(!d.sealKind){d.sealKind=sealKind;d.classBuff=true;buff=true;buffTarget=1;
                    d.sealJudgementSpell=54158;d.sealJudgementBase=1;d.sealJudgementApPer100k=16000;}} // spell_bonus_data 54158: 0.16 AP
            else unavailable("Unreviewed seal row");
            continue;
        }
        // A judgement: its script effect is spell_pal_judgement's.
        if(!creatureCaster&&d.spellFamily==10&&(d.id==20271||d.id==53408||d.id==53407)) {
            if(type==77&&u(86+effect)==6){d.judgementDebuff=d.id==20271?20185u:d.id==53408?20186u:20184u;harm=true;}
            else unavailable("Unreviewed judgement row");
            continue;
        }
        // Fire Nova: its dummy is the script's; the damage, school and radius are
        // the triggered rank's (one SCHOOL_DAMAGE at the totem's enemy area).
        if(!creatureCaster&&d.spellFamily==11&&localFireNovaTriggered(d.id)&&effect==0&&type==3) {
            const auto child=ClientSpellTables::lookup(t.spellIndex,localFireNovaTriggered(d.id));
            const auto cu=[&](uint32_t col){return t.spells->getUInt32(uint32_t(child),col);};
            const auto ci=[&](uint32_t col){return t.spells->getInt32(uint32_t(child),col);};
            const auto radius=child>=0?ClientSpellTables::lookup(t.radiusIndex,cu(92)):-1;
            const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
            if(child<0||cu(71)!=2||cu(86)!=87||cu(89)!=16||cu(72)||ci(80)<0||ci(74)<1||!std::isfinite(r)||r<=0||r>30||!cu(225))unavailable("Unreviewed Fire Nova row");
            else {d.novaLow=uint32_t(ci(80)+1);d.novaHigh=uint32_t(ci(80)+ci(74));d.novaSchool=uint8_t(cu(225));d.novaRadius=r;harm=true;}
            continue;
        }
        // Empower Rune Weapon: two ACTIVATE_RUNE effects whose 47568 branch
        // refreshes every rune, and a trigger of 53258, which no Spell.dbc at
        // the pin carries (so the reference casts nothing there either).
        if(!creatureCaster&&d.id==47568&&d.spellFamily==15) {
            if(type==146&&u(86+effect)==1){d.runeRefresh=true;buff=true;buffTarget=1;}
            else if(!(type==64&&ClientSpellTables::lookup(t.spellIndex,u(116+effect))<0))unavailable("Unreviewed Empower Rune Weapon row");
            continue;
        }
        // Raise Dead: its three effects are the script's (spell_dk_raise_dead);
        // the guardian, its lifetime and the reagent come from 46585 and 48289.
        if(!creatureCaster&&d.id==46584&&d.spellFamily==15) {
            if(effect==1&&type==77&&!d.raiseDeadEntry) {
                const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+effect));
                const auto guardian=ClientSpellTables::lookup(t.spellIndex,46585),reagent=ClientSpellTables::lookup(t.spellIndex,48289);
                const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
                const auto gu=[&](uint32_t col){return t.spells->getUInt32(uint32_t(guardian),col);};
                const auto durationRow=guardian>=0?ClientSpellTables::lookup(t.durationIndex,gu(40)):-1;
                const int32_t duration=durationRow>=0?t.durations->getInt32(uint32_t(durationRow),1):0;
                if(guardian<0||reagent<0||gu(71)!=28||!gu(110)||duration<=0||duration>600000||!std::isfinite(r)||r<=0||r>50)
                    unavailable("Unreviewed Raise Dead rows");
                else {d.raiseDeadEntry=gu(110);d.raiseDeadDurationMs=uint32_t(duration);d.raiseDeadRadius=r;
                      d.raiseDeadReagent=t.spells->getUInt32(uint32_t(reagent),52);}
            }
            continue;
        }
        // Pestilence: two dummies and the script effect around the target; the
        // script (spell_dk_pestilence) is the spread, its radius effect 2's.
        if(!creatureCaster&&d.id==50842&&d.spellFamily==15) {
            if(effect==2&&type==77) {
                const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+effect));
                const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
                if(std::isfinite(r)&&r>0&&r<=30){d.pestilenceRadius=r;harm=true;}else unavailable("Pestilence radius record missing");
            }
            if(type==3||type==77)continue;
        }
        // Corruption ranks 1-3 and 5: an empty DUMMY beside the shadow DoT, with
        // no target, no amount and no handler at the pin (ranks 4 and 6+ do not
        // carry it). It does nothing.
        if(!creatureCaster&&d.spellFamily==5&&effect==1&&type==3&&!u(87)&&!u(90)&&!i(81)&&!u(75)&&!u(117)&&!u(73)&&
           u(71)==6&&u(95)==3&&u(86)==6)continue;
        // A warlock curse on one hostile creature (Curse of Weakness, of the
        // Elements, of Tongues): fixed stat auras that land as one creature
        // aura of the warlock's. MOD_ATTACK_POWER (99) and armor
        // MOD_RESISTANCE_PCT (101) down, MOD_RESISTANCE (22) down and
        // MOD_DAMAGE_PERCENT_TAKEN (87) up on its schools, HASTE_SPELLS (216)
        // down; Tongues' MOD_LANGUAGE (75) has nothing to change on a creature.
        if(!creatureCaster&&d.spellFamily==5&&d.dispelType==2&&type==6&&u(86+effect)==6&&!u(89+effect)&&!u(116+effect)&&
           u(74+effect)<=1&&f(77+effect)==0&&d.durationMs&&d.durationMs<=600000&&!u(spell335::ProcFlags)&&!u(spell335::ProcCharges)) {
            const auto aura=u(95+effect);const auto misc=u(110+effect);
            const int32_t amount=i(80+effect)+int32_t(u(74+effect));
            bool taken=true;
            if(aura==75)continue;
            if(aura==99&&amount<0&&amount>=-100000&&!d.targetDebuffAttackPower)d.targetDebuffAttackPower=amount;
            else if(aura==101&&misc==1&&amount<0&&amount>-100&&!d.targetDebuffArmorPct)d.targetDebuffArmorPct=uint8_t(-amount);
            else if(aura==22&&misc>=2&&misc<=126&&!(misc&1)&&amount<0&&amount>=-100000&&!d.targetDebuffResistance){d.targetDebuffResistance=amount;d.targetDebuffResistanceSchool=uint8_t(misc);}
            else if(aura==87&&misc>=2&&misc<=126&&!(misc&1)&&amount>0&&amount<=100&&!d.targetDebuffDamageTakenPct){d.targetDebuffDamageTakenPct=amount;d.targetDebuffDamageTakenSchool=uint8_t(misc);}
            else if(aura==216&&amount<0&&amount>-100&&!d.targetDebuffCastSpeedPct)d.targetDebuffCastSpeedPct=amount;
            else taken=false;
            if(taken){d.targetDebuffEffectMask|=uint8_t(1u<<effect);harm=true;continue;}
        }
        // Hunter's Mark: MOD_STALKED (68, the creature seen; nothing to change
        // here) and RANGED_ATTACK_POWER_ATTACKER_BONUS (127) on one hostile
        // creature, the hunter's creature aura; one creature per hunter
        // (SPELL_ATTR5_SINGLE_TARGET_SPELL).
        if(!creatureCaster&&d.spellFamily==9&&type==6&&u(86+effect)==6&&!u(89+effect)&&!u(116+effect)&&
           (u(95+effect)==68||u(95+effect)==127||(u(95+effect)==54&&i(80+effect)+int32_t(u(74+effect))<0&&i(80+effect)+int32_t(u(74+effect))>-100))&&
           u(74+effect)<=1&&f(77+effect)==0&&d.durationMs&&d.durationMs<=600000&&!u(spell335::ProcFlags)) {
            const int32_t amount=i(80+effect)+int32_t(u(74+effect));
            if(u(95+effect)==127&&amount>0&&amount<=100000)d.targetDebuffRangedAttackerAp=amount;
            if(u(95+effect)==54)d.targetDebuffHitChancePct=int8_t(amount); // Scorpid Sting
            d.targetDebuffEffectMask|=uint8_t(1u<<effect);d.controlSingleTarget=(u(9)&0x20u)!=0;harm=true;continue;
        }
        // Thunder Clap: its MOD_MELEE_HASTE rider on the creatures its area hit.
        if(!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[0]&0x80u)&&type==6&&u(95+effect)==138&&
           u(86+effect)==22&&(u(89+effect)==15||!u(89+effect))&&!u(116+effect)&&u(74+effect)<=1&&
           i(80+effect)+1<0&&i(80+effect)+1>-100&&d.durationMs&&d.durationMs<=600000&&!d.targetDebuffMeleeHastePct) {
            d.targetDebuffMeleeHastePct=int8_t(i(80+effect)+1);d.targetDebuffMeleeHasteSlot=uint8_t(effect);
            d.targetDebuffEffectMask|=uint8_t(1u<<effect);harm=true;continue;
        }
        // Demoralizing Shout / Roar: area attack-power debuff on enemies around caster
        if(!creatureCaster&&((d.spellFamily==4&&(d.spellFamilyFlags[0]&0x20000u))||(d.spellFamily==7&&d.spellFamilyFlags[0]==0x8u&&!d.spellFamilyFlags[1]))&&
           type==6&&u(95+effect)==99&&u(86+effect)==22&&(u(89+effect)==15||!u(89+effect))&&!u(116+effect)&&
           d.durationMs&&d.durationMs<=600000) {
            const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+effect));
            const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):10.0f;
            d.areaRadius=r>0?r:10.0f;
            d.targetDebuffAttackPower=i(80+effect)+1;
            d.targetDebuffEffectMask|=uint8_t(1u<<effect);
            harm=true;continue;
        }
        // Disarm: remove weapons / parry on hostile single target
        if(!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[0]&0x200u)&&type==6&&!u(116+effect)) {
            if(effect==0&&u(95+effect)==67&&u(86+effect)==6&&!u(89+effect)) {
                d.disarm=true;harm=true;continue;
            }
            if(effect>0&&(u(95+effect)==87||u(95+effect)==278))continue;
        }
        // Dismantle: the rogue's Disarm, its off-hand and ranged disarms beside it
        // (a creature's swing is its main hand's).
        if(!creatureCaster&&d.spellFamily==8&&type==6&&u(86+effect)==6&&!u(89+effect)&&!u(116+effect)&&
           (u(95+effect)==67||u(95+effect)==254||u(95+effect)==278)&&u(95)==278&&u(96)==254&&u(97)==67) {
            if(u(95+effect)==67){d.disarm=true;harm=true;}
            continue;
        }
        // Shield Block: 100% block chance + 100% block value self buff
        if(!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[0]&0x1000u)&&type==6&&u(86+effect)==1&&!u(89+effect)&&
           (u(95+effect)==51||u(95+effect)==150)) {
            d.classBuff=true;buff=true;buffTarget=1;continue;
        }
        // Whirlwind: normalized weapon damage to enemies within 8 yards
        if(!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[1]&0x4u)) {
            if(effect==0&&type==121&&u(86+effect)==22&&u(89+effect)==15) {
                directSlot(effect);d.weaponDamage=true;d.normalizedWeapon=true;d.areaRadius=8.0f;harm=true;continue;
            }
            if(effect==1&&type==64)continue;
        }
        // Sunder Armor: SPELL_EFFECT_TRIGGER_SPELL of 58567 on the target, an
        // armor MOD_RESISTANCE_PCT stacking to its StackAmount beside an empty
        // SPELL_EFFECT_THREAT; its threat is the cast's spell_threat row
        // (localSpellInitialThreat).
        if(!creatureCaster&&d.spellFamily==4&&type==64&&effect==0&&u(86)==6&&!u(89)&&!u(72)&&!u(73)&&u(116)&&!d.durationMs) {
            const auto child=ClientSpellTables::lookup(t.spellIndex,u(116));
            const auto cu=[&](uint32_t col){return child>=0?t.spells->getUInt32(uint32_t(child),col):0u;};
            const auto ci=[&](uint32_t col){return child>=0?t.spells->getInt32(uint32_t(child),col):0;};
            const auto durationRow=child>=0?ClientSpellTables::lookup(t.durationIndex,cu(40)):-1;
            const int32_t duration=durationRow>=0?t.durations->getInt32(uint32_t(durationRow),1):0;
            const int32_t amount=ci(80)+(cu(74)?1:0);
            if(child>=0&&cu(71)==6&&cu(95)==101&&cu(110)==1&&cu(86)==6&&!cu(89)&&amount<0&&amount>-100&&cu(72)==63&&cu(87)==6&&!ci(81)&&!cu(73)&&
               cu(spell335::StackAmount)>1&&cu(spell335::StackAmount)<=10&&-amount*int32_t(cu(spell335::StackAmount))<100&&duration>0&&duration<=600000&&
               !cu(spell335::ProcFlags)&&d.id==7386) {
                d.armorDebuffPct=uint8_t(-amount);d.armorDebuffStackMax=uint8_t(cu(spell335::StackAmount));d.durationMs=uint32_t(duration);
                harm=true;continue;
            }
        }
        // Faerie Fire: an empty MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE (186),
        // the Improved Faerie Fire talent's hook, beside its armor reduction.
        if(!creatureCaster&&d.spellFamily==7&&type==6&&u(95+effect)==186&&u(86+effect)==6&&!u(89+effect)&&i(80+effect)+int32_t(u(74+effect))==0&&
           u(71)==6&&u(95)==101&&u(110)==1&&u(86)==6){d.armorDebuffMinor=true;continue;}
        // SPELL_AURA_MOD_RESISTANCE_PCT on the armor of one hostile target
        // (Expose Armor): a fixed percentage for the aura's duration, which a
        // finisher draws from its combo points.
        if(!creatureCaster&&type==6&&u(95+effect)==101&&u(110+effect)==1&&u(86+effect)==6&&!u(89+effect)&&!u(116+effect)&&
           !d.armorDebuffPct&&u(74+effect)<=1&&f(77+effect)==0&&i(80+effect)+int32_t(u(74+effect))<0&&i(80+effect)+int32_t(u(74+effect))>-100&&
           (d.durationMs||d.comboDurationMaxMs)&&std::max(d.durationMs,d.comboDurationMaxMs)<=600000&&!u(spell335::ProcFlags)&&!u(spell335::ProcCharges)) {
            d.armorDebuffPct=uint8_t(-(i(80+effect)+int32_t(u(74+effect))));d.armorDebuffEffectSlot=uint8_t(effect);harm=true;continue;
        }
        if(!creatureCaster&&!snare&&type==6&&u(95+effect)==33&&u(86+effect)==6&&!u(89+effect)&&!d.snarePercent&&
           i(80+effect)<-1&&i(80+effect)>=-100&&d.durationMs&&d.durationMs<=600000&&!u(116+effect)&&u(46)!=1) { // range 1: a talent's triggered daze
            d.snarePercent=uint8_t(-(i(80+effect)+1));harm=true;continue;
        }
        // A death knight strike's script dummy (Death Strike's heal, Obliterate's
        // disease consumption) and Chains of Ice's own dummy and fading slow.
        if(!creatureCaster&&d.spellFamily==15&&type==3&&(d.weaponDamage||u(86+effect)==1))continue;
        if(!creatureCaster&&d.spellFamily==15&&d.snarePercent&&type==6&&u(95+effect)==226)continue;
        // A generic finisher's script dummies (its attack-power terms) are set
        // aside; Slice and Dice's melee haste lands on the rogue as a class buff.
        if(!combo&&d.comboFinisher&&type==3)continue;
        if(!combo&&d.comboFinisher&&type==6&&u(95+effect)==87&&u(86+effect)==6&&!i(80+effect)&&!u(74+effect))continue;
        if(!combo&&d.comboFinisher&&type==6&&u(95+effect)==138&&u(86+effect)==1&&!u(89+effect)&&i(80+effect)>=0&&i(80+effect)<100) {
            d.classBuff=true;d.classBuffMeleeHastePct=i(80+effect)+1;buff=true;buffTarget=1;continue;
        }
        // SPELL_EFFECT_ADD_COMBO_POINTS on the enemy of an opener outside the
        // reviewed combo profiles (Ambush, Garrote, Cheap Shot).
        if(!combo&&type==80&&u(86+effect)==6&&!u(89+effect)&&!u(116+effect)&&u(74+effect)<=1) {
            const int32_t points=i(80+effect)+1;
            if(points>=1&&points<=5){d.comboGain=uint8_t(points);harm=true;continue;}
        }
        if(classBuffSpell) {
            const auto au=u(95+effect),tg=u(86+effect);const int32_t amount=i(80+effect)+1,misc=i(110+effect);
            if(au==44&&tg==1&&misc>=1&&misc<=12)d.trackCreatureMask|=1u<<(misc-1);
            if(presenceSpell&&au==87&&tg==1&&misc==127&&amount<0&&amount>-100)d.classBuffDamageTakenPct+=amount; // Frost Presence
            if(au==39&&misc>0&&misc<=127)d.classBuffSchoolImmunity|=uint8_t(misc); // Divine Shield, Hand of Protection
            if(au==77&&localControlMechanic(misc))d.classBuffMechanicImmunity|=1u<<misc; // Fear Ward
            if(au==77&&misc==11&&d.spellFamily==10)d.classBuffMechanicImmunity|=1u<<11; // Hand of Freedom
            if(au==149&&tg==1&&misc==127&&amount>0&&amount<=100)d.classBuffPushbackPct=uint8_t(amount); // Barkskin
            if((d.id==2825||d.id==32182)&&amount>0&&amount<=100){if(au==192)d.classBuffMeleeRangedHastePct=amount;else if(au==65)d.classBuffCastSpeedPct=amount;}
            if(au==140&&tg==1&&!misc&&amount>0&&amount<=100)d.classBuffRangedHastePct=amount; // Rapid Fire
            if(au==66&&tg==1&&d.spellFamily==9)d.classBuffFeignDeath=true; // Feign Death
            if(au==137&&misc==-1&&amount>0&&amount<=100)d.classBuffStatPct=amount; // Blessing of Kings
            if(innerFire&&au==22)d.classBuffHitCharges=uint8_t(u(spell335::ProcCharges)); // Inner Fire
            if(au==85&&!misc&&amount>0&&amount<=100000&&!d.manaPer5)d.manaPer5=uint32_t(amount); // Blessing of Wisdom
            if(d.id==48707&&misc==126&&amount>0&&amount<=100){ // Anti-Magic Shell: its absorb pool and aura immunity
                if(au==69){d.magicShellAbsorbPct=uint8_t(amount);d.buffAbsorb=1;d.absorbSchoolMask=126;}
                else if(au==267){d.magicShellHealthPct=uint8_t(amount);d.classBuffAuraImmunitySchool=126;}
            }
            if(au==21&&tg==1&&!misc&&amount>0&&amount<=100&&u(98+effect)>=1000&&u(98+effect)<5000){d.classBuffManaPct=uint8_t(amount);d.classBuffManaIntervalMs=u(98+effect);}
            // Divine Shield's damage done and Divine Protection's damage taken.
            if(!presenceSpell&&tg==1&&misc==127&&amount<0&&amount>-100){if(au==79)d.classBuffDamagePct+=amount;else if(au==87)d.classBuffDamageTakenPct+=amount;}
            if(au==136&&tg==1&&misc==127&&amount>0&&amount<=1000)d.classBuffHealingDonePct+=amount; // Avenging Wrath
            if(au==108&&d.id==54428&&tg==1&&!misc&&amount<0&&amount>-100)d.classBuffHealingDonePct+=amount; // Divine Plea
            if(au==13&&tg==1&&misc==1&&amount>0&&amount<=10000&&d.spellFamily==7)d.classBuffWeaponDamageFlat=amount; // Tiger's Fury
            if(au==134&&tg==1&&!misc&&amount>0&&amount<=100&&d.spellFamily==3)d.classBuffManaRegenInterruptPct=uint8_t(amount); // Mage Armor
            if(au==227&&salvation){const auto r=ClientSpellTables::lookup(t.spellIndex,53055);
                const int32_t pct=t.spells->getInt32(uint32_t(r),80)+int32_t(t.spells->getUInt32(uint32_t(r),74));
                if(pct<0&&pct>-100){d.classBuffThreatTickPct=int8_t(pct);d.classBuffThreatTickMs=u(98+effect);}}
            if(au==103&&tg==1&&!misc&&amount<0)d.classBuffThreatReduction=uint32_t(std::min<int64_t>(-int64_t(amount),1000000000)); // Fade
            if(au==10&&tg==1&&misc>0&&misc<=127&&amount>0&&amount<=1000){d.classBuffThreatPct=int16_t(amount);d.classBuffThreatSchool=uint8_t(misc);} // Righteous Fury
            if(au==28&&spellReflection&&amount>0&&amount<=100)d.classBuffReflectPct=uint8_t(amount); // Spell Reflection
            if(au==4&&retaliation){d.classBuffRetaliationSpell=20240;d.classBuffHitCharges=uint8_t(u(spell335::ProcCharges));} // Retaliation
            if(recklessness&&au==107&&misc==7&&amount>0&&amount<=100){d.classBuffSpecialCritPct=uint8_t(amount);d.classBuffSpecialCritMask={u(122),u(123)};d.classBuffHitCharges=uint8_t(u(spell335::ProcCharges));}
            if(recklessness&&au==87&&misc==127&&amount>0&&amount<=100)d.classBuffDamageTakenPct+=amount; // Recklessness
            if(au==24&&!misc&&d.spellFamily==7&&amount>0&&amount<=1000&&u(98+effect)>=1000){d.innervatePct=uint16_t(amount);d.innervateIntervalMs=u(98+effect);} // Innervate
            if(amount>0&&amount<=100000) {
                if(au==29){for(int k=0;k<5;++k)if(misc==-1||misc==k)d.classBuffStats[size_t(k)]+=amount;}
                else if(au==99)d.classBuffAttackPower+=amount;
                else if(au==22&&(misc&1))d.classBuffArmor+=amount;
                else if((au==22||au==143)&&misc>0&&misc<=126&&!(misc&1)&&amount<=1000&&!d.classBuffResistance){
                    d.classBuffResistance=int16_t(amount);d.classBuffResistanceSchool=uint8_t(misc);d.classBuffResistanceExclusive=au==143;}
                else if(au==34||(!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[1]&0x80u)&&au==230))d.classBuffHealth+=amount;
                else if(au==31&&tg==1)d.classBuffSpeedPct+=amount;
                else if(au==49&&tg==1)d.classBuffDodgePct+=amount;
                else if(au==124&&tg==1)d.classBuffRangedAttackPower+=amount;
                else if(au==138&&tg==1&&amount<=100)d.classBuffMeleeHastePct+=amount;
                else if(au==142&&tg==1&&misc==1&&amount<=1000)d.classBuffArmorPct+=amount;
                else if(au==79&&tg==1&&misc==127&&amount<=1000)d.classBuffDamagePct+=amount;
                else if(au==118&&tg==1&&misc==127&&!presenceSpell&&amount<=1000)d.classBuffHealingTakenPct+=amount; // Demon Skin/Armor
            }
            d.classBuff=true;buff=true;
            buffTarget=(tg==21||tg==25)?tg:(buffTarget?buffTarget:1);
            continue;
        }
        auto secondary=u(89+effect);
        // A heal at TARGET_UNIT_TARGET_CHAINHEAL_ALLY with no chain targets
        // (Healing Wave ranks 1-10) heals one ally, as TARGET_UNIT_TARGET_ALLY.
        const auto target=(u(86+effect)==45&&!chainHeal&&type==10&&u(104+effect)<=1&&!secondary)?21u:u(86+effect);
        // A shaman weapon imbue: its ENCHANT_ITEM_TEMPORARY becomes the shaman's
        // own aura on the main-hand weapon for the enchant's duration.
        if(!creatureCaster&&d.spellFamily==11)if(const auto* imbue=localWeaponImbue(d.id)) {
            if(type==54){d.imbueKind=imbue->kind;d.durationMs=imbue->seconds*1000;d.classBuff=true;buff=true;buffTarget=1;}
            else if(type)unavailable("Unreviewed weapon imbue effect");
            continue;
        }
        if(!creatureCaster&&d.id==8129&&effect==0&&type==62&&u(86)==6&&!u(89)&&!u(110)&&!u(72)&&!u(73)&&i(80)>=0&&i(80)<100&&
           std::isfinite(f(101))&&f(101)>0&&f(101)<=10){d.manaBurnPct=uint8_t(i(80)+1);d.manaBurnDamagePct=uint8_t(std::lround(f(101)*100.f));harm=true;continue;}
        if(layOnHands) {
            if(effect==0){d.layOnHands=true;healing=true;}
            else d.energizeMana=uint32_t(i(81)+1);
            continue;
        }
        if(druidEnrage) {
            if(effect==0){d.periodicRage=uint8_t((i(80)+1)/10);d.periodicRageMs=u(98);d.classBuff=true;d.classBuffEnrage=true;buff=true;buffTarget=1;}
            else if(effect==1)d.energizeRage=uint8_t((i(81)+1)/10);
            continue;
        }
        if(vanishSpell) {
            if(effect==2){d.vanish=true;buff=true;buffTarget=1;}
            continue;
        }
        if(bloodrage) {
            if(effect==0&&i(80)>=0&&i(80)<1000)d.energizeRage=uint8_t((i(80)+1)/10); // tenths of rage
            else if(effect==1) {
                const auto child=ClientSpellTables::lookup(t.spellIndex,u(117));
                const auto cu=[&](uint32_t col){return child>=0?t.spells->getUInt32(uint32_t(child),col):0u;};
                const auto ci=[&](uint32_t col){return child>=0?t.spells->getInt32(uint32_t(child),col):0;};
                const auto durationRow=child>=0?ClientSpellTables::lookup(t.durationIndex,cu(40)):-1;
                const int32_t duration=durationRow>=0?t.durations->getInt32(uint32_t(durationRow),1):0;
                const int32_t tick=ci(80)+(cu(74)?1:0);
                if(child>=0&&cu(71)==6&&cu(95)==24&&cu(86)==1&&cu(110)==1&&cu(98)>=1000&&tick>=10&&tick<1000&&
                   cu(72)==6&&cu(96)==94&&cu(87)==1&&!cu(73)&&duration>0&&duration<=60000) {
                    d.periodicRage=uint8_t(tick/10);d.periodicRageMs=cu(98);d.durationMs=uint32_t(duration);
                    d.classBuff=true;buff=true;buffTarget=1;
                } else unavailable("Unreviewed rage-over-time aura");
            }
            if(!d.energizeRage&&effect==0)unavailable("Unreviewed rage energize");
            continue;
        }
        // Pounce: its bleed is the triggered spell's own DoT (see riderOk).
        if(!creatureCaster&&d.spellFamily==7&&effect==1&&type==64&&u(86+1)==6&&!u(89+1)&&u(117)&&u(71)==6&&u(95)==12&&u(86)==6){d.triggerDotSpell=u(117);harm=true;continue;}
        if(d.controlImmune&&effect>0&&type==6&&(u(95+effect)==39||u(95+effect)==118))continue; // Banish's immunity (riderOk)
        // Shadowfiend: SUMMON of its guardian and a server-side scaling trigger;
        // the guardian's Mana Leech (34650, ENERGIZE_PCT on its owner) is its hits'.
        if(!creatureCaster&&d.id==34433&&d.durationMs&&d.durationMs<=60000) {
            if(effect==0&&type==28&&u(110)) {
                const auto leech=ClientSpellTables::lookup(t.spellIndex,34650);
                const auto lu=[&](uint32_t col){return leech>=0?t.spells->getUInt32(uint32_t(leech),col):0u;};
                const int32_t pct=leech>=0?t.spells->getInt32(uint32_t(leech),80)+1:0;
                if(lu(71)==137&&lu(86)==25&&pct>0&&pct<=100){d.guardianEntry=u(110);d.guardianDurationMs=d.durationMs;d.guardianManaPct=uint8_t(pct);harm=true;}
                else unavailable("Unreviewed Shadowfiend shape");
            }
            continue;
        }
        if(!creatureCaster&&d.id==48743&&u(71)==3&&u(86)==1&&u(72)==1&&u(87)==18&&u(90)==31&&u(73)==10&&u(88)==1) {
            if(effect==2&&i(82)>=0&&i(82)<100)d.deathPactPct=uint8_t(i(82)+1);
            healing=true;continue; // the dummy, the pet's instakill and the heal: spell_dk_death_pact
        }
        // Abolish Poison / Disease: a PERIODIC_TRIGGER_SPELL of a lone DISPEL of
        // the same type on the friendly target, beside that DISPEL at once.
        if(!creatureCaster&&(d.spellFamily==6||d.spellFamily==7)&&u(71)==6&&u(95)==23&&u(86)==21&&u(98)>=1000&&u(116)&&
           u(72)==38&&u(87)==21&&(u(111)==3||u(111)==4)&&!u(73)&&d.durationMs&&d.durationMs<=60000) {
            if(effect==0){d.classBuffDispelMask=uint8_t(1u<<u(111));d.classBuffDispelIntervalMs=u(98);d.classBuff=true;buff=true;buffTarget=21;}
            continue;
        }
        if(cloakSpell) {
            if(effect==0){const int32_t amount=i(80)+int32_t(u(74));
                if(amount<0&&amount>=-100){d.classBuffSpellHitTakenPct=int8_t(amount);d.classBuffSpellHitTakenSchool=126;d.classBuff=true;buff=true;buffTarget=1;}
                else unavailable("Unreviewed Cloak of Shadows amount");}
            continue;
        }
        if(!creatureCaster&&d.spellFamily==10&&u(71)==3&&u(86)==21&&u(72)==64&&u(87)==1&&u(117)==31980&&!u(73)) {
            if(effect==0){d.righteousDefense=true;harm=true;}
            continue; // the dummy and its server-side target selection (31980): the script's
        }
        if(fanOfKnives&&effect==0) {
            const int32_t percent=i(80)+1; // CalcValue: base + 1 (one die side)
            if(percent<=0||percent>1000||u(74)>1||f(77)!=0)unavailable("Invalid weapon percentage");
            else {d.weaponPercent=uint16_t(percent);d.weaponDamage=true;d.fanOfKnives=true;harm=true;}
            continue;
        }
        if(coneOfCold&&effect!=1) {
            if(effect==0){d.areaSnarePercent=uint8_t(-(i(80)+1));d.areaConeDegrees=104.f;harm=true;}
            continue; // effect 2: the empty healing-taken hook (Improved Cone of Cold's)
        }
        // Holy Wrath: its stun on the creatures the area damage hits (undead and
        // demons only: the spell's TargetCreatureType).
        if(!creatureCaster&&d.spellFamily==10&&arcaneExplosion&&effect==1&&type==6&&u(96)==12&&u(87)==22&&u(90)==15&&u(71)==2&&!u(73)&&
           d.targetCreatureType&&d.durationMs&&!u(spell335::ProcFlags)){d.areaStun=true;harm=true;continue;}
        if(frostNova&&arcaneExplosion&&effect==1){d.areaRoot=true;d.controlDamageCapPct=10;harm=true;continue;}
        // Challenging Shout / Roar: a lone MOD_TAUNT on the enemies around the caster.
        if(!creatureCaster&&(d.spellFamily==4||d.spellFamily==7)&&effect==0&&type==6&&u(95)==11&&u(86)==22&&u(89)==15&&!u(72)&&!u(73)&&
           !u(116)&&i(80)+int32_t(u(74))==0&&!u(212)&&d.durationMs&&d.durationMs<=60000&&!u(spell335::ProcFlags)) {
            const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92));
            const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
            if(std::isfinite(r)&&r>0&&r<=30){d.areaTauntRadius=r;harm=true;continue;}
        }
        if(areaFear||intimidatingShout) {
            if(effect==(intimidatingShout?1u:0u)) {
                d.areaFearStunsTarget=intimidatingShout;
                const auto radius=ClientSpellTables::lookup(t.radiusIndex,u(92+effect));
                const float r=t.radii&&radius>=0?t.radii->getFloat(radius,1):0.f;
                if(!std::isfinite(r)||r<=0||r>30||!d.durationMs||d.durationMs>60000)unavailable("Unreviewed area fear");
                else {d.areaFearRadius=r;d.areaMaxTargets=uint8_t(u(212));d.controlDamageCapPct=10;harm=true;}
            }
            continue; // the target's stun (runtime) and the fleeing run speed, as Fear's
        }
        if(partyHeal&&effect==0)secondary=0; // the party area is partyHealRadius's
        if(!arcaneExplosion && !(type==5&&target==1&&secondary==17) && !(d.groundRadius&&type==2&&target==16&&!secondary) && (secondary || (target!=1&&target!=6&&target!=21&&target!=25&&!(chainHeal&&target==45)))) unavailable("Area or scripted targeting is not implemented");
        if(type==6 && (u(95+effect)==3 || u(95+effect)==8) && (((u(spell335::ProcFlags)||u(spell335::ProcCharges))&&!deathItemChannel&&!rootControl)||u(116+effect)))
            unavailable("Periodic proc, charge or triggered effects are not implemented");
        if(snare&&(effect==0||(effect==2&&d.snareZeroHealingMarker))){harm=true;continue;}
        // A creature caster's hostile slow or armor reduction (generated SmartAI
        // family). SpellEffectInfo::CalcValue: base points, +1 for a one-sided
        // die, RealPointsPerLevel on the caster level; creature level scaling
        // never applies to these two aura types. Only fixed-size, single-target
        // shapes with a real duration are admitted.
        if(creatureCaster&&type==6&&(u(95+effect)==33||((u(95+effect)==22||u(95+effect)==101)&&u(110+effect)==1))) {
            const int32_t amount=i(80+effect)+(u(74+effect)==1?1:0);
            const float perLevel=f(77+effect);
            const bool slowAura=u(95+effect)==33,percentAura=u(95+effect)==101;
            if(target!=6||secondary||u(74+effect)>1||!std::isfinite(perLevel)||u(116+effect)||
               !d.durationMs||d.durationMs>600000||u(spell335::ProcFlags)||u(spell335::ProcCharges))
                unavailable("Unsupported creature aura shape");
            else if(slowAura&&(perLevel!=0||amount<=-100||amount>=0||d.npcSlowPercent))
                unavailable("Unsupported creature slow amount");
            else if(percentAura&&(perLevel!=0||amount<=-100||amount>=0||d.npcArmorPercent))
                unavailable("Unsupported creature armor percentage");
            else if(!slowAura&&!percentAura&&(perLevel>0||perLevel<-1000||amount>=0||amount<-100000||d.npcArmorAmount))
                unavailable("Unsupported creature armor reduction amount");
            else if(slowAura)d.npcSlowPercent=uint8_t(-amount);
            else if(percentAura)d.npcArmorPercent=int8_t(amount);
            else {d.npcArmorAmount=amount;d.npcArmorPerLevel=perLevel;}
            harm=true;continue;
        }
        // A creature's stun or root on a player: one aura, a real duration, not
        // broken by damage (AURA_INTERRUPT_FLAG_TAKE_DAMAGE stays unmodelled).
        if(creatureCaster&&type==6&&(u(95+effect)==12||u(95+effect)==26)) {
            if(target!=6||secondary||u(116+effect)||!d.durationMs||d.durationMs>600000||
               (u(spell335::AuraInterruptFlags)&0x2u)||d.npcPlayerControl||u(spell335::ProcFlags)||u(spell335::ProcCharges))
                unavailable("Unsupported creature control shape");
            else d.npcPlayerControl=u(95+effect)==12?1:2;
            harm=true;continue;
        }
        // A creature's melee weapon special (Spell::EffectWeaponDmg).
        if(creatureCaster&&(type==17||type==31||type==58||type==121)) {
            const int32_t base=i(80+effect),dice=i(74+effect);const float perLevel=f(77+effect);
            if(u(213)!=2||target!=6||secondary||u(116+effect)||dice<0||dice>100000||!std::isfinite(perLevel)||std::abs(perLevel)>10000)
                unavailable("Unsupported creature weapon effect");
            else if(type==31) {
                const int32_t amount=base+(dice==1?1:0);
                if(dice>1||perLevel!=0||amount<=0||amount>1000||d.npcWeaponPercent)unavailable("Unsupported creature weapon percentage");
                else {d.npcWeaponPercent=uint16_t(amount);d.npcWeaponPercentFirst=!d.npcWeaponEffect;}
            } else if(d.npcWeaponEffect||base+(dice>=1?1:0)<0||base>100000)unavailable("Unsupported creature weapon damage bonus");
            else {
                // CalcValue: BasePoints plus irand(1, DieSides) (nothing for 0 sides).
                d.npcWeaponEffect=true;d.npcWeaponScales=type!=17;d.npcWeaponBonus=uint32_t(base+(dice>=1?1:0));
                d.npcWeaponBonusMax=d.npcWeaponBonus+(dice>1?uint32_t(dice-1):0);d.npcWeaponBonusPerLevel=perLevel;
            }
            harm=true;continue;
        }
        // Feint, Cower: SPELL_EFFECT_THREAT with a negative amount on its target.
        if(!creatureCaster&&(d.spellFamily==8||d.spellFamily==7)&&type==63&&target==6&&!secondary&&effect==0&&u(74)==1&&i(80)<-1&&i(80)>-100000&&
           std::isfinite(f(77))&&f(77)<=0&&f(77)>=-100&&!u(72)&&!u(73)&&!d.durationMs){
            d.threatReduction=uint32_t(-(i(80)+1));d.threatReductionPerLevel=-f(77);harm=true;continue;}
        const auto base=i(80+effect), dice=i(74+effect);const auto scale=f(77+effect);
        const float perCombo=f(119+effect);
        if(base < -1 || base > 100000 || dice<0 || dice>100000 || !std::isfinite(scale)||std::abs(scale)>10000 ||
           !std::isfinite(perCombo)||perCombo<0||perCombo>100000||(perCombo!=0&&!d.comboFinisher)) {
            if(!rider(effect,"Invalid or combo-point effect amount is not implemented"))
                unavailable("Invalid or combo-point effect amount is not implemented");
            continue;
        }
        // DBC base points encode one less than the minimum; preserve the dice
        // range and choose a deterministic midpoint in the local ruleset.
        const uint32_t low=uint32_t(base+1),high=low+(dice>1?uint32_t(dice-1):0);
        if(combo&&type==80){d.comboGain=uint8_t(low);harm=true;}
        else if(combo&&(type==58||type==121)){
            directSlot(effect);d.weaponDamage=true;d.normalizedWeapon=type==121;d.damage+=low;d.damageMax+=high;d.damagePerLevel+=scale;harm=true;
        }else if(combo&&type==31){
            if(low>1000||!low||dice>1||scale!=0||perCombo!=0)unavailable("Invalid weapon percentage");
            else d.weaponPercent=uint16_t(low);
        }else if(combo&&type==3&&d.comboProfile==uint8_t(LocalComboProfile::Eviscerate)){
            // Its reviewed dummy contributes the AP-per-combo term in localComboAmount.
        }else if(simpleShield!=SimpleShieldKind::None && effect==0) {
            buff=true;buffTarget=target;
            if(simpleShield==SimpleShieldKind::Mana){d.buffAbsorb=low;d.absorbSchoolMask=u(110);}
        } else if(earthShield && effect<2) {
            buff=true;buffTarget=21;
        } else if(reactive && effect==0 && type==6 && u(95)==42) {
            buff=true;buffTarget=1;
        } else if(reactive && effect==1 && type==6 && u(96)==85 && low>0) {
            d.manaPer5=low;buff=true;buffTarget=1;
        } else if(type==6 && (u(95+effect)==34 || (u(95+effect)==22 && u(110+effect)==1) ||
             (u(95+effect)==69 && u(110+effect) && u(110+effect)<=127)) &&
           (target==1 || target==21 || target==25) && !secondary && d.durationMs && (scale==0 || (u(95+effect)==69 && scale>0 && scale<100)) && dice<=1 && low>0 &&
           !u(spell335::ProcFlags) && !u(spell335::ProcCharges) && !u(116+effect) && !(u(4)&64u)) {
            if(buffTarget && buffTarget!=target)unavailable("Mixed buff targets are not implemented");
            buffTarget=target;
            if(u(95+effect)==34)d.buffHealth+=low;
            else if(u(95+effect)==22)d.buffArmor+=low;
            else {if(d.absorbSchoolMask && d.absorbSchoolMask!=u(110+effect))unavailable("Mixed absorb schools are not implemented");
                d.buffAbsorb+=low;d.absorbSchoolMask=u(110+effect);d.buffAbsorbPerLevel=scale;}
            buff=true;
        } else if(!combo&&type==3&&target==6&&!secondary&&effect==0&&!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[0]&0x200000u)&&
                  !u(72)&&!u(73)&&!d.weaponDamage&&[&]{const auto r=ClientSpellTables::lookup(t.spellIndex,50783);
                      return r>=0&&t.spells->getUInt32(uint32_t(r),71)==58;}()) {
            // spell_warr_slam: the dummy casts 50783 (WEAPON_DAMAGE) with its own
            // value as the bonus, so Slam is the weapon strike plus that value.
            directSlot(effect);d.weaponDamage=true;d.damage+=low;d.damageMax+=high;d.damagePerLevel+=scale;harm=true;
        } else if(!combo&&(type==58||type==121||type==17)&&target==6&&!secondary) {
            // Spell::EffectWeaponDmg for a single hostile target: the weapon's
            // damage (normalized for 121) plus this flat bonus, through the
            // same weapon pipeline the reviewed combo attacks use.
            if(d.weaponDamage)unavailable("Repeated weapon damage effects are not implemented");
            else {directSlot(effect);d.weaponDamage=true;d.normalizedWeapon=type==121;d.damage+=low;d.damageMax+=high;d.damagePerLevel+=scale;harm=true;}
        } else if(!combo&&type==31&&target==6&&!secondary) {
            if(low>1000||!low||dice>1||scale!=0)unavailable("Invalid weapon percentage");
            else {d.weaponPercent=uint16_t(low);d.weaponDamage=true;harm=true;}
        } else if(type==9&&horrorCoil&&effect==0) {
            directSlot(effect);d.damage+=low;d.damageMax+=high;d.damagePerLevel+=scale;harm=true;
            d.directLeechPct=uint16_t(std::lround(f(101)*100.f));
        } else if(type==2) {directSlot(effect);if(u(3)==kLocalMechanicBleed||u(83+effect)==kLocalMechanicBleed)d.directIgnoresArmor=true;d.damage+=low;d.damageMax+=high;d.damagePerLevel+=scale;d.directPerCombo+=perCombo;harm=true;}
        else if(type==10) {
            directSlot(effect);d.heal+=low;d.healMax+=high;d.healPerLevel+=scale;healing=true;
            if((target!=1&&target!=21&&target!=25&&!(chainHeal&&target==45))||(healingTarget&&healingTarget!=target))
                unavailable("Mixed or hostile healing targets are not implemented");
            healingTarget=target;
        }
        else if(type==6&&d.channel&&(u(95+effect)==23||u(95+effect)==227)&&u(116+effect)&&u(98+effect)>0&&d.durationMs&&
                u(98+effect)<=d.durationMs&&!d.periodicDamage&&!secondary) {
            // A periodic-trigger channel (Mind Flay, Arcane Missiles): each
            // tick casts a child whose school damage is the tick's amount.
            const auto child=ClientSpellTables::lookup(t.spellIndex,u(116+effect));
            bool found=false;
            // PERIODIC_TRIGGER_SPELL_WITH_VALUE (227, Mind Flay): the amount is
            // this aura's own, handed to the child's school damage.
            if(u(95+effect)==227&&child>=0&&base>=0&&base<100000) {
                bool childDamages=false;for(uint32_t ce=0;ce<3;++ce)childDamages|=t.spells->getUInt32(uint32_t(child),71+ce)==2;
                if(childDamages){d.periodicEffectSlot=uint8_t(effect);d.periodicDamage=low;d.periodicDamageMax=high;d.periodicIntervalMs=u(98+effect);harm=true;found=true;}
            }
            if(child>=0&&!found)for(uint32_t ce=0;ce<3&&!found;++ce)if(t.spells->getUInt32(uint32_t(child),71+ce)==2) {
                const int32_t cb=t.spells->getInt32(uint32_t(child),80+ce),cd=t.spells->getInt32(uint32_t(child),74+ce);
                if(cb>=0&&cb<100000&&cd>=0&&cd<100000) {
                    d.periodicEffectSlot=uint8_t(effect);d.periodicDamage=uint32_t(cb+1);d.periodicDamageMax=uint32_t(cb+std::max(1,cd));
                    d.periodicIntervalMs=u(98+effect);harm=true;found=true;
                }
            }
            if(!found)unavailable("Unsupported periodic trigger channel");
        }
        else if(type==6&&(u(95+effect)==3||(u(95+effect)==53&&(d.channel||(d.spellFamily==6&&!creatureCaster&&std::isfinite(f(101+effect))&&f(101+effect)>0&&f(101+effect)<=1))))&&
                d.durationMs&&d.durationMs<=600000&&u(98+effect)>0&&u(98+effect)<=d.durationMs&&!d.periodicDamage) {
            // SPELL_AURA_PERIODIC_LEECH: the damage heals the caster by its
            // EffectValueMultiplier (Drain Life's channel, Devouring Plague's DoT).
            if(u(95+effect)==53){d.periodicLeech=true;const float m=f(101+effect);d.periodicLeechPct=uint8_t(std::isfinite(m)&&m>0&&m<=1?std::lround(m*100.f):100);}
            d.periodicEffectSlot=uint8_t(effect);
            if(u(3)==kLocalMechanicBleed||u(83+effect)==kLocalMechanicBleed)d.periodicIgnoresArmor=true;d.periodicDamage=low;d.periodicDamageMax=high;d.periodicDamagePerLevel=scale;d.periodicPerCombo=perCombo;d.periodicIntervalMs=u(98+effect);harm=true;
        } else if(type==6&&u(95+effect)==8&&low>0&&d.durationMs&&d.durationMs<=600000&&u(98+effect)>0&&u(98+effect)<=d.durationMs&&
                  !d.periodicHeal&&!d.periodicDamage) {
            d.periodicEffectSlot=uint8_t(effect);
            d.periodicHeal=low;d.periodicHealMax=high;d.periodicHealPerLevel=scale;
            d.periodicIntervalMs=u(98+effect);healing=true;
            if((target!=1&&target!=21&&target!=25&&!(chainHeal&&target==45))||(healingTarget&&healingTarget!=target))
                unavailable("Mixed or hostile healing targets are not implemented");
            healingTarget=target;
        } else if(type==6&&(u(95+effect)==12||u(95+effect)==27||(confuseControl&&u(95+effect)==5)||((fearControl||horrorCoil)&&u(95+effect)==7)||(rootControl&&u(95+effect)==26&&effect==0))) {
            // P04 control auras, admitted only in the narrow shape every
            // reachable client spell already has: one aura effect, a single
            // hostile unit target, a real fixed duration, no proc definition,
            // no charges, no triggered child and no channel. Anything wider
            // would need state this realm does not carry, so it is rejected
            // here rather than silently applied as something weaker.
            const bool silence=u(95+effect)==27;
            // What may ride beside the control: Cheap Shot's and Gouge's combo
            // points, Gouge's opening hit (its stun is the third effect), and a
            // finisher's script dummy on the rogue plus Kidney Shot's
            // zero-amount damage-taken hook (a talent's slot).
            const auto riderOk=[&](uint32_t k){
                const auto type=u(71+k);
                if(k==effect||!type||type==80)return true;
                if(confuseControl&&type==6&&u(95+k)==33&&u(86+k)==6)return true; // Blind's slow
                if(confuseControl&&type==6&&u(95+k)==56&&k==1)return true; // Polymorph's transform
                if(fearControl&&k==1)return true; // Fear's run speed
                if(type==2&&k<effect&&u(86+k)==6&&!u(89+k))return true;
                if(type==58&&k<effect&&d.comboFinisher&&d.spellFamily==7&&u(86+k)==6&&!u(89+k))return true; // Maim's weapon strike
                if(horrorCoil&&type==9&&k==0)return true; // Death Coil's leech
                if(rootControl&&k==1)return true; // Entangling Roots' DoT (its own branch)
                if(d.id==47476&&type==6&&u(95+k)==4&&!i(80+k)&&!u(74+k))return true; // Strangulate's empty dummy
                if(d.spellFamily==7&&k==1&&type==64&&u(86+k)==6&&!u(89+k)&&u(116+k))return true; // Pounce's bleed (triggerDotSpell)
                // Banish, Cyclone: total school immunity and no healing taken on the held creature.
                if((d.spellFamily==5||d.spellFamily==7)&&effect==0&&type==6&&u(86+k)==6&&!u(89+k)&&i(110+k)==127&&
                   ((k==1&&u(95+k)==39)||(k==2&&u(95+k)==118&&i(80+k)+int32_t(u(74+k))==-100))){d.controlImmune=true;return true;}
                if(d.comboFinisher&&type==3&&u(86+k)==1)return true;
                return d.comboFinisher&&type==6&&u(95+k)==87&&u(86+k)==6&&!i(80+k)&&!u(74+k);
            };
            const auto controlMs=std::max(d.durationMs,d.comboDurationMaxMs);
            if(d.controlProfile)unavailable("Mixed control auras are not implemented");
            else if(effect&&!(u(71)==2&&!silence)&&!horrorCoil&&!(confuseControl&&u(71)==6&&u(95)==33)&&!(u(71)==58&&d.comboFinisher&&d.spellFamily==7))unavailable("A control aura outside the first effect is not implemented");
            else if(target!=6)unavailable("Area or scripted targeting is not implemented");
            else if(!controlMs||controlMs>600000)
                unavailable("A control without a real fixed duration is not implemented");
            else if((u(spell335::ProcFlags)&&!damageBrokenControl&&!fearControl&&!rootControl)||u(spell335::ProcCharges)||u(116+effect))
                unavailable("Proc, charge or triggered control auras are not implemented");
            else if(u(spell335::ChannelInterruptFlags))
                unavailable("Channelled control auras are not implemented");
            else if(!riderOk(0)||!riderOk(1)||!riderOk(2))
                unavailable("A control aura beside another effect is not implemented");
            // No mechanic requirement is imposed on either aura. The reference
            // imposes none: MOD_STUN carries five different mechanics across
            // the reachable spells alone - stunned, asleep, shackled, sapped and
            // incapacitated - and SPELL_PREVENTION_TYPE is a column of the
            // spell being PREVENTED, never of the silence doing the preventing.
            // Both rules were written here first and both were fabrications.
            else {
                d.controlProfile=silence?2:rootControl?3:1;d.controlEffectSlot=uint8_t(effect);harm=true;
                if(fearControl||(rootControl&&u(spell335::ProcFlags)))d.controlDamageCapPct=10;
                d.controlSingleTarget=(u(9)&0x20u)!=0; // SPELL_ATTR5_SINGLE_TARGET_SPELL
            }
        } else if(type==38) {
            // P04 SPELL_EFFECT_DISPEL, admitted in one shape only. Spell::CheckCast
            // refuses a PURE dispel with SPELL_FAILED_NOTHING_TO_DISPEL when the
            // target holds nothing it can remove (Spell.cpp:6264-6306), and this
            // realm holds nothing in the direction any player dispel looks:
            // every aura on a creature is harmful and every aura on a player is
            // helpful (the source audit section 5). The
            // fifteen pure dispels would therefore be fifteen buttons whose only
            // output is an error string, and they are refused by name. A dispel
            // BESIDE a non-dispel effect skips that gate (`hasNonDispelEffect`,
            // :6278-6282): the cast lands, the other effect runs, and the dispel
            // draws from an empty list - exactly what the reference does against
            // a buff-less creature, and exactly what Shield Slam does here.
            bool besideOther=false;
            for(unsigned k=0;k<3;++k)if(k!=effect&&u(71+k)&&u(71+k)!=38)besideOther=true;
            // A friendly dispel (Cleanse, Remove Curse, Cure Poison...): creature
            // debuffs on players are dispellable; one of that type per effect.
            if(!besideOther&&!creatureCaster&&(target==21||target==1)&&!secondary&&u(110+effect)>=1&&u(110+effect)<=4&&low==1&&dice<=1&&scale==0)
                d.dispelMask|=uint8_t(1u<<u(110+effect));
            // An offensive dispel (Purge, Tranquilizing Shot) on a creature's own
            // buffs (LocalNpcBuff): magic, and enrage for Tranquilizing Shot.
            else if(!besideOther&&!creatureCaster&&target==6&&!secondary&&(u(110+effect)==1||u(110+effect)==9)&&low>=1&&low<=10&&dice<=1&&scale==0) {
                d.dispelProfile=1;d.dispelAttempts=uint8_t(std::max<uint32_t>(d.dispelAttempts,low));d.hostileDispelMask|=1u<<u(110+effect);harm=true;
            }
            else if(!besideOther)unavailable("A pure dispel is not implemented: nothing this realm holds can be dispelled");
            else if(d.dispelProfile)unavailable("Repeated dispel effects are not implemented");
            else if(target!=6)unavailable("A dispel of a friendly target is not implemented");
            else if(u(110+effect)!=1)unavailable("A non-magic dispel type is not implemented");
            else if(dice>1||scale!=0||low>255)unavailable("Invalid dispel attempt count");
            else {d.dispelProfile=1;d.dispelAttempts=uint8_t(low);harm=true;}
        } else if(type==68&&target==6&&!secondary) {
            d.interruptCast=true;harm=true;
        } else if(type==63&&target==6&&!secondary&&!creatureCaster&&low<=100000) {
            // SPELL_EFFECT_THREAT: flat threat on the target (Distracting Shot);
            // Wind Shear's carries zero and changes nothing.
            if(low){d.threatAmount=low;harm=true;}
        } else if(type==6&&u(95+effect)==11&&target==6&&!secondary&&!creatureCaster&&d.spellFamily==9&&d.durationMs) {
            d.taunt=true;harm=true; // Distracting Shot's MOD_TAUNT: the taunt the realm already applies
        } else if(type==114&&target==6&&!secondary) {
            d.taunt=true;harm=true;
        } else if(type==96&&target==6&&!secondary) {
            d.charge=true;harm=true;
        } else if(chargeSpell&&type==3&&target==1&&!secondary&&base>=0&&base<1000) {
            d.chargeRage=uint16_t((base+1)/10); // Charge's script: tenths of rage.
        } else if(!creatureCaster&&d.spellFamily==4&&(d.spellFamilyFlags[0]&0x20000000u)&&type==3&&target==6&&!secondary) {
            directSlot(effect);d.damage=low;d.damageMax=high;d.executeSpell=true;harm=true;
            d.extraEnergyMultiplier=f(216)*10.f;
        } else if(type==5&&!creatureCaster&&(target==1||target==0)&&(secondary==17||!secondary)) {
            d.teleport=true;
        } else if(type==77&&!creatureCaster&&d.spellFamily==5&&effect==0&&target==1&&!secondary&&!u(72)&&!u(73)&&localHealthstoneItem(d.id)) {
            // spell_warl_create_healthstone: the rank's healthstone (without the
            // Improved Healthstone talent, which the realm does not grant).
            d.createItemId=localHealthstoneItem(d.id);d.createItemCount=1;d.createItemUnique=true;
        } else if(type==64&&!creatureCaster&&d.spellFamily==15&&target==6&&!secondary&&localDiseaseSpell(u(116+effect))&&!d.diseaseSpell) {
            // Icy Touch / Plague Strike: the disease lands with the hit. Its
            // row: one periodic damage aura (3), Frost Fever's two attack-speed
            // auras (138, 140) of the same amount, Blood Plague's dummy (4).
            const auto child=ClientSpellTables::lookup(t.spellIndex,u(116+effect));
            const auto cu=[&](uint32_t col){return t.spells->getUInt32(uint32_t(child),col);};
            const auto ci=[&](uint32_t col){return t.spells->getInt32(uint32_t(child),col);};
            const auto durationRow=child>=0?ClientSpellTables::lookup(t.durationIndex,cu(40)):-1;
            const int32_t duration=durationRow>=0?t.durations->getInt32(uint32_t(durationRow),1):0;
            if(child<0||cu(71)!=6||cu(95)!=3||cu(86)!=6||cu(98)<1000||duration<=0||duration>60000||!cu(225))
                unavailable("Unreviewed disease row");
            else {
                int8_t haste=0;bool other=false;
                for(uint32_t k=1;k<3;++k)if(cu(71+k)) {
                    const auto au=cu(95+k);const int32_t amount=ci(80+k)+(cu(74+k)?1:0); // CalcValue: +1 only with a die
                    if(au==138||au==140){if(amount<0&&amount>-100&&(!haste||haste==amount))haste=int8_t(amount);else other=true;}
                    else if(!(au==4&&!amount))other=true;
                }
                if(other)unavailable("Unreviewed disease row");
                else {d.diseaseSpell=u(116+effect);d.diseaseIntervalMs=cu(98);d.diseaseDurationMs=uint32_t(duration);
                      d.diseaseSchool=uint8_t(cu(225));d.diseaseHastePct=haste;harm=true;}
            }
        } else if(type==3&&!creatureCaster&&d.spellFamily==15&&(d.spellFamilyFlags[0]&0x2000u)&&target==25&&!secondary&&effect==0&&
                  !u(72)&&!u(73)&&low>0&&!scale) {
            // spell_dk_death_coil: on an enemy, the dummy's value as the shadow
            // hit of 47632 (its friendly branch heals only undead creatures,
            // which a player is not). The hit is this spell's direct damage.
            directSlot(effect);d.damage+=low;d.damageMax+=high;harm=true;
        } else if(type==3&&!creatureCaster&&d.id==49576&&target==6&&!secondary&&effect==0&&!u(72)&&!u(73)) {
            d.deathGrip=true;d.taunt=true;harm=true; // spell_dk_death_grip (49576 -> 49560)
        } else if(lifeTap&&type==3&&effect==0) {
            d.lifeTapAmount=uint32_t(i(80)+1);d.lifeTapPerLevel=f(77); // CalcValue: base + 1 (one die side)
        } else if(type==77&&!creatureCaster&&d.spellFamily==5&&target==6&&!secondary&&!i(80+effect)&&!u(74+effect)&&!u(116+effect)&&
                  effect==2&&u(71)==6&&u(95)==3&&u(71+1)==2) {
            // Immolate: a periodic fire aura, its opening hit, and a script
            // effect with no handler and no amount (no SpellScript at the pin;
            // Conflagrate's consumption is a talent's). It does nothing.
        } else if(type==77&&!creatureCaster&&d.teleport) {
            // The Teleport spells' script effect only plays the departure visual.
        } else if(type==24&&!creatureCaster&&(target==1||target==0)&&!secondary&&u(107+effect)) {
            // Spell::EffectCreateItem into the caster's bags.
            if(d.createItemId||low>200)unavailable("Repeated or oversized item creation is not implemented");
            else {d.createItemId=u(107+effect);d.createItemCount=uint16_t(std::max(1u,low));}
        } else {
            const auto reason="Unsupported effect "+std::to_string(type)+(type==6?" / aura "+std::to_string(u(95+effect)):"");
            if(!rider(effect,reason))unavailable(reason);
        }
    }
    if(d.channel&&!creatureCaster) {
        if(!d.periodicDamage||d.damage||d.weaponDamage||healing||buff)unavailable("Channeled spells are not implemented");
        else d.soulShardOnKill=d.soulShardOnKill||d.name=="Drain Soul";
    }
    if(!setAside.empty()) {
        const bool directHit=(d.damage||d.weaponDamage||d.interruptCast||d.taunt||d.charge||d.periodicDamage)&&!healing&&!buff&&!d.controlProfile;
        if(!directHit)unavailable(setAside.front());
    }
    if(harm&&!d.schoolMask&&(!creatureCaster||d.damage||d.periodicDamage))unavailable("Damaging spell has no school");
    if(buff&&(harm||healing)) unavailable("Mixed stat buffs and other effects are not implemented");
    if(harm&&healing) unavailable("Mixed hostile/friendly spells are not implemented");
    if(!harm&&!healing&&!buff&&!d.formId&&!formBoost&&!formResource&&!summonPet&&!areaAura&&!d.controlProfile&&!d.createItemId&&!d.teleport&&!d.totemEntry&&!d.dispelMask&&!d.lifeTapAmount&&!d.raiseDeadEntry&&!d.vanish&&!d.layOnHands&&!d.guardianEntry&&!d.deathPactPct) unavailable("No supported direct or periodic damage/healing effect");
    if(d.teleport&&(harm||healing||buff||d.createItemId))unavailable("Teleport beside other effects is not implemented");
    if(d.createItemId&&(harm||healing||buff))unavailable("Item creation beside other effects is not implemented");
    if(!creatureCaster){d.healingSelfOnly=healingTarget==1;d.buffSelfOnly=d.formId!=0||formBoost||formResource||buffTarget==1;}
    const auto rangeRow=ClientSpellTables::lookup(t.rangeIndex,u(46));
    if(rangeRow<0) unavailable("Range record missing");
    else {
        d.minRange=t.ranges->getFloat(rangeRow,(healing||buff)?2:1);d.range=t.ranges->getFloat(rangeRow,(healing||buff)?4:3);
        // SpellRange.dbc Flags (column 5): Spell::CheckRange branches on it for
        // the melee reach term and the completion leniency (Spell.cpp:7332-7396).
        const auto rangeFlags=t.ranges->getUInt32(rangeRow,5);
        if(rangeFlags>2)unavailable("Range record carries an unknown range type");
        else d.sourceRangeFlags=uint8_t(rangeFlags);
        if(!std::isfinite(d.range)||!std::isfinite(d.minRange)||d.minRange<0||d.range<d.minRange||d.range>(creatureCaster?50000.f:100.f))
            unavailable("Invalid or unsupported range");
    }
    if(d.mana>100000||d.manaPercent>100||d.cooldownMs>3600000||d.categoryCooldownMs>3600000||d.globalCooldownMs>60000)
        unavailable("Invalid spell resource or cooldown metadata");
    if((reactive||earthShield||molten||simpleShield!=SimpleShieldKind::None)&&!validLocalProc(d))unavailable("Invalid shield metadata");
    if(!t.iconIndex.empty()) {
        const auto iconRow=ClientSpellTables::lookup(t.iconIndex,d.iconId);
        if(iconRow>=0) {d.iconPath=t.icons->getString(iconRow,1);if(d.iconPath.size()>256)d.iconPath.clear();}
    }
    if(d.id==30451||d.id==42894||d.id==42896||d.id==42897){
        if(!decodeClientArcaneBlast(t,row,d))d.unsupportedReason="Unreviewed Arcane Blast source closure";
    }
    return d.unsupportedReason.empty();
}

// Ground mounts only: preserve the client's cast time, source creature and
// run-speed aura. Flight, vehicle and scripted mount effects stay unsupported.
// classMount: a class's own steed (Warhorse, Charger, Felsteed, Dreadsteed,
// Acherus Deathcharger) carries one more effect, the summon sound script (77),
// which has no gameplay of its own.
inline bool decodeClientGroundMount(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d,bool classMount=false) {
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    d.id=u(0);d.clientSpell=true;d.allowableClasses=0x5ff;d.resourceType=255;d.range=0;
    // Column 38, BaseLevel, as everywhere else since the reference. This decoder carries
    // only the columns a ground mount needs and deliberately does not set
    // d.spellLevel: SpellLevel is the initial-threat term (Spell.cpp:5780) and a
    // mount never lands on a target, so claiming one would make the implementation's threat
    // census untrue. Every ground mount has BaseLevel 0 and SpellLevel 1.
    d.name=t.spells->getString(row,136);d.iconId=u(133);d.visualId=u(131);d.baseLevel=u(38);
    if(d.name.empty() || d.name.size()>96 || u(42) || u(43) || u(44) || u(45) || u(226))return false;
    for(unsigned e=0;e<3;++e) {
        if(!u(71+e))continue;
        if(classMount&&u(71+e)==77)continue;
        if(u(71+e)!=6 || u(86+e)!=1 || u(89+e))return false;
        if(u(95+e)==78) {d.mountCreatureId=u(110+e);d.mountDisplayId=localMountDisplay(d.mountCreatureId);}
        else if(u(95+e)==32 && i(80+e)>=0 && i(80+e)<=199)d.mountSpeedPercent=uint32_t(i(80+e)+1);
        else return false;
    }
    if(!d.mountDisplayId || !d.mountSpeedPercent)return false;
    const auto cast=ClientSpellTables::lookup(t.castIndex,u(28));
    if(cast<0 || t.casts->getInt32(cast,1)<0 || t.casts->getInt32(cast,1)>10000 || t.casts->getInt32(cast,2))return false;
    d.castTimeMs=uint32_t(t.casts->getInt32(cast,1));
    d.interruptFlags=u(spell335::InterruptFlags);d.noPushback=(u(10)&0x8000u)!=0;
    d.noCategoryCooldownMods=(u(10)&0x80000000u)!=0;
    const auto icon=ClientSpellTables::lookup(t.iconIndex,d.iconId);
    if(icon>=0) d.iconPath=t.icons->getString(icon,1);
    if(d.iconPath.size()>256)d.iconPath.clear();
    return true;
}

/// One SkillLineAbility.dbc row, reduced to what this import needs.
/// Build 12340: ID, SkillLine, Spell, RaceMask, ClassMask, ExcludeRace,
/// ExcludeClass, MinSkillLineRank, SupercededBySpell, AcquireMethod,
/// TrivialSkillLineRankHigh, TrivialSkillLineRankLow, CharacterPoints[2].
struct AbilityRow {
    uint32_t spellId = 0, skillId = 0, classMask = 0, supercededBy = 0;
    uint16_t requiredSkill = 0, trivialHigh = 0, trivialLow = 0;
    uint32_t category = 0;   ///< the SkillLine category this came from
    std::vector<LocalRecipeAccess> recipeAccess;
    bool ambiguousRecipe = false;
};

// One unconditional passive percentage modifier for pushback. Mixed talents,
// proc effects and form-dependent profiles stay unavailable as whole spells.
inline bool decodeClientPushbackTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto i=[&](uint32_t col){return t.spells->getInt32(row,col);};
    if(!(u(4)&64u) || (u(4)&~464u) || !d.talentId || !d.spellFamily ||
       u(71)!=6 || u(72) || u(73) || u(95)!=108 || u(110)!=9 ||
       u(86)!=1 || u(89) || u(74)>1 || t.spells->getFloat(row,77)!=0 ||
       i(80)<0 || i(80)>99 || u(83) || u(92) || u(98) || u(104) ||
       u(107) || u(113) || u(116) || t.spells->getFloat(row,119)!=0 ||
       u(32) || u(33) || u(34) || u(36) || u(40) || u(41) ||
       u(42) || u(43) || u(44) || u(45) || u(49)>1 || u(50) || u(51) ||
       i(68)!=-1 || u(69) || u(70) || u(204) || u(226))return false;
    for(unsigned col=5;col<=27;++col)if(u(col))return false;
    for(unsigned col=52;col<68;++col)if(u(col))return false;
    const auto cast=ClientSpellTables::lookup(t.castIndex,u(28));
    if(cast<0 || t.casts->getInt32(cast,1) || t.casts->getInt32(cast,2))return false;
    std::array<uint32_t,3> mask{};
    for(unsigned k=0;k<3;++k)mask[k]=u(spell335::EffectClassMask+k);
    if(!(mask[0]|mask[1]|mask[2]))return false;
    d.passive=true;d.passivePushbackPct=uint8_t(i(80)+1);d.pushbackSpellMask=mask;
    d.unsupportedReason.clear();return true;
}

// Unconditional cast-time, mana-cost and flat/percentage recovery reductions.
// All populated effects must fit; unsupported secondary effects reject the talent.
inline bool decodeClientCastModifierTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto i=[&](uint32_t col){return t.spells->getInt32(row,col);};
    if(!(u(4)&64u) || (u(4)&~(464u|0x40000u)) || !d.talentId || !d.spellFamily ||
       u(2) || u(3) || u(32) || u(33) || u(34) || u(36) || u(41) ||
       u(42) || u(43) || u(44) || u(45) || u(49)>1 || u(50) || u(51) ||
       i(68)!=-1 || u(69) || u(70) || u(204) || u(226))return false;
    // These Attr3 bits only govern proc dispatch; this profile forbids
    // proc flags, charges and triggered effects, so no proc is being granted.
    for(unsigned col=5;col<=27;++col)if(u(col)&~(col==7?0x04080000u:0u))return false;
    for(unsigned col=52;col<68;++col)if(u(col))return false;
    const auto cast=ClientSpellTables::lookup(t.castIndex,u(28));
    if(cast<0 || t.casts->getInt32(cast,1) || t.casts->getInt32(cast,2))return false;
    if(u(40)) {
        const auto duration=ClientSpellTables::lookup(t.durationIndex,u(40));
        if(duration<0 || t.durations->getInt32(duration,1)!=-1 ||
           t.durations->getInt32(duration,2)!=0 || t.durations->getInt32(duration,3)!=-1)return false;
    }
    // A secondary target-none ApplyAura inherits the explicit self target
    // established by the first self aura. Other target forms remain blocked.
    // EffectItemType is not consumed by aura 107/108; only the explicit
    // EffectSpellClassMask controls matching, including all three words.
    std::array<LocalPassiveCastModifier,3> modifiers{};bool any=false;
    for(unsigned e=0;e<3;++e)if(u(71+e)) {
        const auto op=u(110+e),aura=u(95+e);
        if(aura==10) {
            const int64_t amount=int64_t(i(80+e))+1;
            if(u(71+e)!=6||u(86+e)!=1||u(89+e)||u(74+e)>1||t.spells->getFloat(row,77+e)!=0||
               u(83+e)||u(92+e)||u(98+e)||u(104+e)||u(113+e)||u(116+e)||t.spells->getFloat(row,119+e)!=0||
               amount>=0||amount< -100||!op||op>127||d.passiveSchoolThreatMask)return false;
            for(unsigned k=0;k<3;++k)if(u(spell335::EffectClassMask+e*3+k))return false;
            d.passiveSchoolThreatPercent=int16_t(amount);d.passiveSchoolThreatMask=uint8_t(op);any=true;continue;
        }
        // Operations 18 and 26 are the proc chance and PPM modifiers
        // Aura::CalcProcChance consumes. They were rejected here, which made the
        // shared arithmetic that consumes them unreachable against real data.
        if(u(71+e)!=6 || !((aura==107 && (op==0||op==1||op==3||op==5||op==7||op==10||op==11||op==12||op==16||op==18||op==21||op==22||op==23||op==26)) || (aura==108 && (op==0||op==1||op==2||op==3||op==5||op==7||op==9||op==11||op==12||op==14||op==16||op==18||op==21||op==22||op==23||op==26))) ||
           (u(86+e)!=1 && !(e>0&&u(86+e)==0&&u(71)==6&&u(86)==1)) || u(89+e) || u(74+e)>1 || t.spells->getFloat(row,77+e)!=0 ||
           u(83+e) || u(92+e) || u(98+e) || u(104+e) || u(113+e) || u(116+e) ||
           t.spells->getFloat(row,119+e)!=0)return false;
        const int64_t amount=int64_t(i(80+e))+1;
        const bool increases=op==0||op==3||op==5||op==7||op==9||op==12||op==16||op==22||op==23;
        // The reference places no ceiling on a chance/PPM modifier; the largest
        // real values in the whole population are +100 flat and +100 percent,
        // and +75 percent for op 26. Keep this build's bounded-record discipline
        // rather than the reference's absence of one.
        if(op==18||op==26) {
            if(!amount||amount>(aura==108?1000:100000)||amount<(aura==108?-100:-100000))return false;
        } else if(op==1) {
            if(!amount || amount>(aura==108?1000:600000) || amount<(aura==108?-100:-600000))return false;
        } else if(increases ? amount<=0||amount>(aura==108||(op==5||op==7||op==16)?100:100000) :
           amount>=0||amount<(aura==108?-100:op==11?-3600000:-60000))return false;
        auto& mod=modifiers[e];mod.operation=uint8_t(op);mod.percentage=aura==108;mod.active=true;mod.amount=int32_t(amount);
        for(unsigned k=0;k<3;++k)mod.mask[k]=u(spell335::EffectClassMask+e*3+k);
        if(!(mod.mask[0]|mod.mask[1]|mod.mask[2]))return false;
        if(op==9){if(d.passivePushbackPct)return false;d.passivePushbackPct=uint8_t(amount);d.pushbackSpellMask=mod.mask;mod={};}
        any=true;
    }
    if(!any)return false;
    d.passive=true;d.passiveCastModifiers=modifiers;d.unsupportedReason.clear();return true;
}

// A single unconditional regeneration aura. Mixed talents (Intensity,
// Pyromaniac), timed auras, triggered effects and form/equipment conditions
// remain blocked as whole talents. SpellFamily may be zero on Meditation.
inline bool decodeClientRegenerationTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto i=[&](uint32_t col){return t.spells->getInt32(row,col);};
    const auto aura=u(95);
    if(!(u(4)&64u) || (u(4)&~464u) || !d.talentId ||
       u(2) || u(3) || u(71)!=6 || u(72) || u(73) || (aura!=134 && aura!=219) ||
       u(86)!=1 || u(89) || u(74)!=1 || t.spells->getFloat(row,77)!=0 ||
       i(80)<0 || i(80)>99 || u(83) || u(92) || u(98) || u(104) ||
       u(107) || u(113) || u(116) || t.spells->getFloat(row,119)!=0 ||
       (aura==134 ? u(110)!=0 : u(110)>4) ||
       u(29) || u(30) || u(31) || u(32) || u(33) || u(34) || u(36) ||
       u(41) || u(42) || u(43) || u(44) || u(45) || u(49)>1 || u(50) || u(51) ||
       i(68)!=-1 || u(69) || u(70) || u(204) || u(205) || u(206) || u(226))return false;
    for(unsigned col=5;col<=27;++col)if(u(col))return false;
    for(unsigned col=52;col<68;++col)if(u(col))return false;
    const auto cast=ClientSpellTables::lookup(t.castIndex,u(28));
    if(cast<0 || t.casts->getInt32(cast,1) || t.casts->getInt32(cast,2))return false;
    if(u(40)) {
        const auto duration=ClientSpellTables::lookup(t.durationIndex,u(40));
        if(duration<0 || t.durations->getInt32(duration,1)!=-1 ||
           t.durations->getInt32(duration,2)!=0 || t.durations->getInt32(duration,3)!=-1)return false;
    }
    // Aura 219 uses EffectMiscValue as a stat index, not its spell class mask;
    // the source UpdateManaRegen sums stat * amount / 500 per second.
    // Aura 134 has no spell mask or stat-dependent operation.
    if(aura==134)for(unsigned k=0;k<3;++k)if(u(spell335::EffectClassMask+k))return false;
    d.passiveManaRegenInterruptPct=0;d.passiveManaRegenStatPct={};
    if(aura==134)d.passiveManaRegenInterruptPct=uint8_t(i(80)+1);
    else d.passiveManaRegenStatPct[u(110)]=uint8_t(i(80)+1);
    d.passive=true;d.unsupportedReason.clear();return true;
}

// Narrow source profiles compare all gameplay columns, including otherwise easy
// to miss conditions/costs/secondary effects. Names/icons/visuals are presentation.
inline bool matchesClientTalentSource(const ClientSpellTables& t,uint32_t row,
        std::initializer_list<std::pair<uint32_t,uint32_t>> populated) {
    for(uint32_t col=1;col<234;++col) {
        if(col>=131 && col<=203)continue;
        uint32_t expected=0;
        for(const auto& [key,value]:populated)if(key==col){expected=value;break;}
        if(t.spells->getUInt32(row,col)!=expected)return false;
    }
    return true;
}
inline bool clientTalentInstantSelfTables(const ClientSpellTables& t,uint32_t row) {
    const auto cast=ClientSpellTables::lookup(t.castIndex,t.spells->getUInt32(row,28));
    const auto range=ClientSpellTables::lookup(t.rangeIndex,t.spells->getUInt32(row,46));
    if(cast<0 || range<0 || t.casts->getInt32(cast,1) || t.casts->getInt32(cast,2) ||
       t.ranges->getFloat(range,2)!=0 || t.ranges->getFloat(range,4)!=0)return false;
    if(const auto id=t.spells->getUInt32(row,40)) {
        const auto duration=ClientSpellTables::lookup(t.durationIndex,id);
        if(duration<0 || t.durations->getInt32(duration,1)!=-1 ||
           t.durations->getInt32(duration,2) || t.durations->getInt32(duration,3)!=-1)return false;
    }
    return true;
}
// Cruelty is melee-weapon critical chance, not unconditional/spell critical
// chance. Keep its weapon requirement for the exact attacking-hand calculation.
inline bool decodeClientCrueltyTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    constexpr uint32_t ranks[]={12320,12852,12853,12855,12856};
    if(d.talentId!=157 || d.allowableClasses!=1 || d.talentRank<1 || d.talentRank>5 ||
       ranks[d.talentRank-1]!=d.id)return false;
    const uint32_t rank=d.talentRank;
    if(!matchesClientTalentSource(t,row,{{4,464},{28,1},{35,101},{39,1},{40,rank==1?0u:21u},
       {46,1},{68,2},{69,173555},{71,6},{74,1},{80,rank-1},{86,1},{95,52},
       {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1}}) ||
       !clientTalentInstantSelfTables(t,row))return false;
    d.passiveMeleeCritPct=uint8_t(rank);d.requiredItemClass=2;d.requiredItemSubclasses=173555;
    d.requiredInventoryTypes=0;d.passive=true;d.buffSelfOnly=true;d.unsupportedReason.clear();return true;
}

// Reviewed Flurry talent and internal haste aura are admitted as one closure.
// Parent: critical melee auto/special (including permitted triggered hits).
// Child: successful/absorbed auto-attacks spend charges; Shaman's source script
// additionally excludes Windfury, Stormstrike and Lava Lash at runtime.
inline bool decodeClientFlurryTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d,
                                    LocalSpellDefinition* internalAura=nullptr) {
    constexpr uint32_t warriorParents[]={12319,12971,12972,12973,12974};
    constexpr uint32_t shamanParents[]={16256,16281,16282,16283,16284};
    constexpr uint32_t shamanChildren[]={16257,16277,16278,16279,16280};
    const bool shaman=d.talentId==602;
    if((!shaman&&d.talentId!=156)||d.allowableClasses!=(shaman?64u:1u)||
       d.talentRank<1||d.talentRank>5)return false;
    const uint32_t rank=d.talentRank;
    if(d.id!=(shaman?shamanParents:warriorParents)[rank-1])return false;
    const uint32_t childId=shaman?shamanChildren[rank-1]:12965+rank;
    const uint32_t haste=rank*(shaman?6u:5u);
    if(!matchesClientTalentSource(t,row,{{4,shaman?208u:262336u},{7,67108864u},
        {28,1},{34,20},{35,100},{39,1},{40,shaman?21u:0u},{46,1},{68,4294967295u},
        {71,6},{74,1},{86,1},{95,42},{116,childId},{216,0x3f800000},
        {217,0x3f800000},{218,0x3f800000},{225,1}})||!clientTalentInstantSelfTables(t,row))return false;
    const auto childRow=ClientSpellTables::lookup(t.spellIndex,childId);
    if(childRow<0||!matchesClientTalentSource(t,uint32_t(childRow),{
        {4,shaman?0u:262144u},{7,shaman?524288u:0u},{28,1},{34,4},{35,100},{36,3},
        {39,1},{40,8},{46,1},{68,4294967295u},{71,6},{74,1},{80,haste-1},{86,1},
        {95,138},{208,shaman?11u:4u},{210,shaman?512u:0u},
        {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1}}))return false;
    const auto duration=ClientSpellTables::lookup(t.durationIndex,8);
    if(duration<0||t.durations->getInt32(duration,1)!=15000||t.durations->getInt32(duration,2)||
       t.durations->getInt32(duration,3)!=15000)return false;
    LocalSpellDefinition aura;aura.id=childId;aura.clientSpell=true;aura.triggeredOnly=true;
    aura.allowableClasses=d.allowableClasses;aura.buffSelfOnly=true;aura.durationMs=15000;
    aura.range=0;aura.schoolMask=1;aura.spellFamily=shaman?11:4;
    aura.spellFamilyFlags={0,shaman?512u:0u,0};aura.meleeHastePct=uint8_t(haste);
    aura.procParentTalentId=d.talentId;aura.baseLevel=1;
    aura.name=t.spells->getString(childRow,136);aura.iconId=t.spells->getUInt32(childRow,133);
    aura.proc.effect=LocalProcEffect::ConsumeOwnerAuraCharge;aura.proc.spellId=childId;
    aura.proc.flags=4;aura.proc.chance=100;aura.proc.charges=3;aura.proc.amount=haste;
    aura.proc.schoolMask=1;aura.proc.spellFamily=aura.spellFamily;
    aura.proc.spellFamilyFlags=aura.spellFamilyFlags;aura.proc.cooldownMs=shaman?500:0;
    aura.proc.hitMask=LocalProcHitNormal|LocalProcHitCritical|LocalProcHitAbsorb;
    // Runtime preserves the core auto-attack exception without admitting spells.
    aura.proc.allowTriggered=false;
    if(!validLocalProc(aura))return false;
    LocalProcDefinition proc;proc.effect=LocalProcEffect::ApplyOwnerAura;proc.spellId=childId;
    proc.flags=20;proc.chance=100;proc.amount=haste;proc.schoolMask=1;
    proc.spellFamily=aura.spellFamily;proc.spellFamilyFlags=aura.spellFamilyFlags;
    proc.spellTypeMask=1;proc.hitMask=LocalProcHitCritical;proc.allowTriggered=true;
    d.proc=proc;d.passive=true;d.buffSelfOnly=true;d.unsupportedReason.clear();
    if(!validLocalProc(d))return false;
    if(internalAura)*internalAura=std::move(aura);
    return true;
}

// The actual Primal Fury talent teaches TWO form-exclusive helper auras. Both
// helpers and both leaves must match; importing only the bear half is forbidden.
inline bool decodeClientPrimalFuryTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    if(d.talentId!=801 || d.allowableClasses!=1024 || d.talentRank<1 || d.talentRank>2 ||
       d.id!=(d.talentRank==1?37116u:37117u))return false;
    const uint32_t bear=d.talentRank==1?16958:16961,cat=d.talentRank==1?16952:16954;
    const uint32_t chance=d.talentRank==1?50:100;
    if(!matchesClientTalentSource(t,row,{{4,8651136},{5,0x80000000},{6,1},{8,32768},{12,145},
       {28,1},{35,101},{46,1},{68,0xffffffff},{71,36},{72,36},{74,1},{75,1},
       {80,0xffffffff},{81,0xffffffff},{86,1},{87,1},{116,bear},{117,cat},
       {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1},{231,0x3f800000}}) ||
       !clientTalentInstantSelfTables(t,row))return false;
    const auto bearRow=ClientSpellTables::lookup(t.spellIndex,bear);
    const auto catRow=ClientSpellTables::lookup(t.spellIndex,cat);
    const auto rageRow=ClientSpellTables::lookup(t.spellIndex,16959);
    const auto comboRow=ClientSpellTables::lookup(t.spellIndex,16953);
    if(bearRow<0 || catRow<0 || rageRow<0 || comboRow<0)return false;
    if(!matchesClientTalentSource(t,uint32_t(bearRow),{{4,464},{8,524288},{12,144},{28,1},
       {34,87380},{35,chance},{46,1},{68,0xffffffff},{71,6},{86,1},{95,42},{116,16959},
       {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1},
       {229,0x3f800000},{230,0x3f800000},{231,0x3f800000}}) ||
       !clientTalentInstantSelfTables(t,uint32_t(bearRow)))return false;
    if(!matchesClientTalentSource(t,uint32_t(catRow),{{4,464},{7,524288},{8,524288},{12,1},{28,1},
       {34,87376},{35,chance},{46,1},{68,0xffffffff},{71,6},{86,1},{95,42},{116,16953},{208,7},
       {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1},
       {229,0x3f800000},{230,0x3f800000},{231,0x3f800000}}) ||
       !clientTalentInstantSelfTables(t,uint32_t(catRow)))return false;
    if(!matchesClientTalentSource(t,uint32_t(rageRow),{{4,16777216},{8,524416},{9,8},{12,144},
       {28,1},{35,101},{46,1},{68,0xffffffff},{71,30},{74,1},{80,49},{86,1},{110,1},
       {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1},
       {229,0x3f800000},{230,0x3f800000},{231,0x3f800000}}) ||
       !clientTalentInstantSelfTables(t,uint32_t(rageRow)))return false;
    if(!matchesClientTalentSource(t,uint32_t(comboRow),{{4,16777232},{6,4},{7,196608},{8,524416},
       {9,8},{12,1},{28,1},{35,101},{39,1},{46,2},{49,3},{68,0xffffffff},{71,80},{74,1},{86,6},
       {216,0x3f800000},{217,0x3f800000},{218,0x3f800000},{225,1}}))return false;
    const auto comboCast=ClientSpellTables::lookup(t.castIndex,1);
    const auto comboRange=ClientSpellTables::lookup(t.rangeIndex,2);
    if(comboCast<0 || comboRange<0 || t.casts->getInt32(comboCast,1) || t.casts->getInt32(comboCast,2) ||
       t.ranges->getFloat(comboRange,1)!=0 || t.ranges->getFloat(comboRange,3)!=5)return false;
    LocalProcDefinition bearProc;bearProc.effect=LocalProcEffect::RestorePower;bearProc.spellId=16959;
    bearProc.resourceType=1;bearProc.amount=5;bearProc.schoolMask=1;bearProc.flags=87380;
    bearProc.chance=uint8_t(chance);bearProc.hitMask=LocalProcHitCritical;bearProc.spellTypeMask=1;
    bearProc.requiredForms=144;
    LocalProcDefinition catProc;catProc.effect=LocalProcEffect::AddComboPoints;catProc.spellId=16953;
    catProc.amount=1;catProc.schoolMask=1;catProc.flags=87376;catProc.chance=uint8_t(chance);
    catProc.hitMask=LocalProcHitCritical;catProc.triggerSpellFamily=7;
    catProc.triggerSpellFamilyFlags={233472,1024,262144};catProc.requiredForms=1;catProc.range=5;
    d.proc=bearProc;d.secondaryProc=catProc;d.requiredForms=145;d.excludedForms=0;
    d.passive=true;d.buffSelfOnly=true;d.unsupportedReason.clear();return true;
}

// Reviewed passive resource procs, with SQL overrides pinned in the source audit.
// Their triggers have neither charges nor cooldown state; only the learned rank
// is considered by the authority. Unsupported extra effects reject the whole rank.
inline bool decodeClientResourceProcTalent(const ClientSpellTables& t,uint32_t row,LocalSpellDefinition& d) {
    struct Profile {uint32_t id,child;uint8_t kind,rank;};
    constexpr Profile profiles[]={
        {12322,12964,1,1},{12999,12964,1,2},{13000,12964,1,3},{13001,12964,1,4},{13002,12964,1,5},
        {51634,51637,2,1},{51635,51637,2,2},{51636,51637,2,3},
        {35541,35542,3,1},{35550,35545,3,2},{35551,35546,3,3},{35552,35547,3,4},{35553,35548,3,5}};
    const Profile* profile=nullptr;for(const auto& v:profiles)if(v.id==d.id){profile=&v;break;}
    if(!profile)return false;
    const bool wrath=profile->kind==1,focused=profile->kind==2;
    const auto u=[&](uint32_t c){return t.spells->getUInt32(row,c);};
    const auto i=[&](uint32_t c){return t.spells->getInt32(row,c);};
    const auto f=[&](uint32_t c){return t.spells->getFloat(row,c);};
    if(d.allowableClasses!=(wrath?1u:8u) || u(4)!=(profile->kind==3?448u:464u) ||
       u(71)!=6 || u(72) || u(73) || u(95)!=42 || u(86)!=1 || u(89) || u(74)!=1 || f(77)!=0 ||
       i(80)!=(focused?1:-1) || u(110)!=(focused?22u:0u) || u(113) || u(116)!=profile->child ||
       u(34)!=(focused?0x14u:4u) || u(35)!=(wrath?100u:focused?(profile->rank==3?100u:33u*profile->rank):20u) ||
       u(36) || u(49)>1 || u(29) || u(30) || u(31) || u(32) || u(33) ||
       u(41) || u(42) || u(43) || u(44) || u(45) || u(204) || u(205) || u(206) || u(226) ||
       u(83) || u(92) || u(98) || u(104) || u(107) || f(119)!=0 ||
       i(68)!=(wrath?2:-1) || u(69)!=(wrath?173555u:0u) || u(70) ||
       u(208)!=(profile->kind==3?8u:0u) || u(209) || u(210) || u(211))return false;
    for(unsigned k=5;k<28;++k)if(u(k))return false;
    for(unsigned k=50;k<68;++k)if(u(k))return false;
    for(unsigned k=122;k<131;++k)if(u(k)!=(focused&&k==122?1048832u:0u))return false;
    const auto cast=ClientSpellTables::lookup(t.castIndex,u(28));
    if(cast<0 || t.casts->getInt32(cast,1) || t.casts->getInt32(cast,2))return false;
    if(u(40)) {
        const auto duration=ClientSpellTables::lookup(t.durationIndex,u(40));
        if(!wrath || duration<0 || t.durations->getInt32(duration,1)!=-1 ||
           t.durations->getInt32(duration,2) || t.durations->getInt32(duration,3)!=-1)return false;
    }
    const auto child=ClientSpellTables::lookup(t.spellIndex,profile->child);if(child<0)return false;
    const auto c=[&](uint32_t col){return t.spells->getUInt32(child,col);};
    const auto ci=[&](uint32_t col){return t.spells->getInt32(child,col);};
    const auto cf=[&](uint32_t col){return t.spells->getFloat(child,col);};
    if(c(71)!=30 || c(72) || c(73) || c(74)!=1 || cf(77)!=0 || ci(80)<0 || ci(80)>999 ||
       c(86)!=1 || c(89) || c(95) || c(98) || c(110)!=(wrath?1u:3u) || c(113) || c(116) ||
       c(34) || c(36) || c(40) || c(42) || c(43) || c(44) || c(45) || c(204) || c(226) ||
       ci(68)!=-1 || c(69) || c(70) || c(208) || c(209) || c(210) || c(211) || c(46)!=1 || c(28)!=1 ||
       (c(4)&64u) || cf(119)!=0)return false;
    for(unsigned k=5;k<28;++k)if(c(k))return false;
    for(unsigned k=50;k<68;++k)if(c(k))return false;
    const auto childCast=ClientSpellTables::lookup(t.castIndex,c(28));
    const auto childRange=ClientSpellTables::lookup(t.rangeIndex,c(46));
    if(childCast<0 || childRange<0 || t.casts->getInt32(childCast,1) || t.casts->getInt32(childCast,2) ||
       t.ranges->getFloat(childRange,2)!=0 || t.ranges->getFloat(childRange,4)!=0)return false;
    LocalProcDefinition proc;proc.effect=LocalProcEffect::RestorePower;proc.spellId=profile->child;
    proc.resourceType=wrath?1:3;proc.amount=uint32_t(ci(80)+1);proc.schoolMask=c(225);
    if(wrath){if(proc.amount%10)return false;proc.amount/=10;proc.ppm=float(profile->rank*3);}
    proc.flags=profile->kind==3?0x800000u:u(34);proc.chance=uint8_t(u(35));
    if(focused){proc.hitMask=LocalProcHitCritical;proc.spellTypeMask=1;proc.allowTriggered=true;proc.attributesMask=LocalProcTriggeredCanProc;}
    d.proc=proc;d.passive=true;d.buffSelfOnly=true;
    // Equipped weapon requirements are checked against the hand which generated
    // the proc event; an unrelated equipped item cannot satisfy Unbridled Wrath.
    d.requiredItemClass=int8_t(i(68));d.requiredItemSubclasses=u(69);d.requiredInventoryTypes=u(70);
    if(!validLocalProc(d)){d.proc={};return false;}
    d.unsupportedReason.clear();return true;
}

/// The next rank the reference's own chain names (spell_ranks, the authority
/// SpellMgr::LoadSpellRanks reads), else the client's SkillLineAbility column
/// 8 for a spell the table does not list. Where both are present the table
/// wins: the reference never reads the column (audit section 7.3, 10.3).
inline uint32_t localReferenceNextRank(uint32_t id,uint32_t clientSuperceded) {
    return localSpellRankRow(id)?localSpellRankNext(id):clientSuperceded;
}
/// SpellInfo::GetFirstRankSpell()->Id for every definition, in the reference's
/// order of authority: spell_ranks (SpellMgr::LoadSpellRanks), then Talent.dbc
/// rank 1 (LoadSpellTalentRanks; set by importClientTalents before this runs),
/// then the root of the linked supercededBySpell chain (the client column and
/// reviewed profiles, for chains the table does not list), then the spell
/// itself. Idempotent: a first rank already assigned is kept.
inline void assignLocalSpellRankIdentity(std::vector<LocalSpellDefinition>& spells) {
    std::map<uint32_t,uint32_t> predecessor; // next -> previous, from the linked chain
    for(const auto& d:spells)if(d.supercededBySpell&&d.supercededBySpell!=d.id)predecessor.emplace(d.supercededBySpell,d.id);
    for(auto& d:spells) {
        if(d.firstRankSpell)continue;
        if(const auto first=localSpellRankFirst(d.id)){d.firstRankSpell=first;continue;}
        uint32_t root=d.id;
        for(size_t depth=0;depth<2048;++depth) {
            const auto it=predecessor.find(root);
            if(it==predecessor.end()||it->second==d.id)break;
            root=it->second;
        }
        d.firstRankSpell=root;
    }
}
inline void importClientTalents(LocalSpellImport& out,const pipeline::DBCFile* talents,const pipeline::DBCFile* tabs,
        const pipeline::DBCFile* spells,const pipeline::DBCFile* ranges,const pipeline::DBCFile* casts,
        const pipeline::DBCFile* durations,const pipeline::DBCFile* icons,const pipeline::DBCFile* runes,const pipeline::DBCFile* radii=nullptr){
    if(!talents || !tabs || !spells || !ranges || !casts || !durations || !talents->isLoaded() || !tabs->isLoaded() ||
       talents->getFieldCount()!=23 || tabs->getFieldCount()<24 || spells->getFieldCount()!=234)return;
    detail::ClientSpellTables t;t.spells=spells;t.ranges=ranges;t.casts=casts;t.durations=durations;t.icons=icons;t.runeCosts=runes;
    if(radii&&radii->isLoaded()&&radii->getFieldCount()==4){t.radii=radii;detail::ClientSpellTables::buildIndex(radii,t.radiusIndex);}
    detail::ClientSpellTables::buildIndex(spells,t.spellIndex);
    detail::ClientSpellTables::buildIndex(ranges,t.rangeIndex);detail::ClientSpellTables::buildIndex(casts,t.castIndex);
    detail::ClientSpellTables::buildIndex(durations,t.durationIndex);detail::ClientSpellTables::buildIndex(icons,t.iconIndex);detail::ClientSpellTables::buildIndex(runes,t.runeCostIndex);
    std::vector<std::pair<uint32_t,uint32_t>> rows;detail::ClientSpellTables::buildIndex(spells,rows);
    std::map<uint32_t,uint32_t> masks;for(uint32_t row=0;row<tabs->getRecordCount();++row)masks[tabs->getUInt32(row,0)]=tabs->getUInt32(row,20);
    for(uint32_t row=0;row<talents->getRecordCount() && out.spells.size()<8192;++row){
        const auto mask=masks[talents->getUInt32(row,1)];if(!mask)continue;
        for(uint8_t rank=1;rank<=5;++rank){
            const auto id=talents->getUInt32(row,3+rank);if(!id)continue;
            const auto source=detail::ClientSpellTables::lookup(rows,id);if(source<0)continue;
            LocalSpellDefinition d;d.id=id;d.allowableClasses=mask;d.clientSpell=true;
            detail::decodeClientSpell(t,uint32_t(source),d);
            d.talentId=talents->getUInt32(row,0);d.talentTab=talents->getUInt32(row,1);d.talentRank=rank;d.talentRow=uint8_t(talents->getUInt32(row,2));
            // SpellMgr::LoadSpellTalentRanks: a talent's rank-1 spell is the first
            // rank of every rank of that talent, unless spell_ranks names it.
            d.firstRankSpell=localSpellRankFirst(id)?localSpellRankFirst(id):talents->getUInt32(row,4);
            for(size_t k=0;k<3;++k){d.talentPrerequisites[k]=talents->getUInt32(row,13+k);d.talentPrerequisiteRanks[k]=uint8_t(talents->getUInt32(row,16+k));}
            const auto u=[&](uint32_t col){return spells->getUInt32(source,col);};
            // Legacy flat health/physical armor profile. Reviewed family-based
            // pushback and cast/cost modifier profiles are decoded below.
            if(u(4)&64u){
                d.passive=true;bool valid=true,any=false;
                for(uint32_t k=0;k<3;++k)if(u(71+k)){
                    const auto amount=int64_t(spells->getInt32(source,80+k))+1;
                    if(u(71+k)!=6 || u(86+k)!=1 || u(89+k) || u(116+k) || amount<0 || amount>100000 || u(74+k)>1 || spells->getFloat(source,77+k)!=0){valid=false;continue;}
                    if(u(95+k)==34){d.passiveHealth+=uint32_t(amount);any=true;}
                    else if(u(95+k)==22 && u(110+k)==1){d.passiveArmor+=uint32_t(amount);any=true;}
                    else valid=false;
                }
                if(valid && any && !u(spell335::ProcFlags) && !u(spell335::ProcCharges) &&
                   u(spell335::StackAmount)<=1 && !u(20) && !u(21) && !u(22) && !u(23) &&
                   !u(24) && !u(25) && !u(26) && !u(27) && !u(12) && !u(13))d.unsupportedReason.clear();
                else if(d.unsupportedReason.empty())d.unsupportedReason="Passive talent effect is not implemented";
            }
            // Furor and Tactical Mastery are talents, so the legacy passive gate
            // above re-rejects what decodeClientSpell already accepted. Re-run
            // the reviewed profile here, the way every other talent decoder in
            // this chain does.
            decodeClientFormResourceTalent(t,uint32_t(source),d);
            decodeClientPushbackTalent(t,uint32_t(source),d);
            decodeClientCastModifierTalent(t,uint32_t(source),d);
            decodeClientRegenerationTalent(t,uint32_t(source),d);
            decodeClientResourceProcTalent(t,uint32_t(source),d);
            decodeClientCrueltyTalent(t,uint32_t(source),d);
            decodeClientProgressionTalent(t,uint32_t(source),d);
            if((d.talentId==796||d.talentId==805||d.talentId==794||d.talentId==807||d.talentId==798)&&
               !decodeClientFeralProgressionTalent(t,uint32_t(source),d))
                d.unsupportedReason="Unreviewed Feral progression source profile";
            if((d.talentId==821||d.talentId==824||d.talentId==826)&&!decodeClientDruidProgressionTalent(t,uint32_t(source),d))
                d.unsupportedReason="Unreviewed Druid progression source profile";
            if((d.talentId==2250||d.talentId==1581||d.talentId==1657||d.talentId==661||d.talentId==165)&&
               !decodeClientWarriorProgressionTalent(t,uint32_t(source),d))
                d.unsupportedReason="Unreviewed Warrior progression source profile";
            if(localWarriorTalentReviewed(d.talentId)&&!decodeClientWarriorTalent(t,uint32_t(source),d))
                d.unsupportedReason="Unreviewed Warrior talent source profile";
            if(d.talentId==80&&!decodeClientArcaneStability(t,uint32_t(source),d))
                d.unsupportedReason="Unreviewed Arcane Stability source profile";
            decodeClientClearcastingTalent(t,uint32_t(source),d);
            decodeClientFlurryTalent(t,uint32_t(source),d);
            decodeClientPrimalFuryTalent(t,uint32_t(source),d);
            if(d.talentId==24&&!decodeClientMoltenShields(t,uint32_t(source),d))d.unsupportedReason="Unreviewed Molten Shields source profile";
            if(d.talentId==34&&!decodeClientIgniteTalent(t,uint32_t(source),d))d.unsupportedReason="Unreviewed Ignite source profile";
            if(d.id==34950||d.id==34954)
                if(!decodeClientGoForTheThroat(t,uint32_t(source),d))
                    d.unsupportedReason="Unreviewed Go for the Throat source profile";
            if((d.talentId==2083||d.talentId==1643||d.talentId==616||d.talentId==617||d.talentId==1690||
                d.talentId==1692||d.talentId==901||d.talentId==2054)&&!decodeClientStormstrike(t,uint32_t(source),d))
                d.unsupportedReason="Unreviewed Enhancement progression source profile";
            out.audit.push_back({d.id,d.allowableClasses,true,d.unsupportedReason.empty()?"Supported decoder; imported":d.unsupportedReason});
            auto existing=std::find_if(out.spells.begin(),out.spells.end(),[&](const auto& v){return v.id==id;});
            if(existing==out.spells.end())out.spells.push_back(std::move(d));else *existing=std::move(d);
        }
    }
    // Internal aura records are retained only through an admitted parent. They
    // are never class/talent decoder admissions or independently trainable spells.
    auto parent=std::find_if(out.spells.begin(),out.spells.end(),[](const auto& d){return d.id==23881;});
    if(parent!=out.spells.end() && parent->unsupportedReason.empty() && parent->triggeredAuraSpellId==23885) {
        const auto source=detail::ClientSpellTables::lookup(rows,23881);
        LocalSpellDefinition aura;
        if(source>=0 && decodeClientBloodthirst(t,uint32_t(source),*parent,&aura)) {
            const auto existing=std::find_if(out.spells.begin(),out.spells.end(),[](const auto& d){return d.id==23885;});
            if(existing!=out.spells.end())*existing=std::move(aura);
            else if(out.spells.size()<8192)out.spells.push_back(std::move(aura));
            else {
                parent->unsupportedReason="Internal Bloodthirst aura exceeds definition capacity";
                for(auto& observed:out.audit)if(observed.id==23881)observed.status=parent->unsupportedReason;
            }
        }
    }
    // Append after iteration: vector growth must not invalidate a parent reference.
    std::vector<LocalSpellDefinition> igniteChildren;
    for(auto& talent:out.spells)if(talent.unsupportedReason.empty()&&talent.proc.effect==LocalProcEffect::Ignite){
        const auto source=detail::ClientSpellTables::lookup(rows,talent.id);LocalSpellDefinition child;
        if(source<0||!decodeClientIgniteTalent(t,uint32_t(source),talent,&child)||out.spells.size()>=8192){
            talent.unsupportedReason="Internal Ignite closure unavailable";
            for(auto& observed:out.audit)if(observed.id==talent.id)observed.status=talent.unsupportedReason;
        }else if(igniteChildren.empty())igniteChildren.push_back(std::move(child));
    }
    for(auto& child:igniteChildren){
        auto existing=std::find_if(out.spells.begin(),out.spells.end(),[&](const auto& d){return d.id==child.id;});
        if(existing==out.spells.end())out.spells.push_back(std::move(child));else *existing=std::move(child);
    }
    std::vector<LocalSpellDefinition> flurryChildren;
    for(auto& talent:out.spells)if(talent.unsupportedReason.empty()&&talent.proc.effect==LocalProcEffect::ApplyOwnerAura) {
        const auto source=detail::ClientSpellTables::lookup(rows,talent.id);LocalSpellDefinition aura;
        if(source<0||!(talent.clearcastingProfile?
           decodeClientClearcastingTalent(t,uint32_t(source),talent,&aura):
           talent.talentId==661?decodeClientWarriorProgressionTalent(t,uint32_t(source),talent,&aura):
           decodeClientFlurryTalent(t,uint32_t(source),talent,&aura))||
           out.spells.size()+flurryChildren.size()>=8192) {
            talent.unsupportedReason="Internal proc aura closure is unavailable";
            for(auto& observed:out.audit)if(observed.id==talent.id)observed.status=talent.unsupportedReason;
        } else flurryChildren.push_back(std::move(aura));
    }
    for(auto& aura:flurryChildren) {
        const auto existing=std::find_if(out.spells.begin(),out.spells.end(),[&](const auto& d){return d.id==aura.id;});
        if(existing!=out.spells.end())*existing=std::move(aura);else out.spells.push_back(std::move(aura));
    }
    // Reviewed Warrior proc talents: the spell each one triggers, built from its
    // own pinned record. A talent whose child does not match stays blocked.
    std::vector<LocalSpellDefinition> warriorChildren;
    for(auto& talent:out.spells)if(talent.unsupportedReason.empty()&&talent.warriorProc) {
        LocalSpellDefinition child;
        if(!decodeClientWarriorProcChild(t,talent,child)||out.spells.size()+warriorChildren.size()>=8192) {
            talent.unsupportedReason="Unreviewed Warrior proc child profile";
            for(auto& observed:out.audit)if(observed.id==talent.id)observed.status=talent.unsupportedReason;
        } else if(child.id&&std::none_of(warriorChildren.begin(),warriorChildren.end(),[&](const auto& c){return c.id==child.id;}))
            warriorChildren.push_back(std::move(child));
    }
    for(auto& child:warriorChildren) {
        const auto existing=std::find_if(out.spells.begin(),out.spells.end(),[&](const auto& d){return d.id==child.id;});
        if(existing!=out.spells.end())*existing=std::move(child);else out.spells.push_back(std::move(child));
    }
    std::vector<LocalSpellDefinition> petPowerChildren;
    for(auto& talent:out.spells)if(talent.unsupportedReason.empty()&&talent.proc.effect==LocalProcEffect::RestorePetPower) {
        const auto source=detail::ClientSpellTables::lookup(rows,talent.id);LocalSpellDefinition child;
        if(source<0||!decodeClientGoForTheThroat(t,uint32_t(source),talent,&child)||
           out.spells.size()+petPowerChildren.size()>=8192) {
            talent.unsupportedReason="Internal owned-creature energize closure is unavailable";
            for(auto& observed:out.audit)if(observed.id==talent.id)observed.status=talent.unsupportedReason;
        } else petPowerChildren.push_back(std::move(child));
    }
    for(auto& child:petPowerChildren) {
        const auto existing=std::find_if(out.spells.begin(),out.spells.end(),[&](const auto& d){return d.id==child.id;});
        if(existing!=out.spells.end())*existing=std::move(child);else out.spells.push_back(std::move(child));
    }
    std::vector<LocalSpellDefinition> stormstrikeChildren;
    for(auto& talent:out.spells)if(talent.unsupportedReason.empty()&&(talent.stormstrikeProfile==1||talent.stormstrikeProfile==4)) {
        const auto source=detail::ClientSpellTables::lookup(rows,talent.id);
        std::array<LocalSpellDefinition,3> children{};
        if(source<0||!decodeClientStormstrike(t,uint32_t(source),talent,&children)||out.spells.size()+stormstrikeChildren.size()+3>8192) {
            talent.unsupportedReason="Internal Stormstrike spell closure is unavailable";
            for(auto& observed:out.audit)if(observed.id==talent.id)observed.status=talent.unsupportedReason;
        } else for(auto& child:children)if(child.id)stormstrikeChildren.push_back(std::move(child));
    }
    for(auto& child:stormstrikeChildren) {
        const auto existing=std::find_if(out.spells.begin(),out.spells.end(),[&](const auto& d){return d.id==child.id;});
        if(existing!=out.spells.end())*existing=std::move(child);else out.spells.push_back(std::move(child));
    }
    std::sort(out.spells.begin(),out.spells.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    // Do not sell a passive whose entire affected spell set is still blocked.
    // Re-evaluated after every import, so adding an executable target unlocks
    // the profile without authored spell-ID exceptions or spending dead points.
    for(auto& talent:out.spells)if(talent.passivePushbackPct && talent.unsupportedReason.empty()) {
        const bool hasTarget=std::any_of(out.spells.begin(),out.spells.end(),[&](const auto& cast) {
            if(cast.passive || !cast.unsupportedReason.empty() || !cast.castTimeMs ||
               !(cast.interruptFlags&2u) || cast.noPushback || (cast.interruptFlags&0x10u) ||
               !(cast.allowableClasses&talent.allowableClasses) || cast.spellFamily!=talent.spellFamily)return false;
            for(unsigned k=0;k<3;++k)if(cast.spellFamilyFlags[k]&talent.pushbackSpellMask[k])return true;
            return false;
        });
        if(!hasTarget) {
            talent.unsupportedReason="Affected pushback spells are not implemented yet";
            for(auto& row:out.audit)if(row.talent && row.id==talent.id)row.status=talent.unsupportedReason;
        }
    }
    for(auto& talent:out.spells)if(talent.passive&&talent.unsupportedReason.empty()&&talent.passiveSchoolThreatMask) {
        const bool relevant=std::any_of(out.spells.begin(),out.spells.end(),[&](const auto& cast){return !cast.passive&&cast.unsupportedReason.empty()&&
            (cast.allowableClasses&talent.allowableClasses)&&(cast.schoolMask&talent.passiveSchoolThreatMask)&&
            (cast.damage||cast.heal||cast.periodicDamage||cast.periodicHeal);});
        if(!relevant){talent.unsupportedReason="Affected school-threat spells are not implemented yet";for(auto& row:out.audit)if(row.talent&&row.id==talent.id)row.status=talent.unsupportedReason;}
    }
    for(auto& talent:out.spells)if(talent.passive && talent.unsupportedReason.empty() &&
        std::any_of(talent.passiveCastModifiers.begin(),talent.passiveCastModifiers.end(),[](const auto& m){return m.active;})) {
        const bool hasTarget=std::any_of(out.spells.begin(),out.spells.end(),[&](const auto& cast) {
            if(cast.passive || !cast.unsupportedReason.empty() || cast.spellFamily!=talent.spellFamily ||
               !(cast.allowableClasses&talent.allowableClasses))return false;
            for(const auto& mod:talent.passiveCastModifiers)if(mod.active) {
                const bool damageProc=cast.proc.effect==LocalProcEffect::DamageAttacker||cast.proc.effect==LocalProcEffect::MeleeDamageShield;
                if((mod.operation==0&&damageProc)||((mod.operation==2||mod.operation==3)&&cast.proc.effect!=LocalProcEffect::None)) {
                    if(cast.proc.spellFamily==talent.spellFamily)
                        for(unsigned k=0;k<3;++k)if(cast.proc.spellFamilyFlags[k]&mod.mask[k])return true;
                }
                // Aura::CalcProcChance passes the PROC AURA's own id, so a
                // chance/PPM modifier matches the proc's family and flags, never
                // the cast's. Without this arm both operations fall through to
                // the cooldown case and every rank is rejected with a misleading
                // reason; with it they are rejected for the honest one.
                if(mod.operation==18||mod.operation==26) {
                    if(cast.proc.effect==LocalProcEffect::None)continue;
                    if(mod.operation==18?!(cast.proc.chance||cast.proc.ppm>0):!(cast.proc.ppm>0))continue;
                    if(cast.proc.spellFamily!=talent.spellFamily)continue;
                    for(unsigned k=0;k<3;++k)if(cast.proc.spellFamilyFlags[k]&mod.mask[k])return true;
                    continue;
                }
                const bool relevant=mod.operation==2?(cast.damage||cast.heal||cast.periodicDamage||cast.periodicHeal):
                    mod.operation==0?(cast.damage||cast.heal):
                    mod.operation==1?cast.durationMs!=0:
                    mod.operation==3?cast.directEffectSlot==0||cast.periodicEffectSlot==0:
                    mod.operation==5?cast.range>0:
                    mod.operation==7?(cast.damage||cast.heal||cast.periodicDamage||cast.periodicHeal):
                    mod.operation==8?cast.buffArmor>0:
                    mod.operation==10?cast.castTimeMs!=0:
                    mod.operation==11?(cast.cooldownMs || (cast.categoryCooldownMs&&!cast.noCategoryCooldownMods)):
                    // A ward's second effect is the Molten Shields reflect
                    // (mage.cpp), carried by wardProfile rather than a slot.
                    mod.operation==12?cast.directEffectSlot==1||cast.periodicEffectSlot==1||(cast.wardProfile&&talent.moltenShieldsChancePct):
                    mod.operation==14?(cast.resourceType==0||cast.resourceType==1||cast.resourceType==3||cast.resourceType==6)&&(cast.mana||cast.manaPercent):
                    mod.operation==16?(cast.damage||cast.periodicDamage||cast.controlProfile||cast.snarePercent):
                    mod.operation==21?cast.globalCooldownMs!=0:
                    mod.operation==22?(cast.periodicDamage||cast.periodicHeal):
                    mod.operation==23?cast.directEffectSlot==2||cast.periodicEffectSlot==2:false;
                // Warrior abilities whose amounts these operations reach through
                // other consumers: weapon strikes (damage, critical chance, all
                // effects), Charge and Bloodrage rage, the area debuffs of
                // Demoralizing Shout and Thunder Clap, and an area's radius.
                const bool warriorRelevant=talent.spellFamily==4&&(
                    ((mod.operation==0||mod.operation==7)&&cast.weaponDamage)||
                    (mod.operation==8&&(cast.weaponDamage||cast.chargeRage||cast.targetDebuffAttackPower<0))||
                    (mod.operation==3&&cast.energizeRage)||
                    (mod.operation==6&&cast.areaRadius>0)||
                    (mod.operation==12&&cast.targetDebuffMeleeHasteSlot==1));
                if(!relevant&&!warriorRelevant)continue;
                for(unsigned k=0;k<3;++k)if(cast.spellFamilyFlags[k]&mod.mask[k])return true;
            }
            return false;
        });
        if(!hasTarget) {
            talent.unsupportedReason="Affected spells are not implemented for this modifier";
            for(auto& row:out.audit)if(row.talent && row.id==talent.id)row.status=talent.unsupportedReason;
        }
    }
    const auto supported=std::count_if(out.spells.begin(),out.spells.end(),[](const auto& d){return d.talentId && d.unsupportedReason.empty();});
    out.diagnostic+=" Supported talent ranks: "+std::to_string(supported)+"; unsupported effects remain blocked.";
    assignLocalSpellRankIdentity(out.spells);
}
} // namespace detail

// 3.3.5a (12340) only. Column meanings agree with the existing WotLK layout and
// AzerothCore's SpellEntry/Spell Effects Reference. No client records, text,
// artwork or numeric spell data ship with this source; everything below is read
// from the player's own installation at runtime.
//
// Without the last three tables this behaves exactly as it always did: the
// twenty hand-picked starter abilities and nothing else. With them, the class
// trainer has a real progression to sell and the profession trainer has real
// recipes, both selected from the client's own SkillLineAbility.dbc rather than
// from a list written here.
inline LocalSpellImport importClientStarterSpells(
    const pipeline::DBCFile* spells, const pipeline::DBCFile* ranges,
    const pipeline::DBCFile* casts, const pipeline::DBCFile* durations,
    const pipeline::DBCFile* icons = nullptr,
    const pipeline::DBCFile* skillLineAbility = nullptr,
    const pipeline::DBCFile* skillLine = nullptr,
    const pipeline::DBCFile* talents = nullptr,
    const pipeline::DBCFile* runeCosts = nullptr,
    const pipeline::DBCFile* radii = nullptr,
    const pipeline::DBCFile* summonProperties = nullptr) {
    struct Starter { uint32_t id; uint8_t cls; };
    constexpr Starter starters[] = {{78,1},{635,2},{21084,2},{75,3},{2973,3},
        {1752,4},{2098,4},{585,5},{2050,5},{45462,6},{45477,6},{45902,6},
        {403,7},{331,7},{133,8},{168,8},{686,9},{687,9},{5176,11},{5185,11}};
    const auto valid=[](const pipeline::DBCFile* table, uint32_t fields) {
        return table && table->isLoaded() && table->getFieldCount() == fields && table->getRecordSize() == fields * 4;
    };
    LocalSpellImport out;
    detail::ClientSpellTables tables;
    tables.spells=spells;tables.ranges=ranges;tables.casts=casts;tables.durations=durations;
    // Range has two localized names and their locale masks: 40 fields in 12340.
    tables.ready=valid(spells,234)&&valid(ranges,40)&&valid(casts,4)&&valid(durations,4);
    if(tables.ready) {
        detail::ClientSpellTables::buildIndex(spells,tables.spellIndex);
        detail::ClientSpellTables::buildIndex(ranges,tables.rangeIndex);
        detail::ClientSpellTables::buildIndex(casts,tables.castIndex);
        detail::ClientSpellTables::buildIndex(durations,tables.durationIndex);
        if(valid(radii,4)){tables.radii=radii;detail::ClientSpellTables::buildIndex(radii,tables.radiusIndex);}
        // SummonProperties.dbc: 6 fields in 12340 (id, category, faction, type, slot, flags).
        if(valid(summonProperties,6)){tables.summons=summonProperties;detail::ClientSpellTables::buildIndex(summonProperties,tables.summonIndex);}
        if(valid(runeCosts,5)) {tables.runeCosts=runeCosts;detail::ClientSpellTables::buildIndex(runeCosts,tables.runeCostIndex);}
        if(valid(icons,2)) {tables.icons=icons;detail::ClientSpellTables::buildIndex(icons,tables.iconIndex);}
    }

    // --- Which spells are class abilities, and which are recipes -------------
    //
    // SkillLine.dbc says what a skill line is for; SkillLineAbility.dbc says
    // which spells belong to it and, for a trade skill, what it costs in skill
    // to learn and where it stops paying. Neither is server data.
    std::vector<detail::AbilityRow> wanted;
    std::vector<uint32_t> talentSpells;
    if(tables.ready&&valid(skillLineAbility,14)&&skillLine&&skillLine->isLoaded()&&skillLine->getFieldCount()>=38) {
        std::map<uint32_t,uint32_t> lineCategory;
        for(uint32_t row=0;row<skillLine->getRecordCount();++row) {
            const auto id=skillLine->getUInt32(row,0);
            const auto category=skillLine->getUInt32(row,1);
            if(id&&(category==kLocalSkillCategoryClass||category==kLocalSkillCategoryProfession||
                    category==kLocalSkillCategorySecondary)) lineCategory.emplace(id,category);
        }
        // Talent-granted spells are excluded where Talent.dbc is available: a
        // talent is bought with talent points, and a class trainer offering one
        // for gold would be selling something it does not own. Without the
        // table they are simply left in - a wrong offer, never a wrong effect.
        if(talents&&talents->isLoaded()&&talents->getFieldCount()>=13) {
            const uint32_t first=4, last=std::min<uint32_t>(talents->getFieldCount(),13);
            for(uint32_t row=0;row<talents->getRecordCount();++row)
                for(uint32_t col=first;col<last;++col)
                    if(const auto id=talents->getUInt32(row,col)) talentSpells.push_back(id);
            std::sort(talentSpells.begin(),talentSpells.end());
            talentSpells.erase(std::unique(talentSpells.begin(),talentSpells.end()),talentSpells.end());
        }
        for(uint32_t row=0;row<skillLineAbility->getRecordCount();++row) {
            detail::AbilityRow entry;
            entry.skillId=skillLineAbility->getUInt32(row,1);
            entry.spellId=skillLineAbility->getUInt32(row,2);
            const auto line=lineCategory.find(entry.skillId);
            // Wand Shoot belongs to the source weapon skill, not a class
            // category. Admit only its exact shared caster access row.
            const bool wandShoot=entry.spellId==5019&&entry.skillId==228&&
                skillLineAbility->getUInt32(row,3)==0&&skillLineAbility->getUInt32(row,4)==400&&
                skillLineAbility->getUInt32(row,5)==0&&skillLineAbility->getUInt32(row,6)==0&&
                skillLineAbility->getUInt32(row,7)==1&&skillLineAbility->getUInt32(row,8)==0;
            if(!entry.spellId||(line==lineCategory.end()&&!wandShoot)) continue;
            entry.category=wandShoot?kLocalSkillCategoryClass:line->second;
            entry.classMask=skillLineAbility->getUInt32(row,4);
            if(entry.category!=kLocalSkillCategoryClass) entry.recipeAccess.push_back({
                skillLineAbility->getUInt32(row,3),entry.classMask,
                skillLineAbility->getUInt32(row,5),skillLineAbility->getUInt32(row,6)});
            entry.requiredSkill=uint16_t(std::min<uint32_t>(skillLineAbility->getUInt32(row,7),65535));
            entry.supercededBy=skillLineAbility->getUInt32(row,8);
            entry.trivialHigh=uint16_t(std::min<uint32_t>(skillLineAbility->getUInt32(row,10),65535));
            entry.trivialLow=uint16_t(std::min<uint32_t>(skillLineAbility->getUInt32(row,11),65535));
            if(entry.category==kLocalSkillCategoryClass) {
                // A class ability with no class mask belongs to everyone with
                // the skill line, which for a class line means the class itself;
                // one with a mask names it outright. Without either there is
                // nothing to gate a trainer on, so it is left out.
                if(!entry.classMask) {
                    // Some Water Shield ranks have no row class mask. Resolve
                    // only reviewed profiles in their authored shaman skill line.
                    const auto* profile=detail::reactiveShieldProfile(entry.spellId);
                    if(profile && entry.skillId==(profile->water?374u:373u))entry.classMask=64;
                    else if(entry.spellId==48574&&entry.skillId==134)entry.classMask=1024; // Final Rake rank: authored feral skill line, omitted row mask.
                    else continue;
                }
                if(std::binary_search(talentSpells.begin(),talentSpells.end(),entry.spellId)) continue;
            }
            wanted.push_back(entry);
        }
        std::sort(wanted.begin(),wanted.end(),[](const auto& a,const auto& b) {
            if(a.spellId!=b.spellId) return a.spellId<b.spellId;
            return a.skillId<b.skillId;
        });
        // One entry per spell. A spell in several lines of the same kind keeps
        // the first line and the union of the class masks.
        std::vector<detail::AbilityRow> merged;
        for(auto& entry:wanted) {
            if(!merged.empty()&&merged.back().spellId==entry.spellId) {
                if(entry.category!=kLocalSkillCategoryClass) {
                    auto& prior=merged.back();
                    if(prior.skillId!=entry.skillId || prior.requiredSkill!=entry.requiredSkill ||
                       prior.trivialHigh!=entry.trivialHigh || prior.trivialLow!=entry.trivialLow || prior.recipeAccess.size()>=16)
                        prior.ambiguousRecipe=true;
                    else prior.recipeAccess.insert(prior.recipeAccess.end(),entry.recipeAccess.begin(),entry.recipeAccess.end());
                }
                merged.back().classMask|=entry.classMask;
                if(!merged.back().supercededBy) merged.back().supercededBy=entry.supercededBy;
                continue;
            }
            merged.push_back(entry);
        }
        wanted=std::move(merged);
    }

    // --- One pass over Spell.dbc --------------------------------------------
    std::map<uint32_t,uint32_t> selectedRows;
    std::vector<uint32_t> mountIds;
    for(const auto& item:kLocalAuctionItems)if(item.mountSpell)mountIds.push_back(item.mountSpell);
    std::sort(mountIds.begin(),mountIds.end());mountIds.erase(std::unique(mountIds.begin(),mountIds.end()),mountIds.end());
    std::vector<std::pair<uint32_t,uint32_t>> mountRows;
    std::vector<std::pair<uint32_t,uint32_t>> abilityRows, recipeRows;  // spell id -> Spell.dbc row
    if(tables.ready) for(uint32_t row=0;row<spells->getRecordCount();++row) {
        const auto id=spells->getUInt32(row,0);
        if(std::binary_search(mountIds.begin(),mountIds.end(),id))mountRows.emplace_back(id,row);
        for(const auto& candidate:starters) if(id==candidate.id) selectedRows.emplace(id,row);
        const auto found=std::lower_bound(wanted.begin(),wanted.end(),id,
            [](const detail::AbilityRow& entry,uint32_t key){return entry.spellId<key;});
        if(found==wanted.end()||found->spellId!=id) continue;
        if(found->category==kLocalSkillCategoryClass) abilityRows.emplace_back(id,row);
        else recipeRows.emplace_back(id,row);
    }

    size_t supported=0;
    for(const auto& starter:starters) {
        LocalSpellDefinition d; d.id=starter.id;d.allowableClasses=1u<<(starter.cls-1);
        d.clientSpell=true;d.name="Spell #"+std::to_string(d.id);d.range=0;
        // a starter is a rank too. Its successor comes from spell_ranks
        // or, failing that, from its own SkillLineAbility row when one was read.
        {
            const auto ability=std::lower_bound(wanted.begin(),wanted.end(),starter.id,
                [](const detail::AbilityRow& a,uint32_t key){return a.spellId<key;});
            const auto clientNext=ability!=wanted.end()&&ability->spellId==starter.id?ability->supercededBy:0u;
            d.supercededBySpell=detail::localReferenceNextRank(starter.id,clientNext);
        }
        const auto found=selectedRows.find(starter.id);
        if(!tables.ready) d.unsupportedReason="WotLK spell tables missing or incompatible (Spell, SpellRange, SpellCastTimes, SpellDuration)";
        else if(found==selectedRows.end()) d.unsupportedReason="Starter spell is missing from installed Spell.dbc";
        else if(detail::decodeClientSpell(tables,found->second,d)) ++supported;
        out.audit.push_back({d.id,d.allowableClasses,false,d.unsupportedReason.empty()?"Supported decoder; imported":d.unsupportedReason});
        out.spells.push_back(std::move(d));
    }

    // Class abilities, in ascending spell id so a truncated import truncates
    // identically on every console. Only what the ruleset can actually cast is
    // kept: an ability that would refuse to fire is not a trainer's stock, and
    // holding one costs the same memory as holding a usable one.
    size_t abilitiesRejected=0;
    for(const auto& entry:abilityRows) {

        if(selectedRows.count(entry.first)) continue;   // already a starter
        const auto row=std::lower_bound(wanted.begin(),wanted.end(),entry.first,
            [](const detail::AbilityRow& a,uint32_t key){return a.spellId<key;});
        LocalSpellDefinition d; d.id=entry.first; d.clientSpell=true; d.range=0;
        d.name="Spell #"+std::to_string(d.id);
        d.allowableClasses=row->classMask;
        // the reference's rank chain first, the client column second.
        // A reviewed profile that names its own successor is checked against
        // this inside the decoder (`supercededBySpell!=next` rejects the row),
        // so a disagreement between the table and a profile would surface in
        // the audit census rather than silently pick a side.
        d.supercededBySpell=detail::localReferenceNextRank(d.id,row->supercededBy);
        // A class mount goes through the ground-mount profile, restricted to its class.
        if(LocalSpellDefinition mount;detail::decodeClientGroundMount(tables,entry.second,mount,true)) {
            mount.allowableClasses=row->classMask;
            out.audit.push_back({mount.id,mount.allowableClasses,false,"Supported decoder; imported class mount"});
            out.spells.push_back(std::move(mount));continue;
        }
        if(!detail::decodeClientSpell(tables,entry.second,d)&&!(d.id==kLocalBerserkerRage&&d.allowableClasses==1&&decodeClientBerserkerRage(tables,d))) {
            out.audit.push_back({d.id,d.allowableClasses,false,d.unsupportedReason});++abilitiesRejected;continue;
        }
        if(out.spells.size()>=kLocalMaxImportedClassAbilities+sizeof(starters)/sizeof(starters[0])){
            out.audit.push_back({d.id,d.allowableClasses,false,"Supported decoder; import capacity exceeded"});continue;
        }
        out.audit.push_back({d.id,d.allowableClasses,false,"Supported decoder; imported"});
        out.spells.push_back(std::move(d));
    }
    // Internal retaliation leaves are retained only through a complete parent
    // closure. They never enter the ordinary class admission/trainer audit.
    std::vector<LocalSpellDefinition> moltenChildren;
    for(const auto& parent:out.spells)if(parent.unsupportedReason.empty()&&parent.procCanCrit&&
            parent.mageArmorGroup&&parent.spiritCritRatingPct==35) {
        const auto source=detail::ClientSpellTables::lookup(tables.spellIndex,parent.proc.spellId);
        if(source<0)continue; // The parent decoder already requires this record.
        const auto& proc=parent.proc;
        LocalSpellDefinition child;child.id=proc.spellId;child.clientSpell=true;child.triggeredOnly=true;
        child.allowableClasses=128;child.sourceDamageClass=1;child.schoolMask=proc.schoolMask;
        child.spellFamily=proc.spellFamily;child.spellFamilyFlags=proc.spellFamilyFlags;
        child.damage=child.damageMax=proc.amount;child.baseLevel=proc.baseLevel;child.range=proc.range;
        child.name=spells->getString(uint32_t(source),136);child.iconId=spells->getUInt32(uint32_t(source),133);
        child.visualId=spells->getUInt32(uint32_t(source),131);child.resourceType=0;
        moltenChildren.push_back(std::move(child));
    }
    for(auto& child:moltenChildren)out.spells.push_back(std::move(child));
    // Arcane Blast's script aura is retained once through an admitted ordinary
    // rank, before talent dependencies inspect available Arcane spells.
    const auto blastParent=std::find_if(out.spells.begin(),out.spells.end(),[](const auto& d){return d.arcaneBlastProfile==1&&d.unsupportedReason.empty();});
    if(blastParent!=out.spells.end()){
        const auto source=detail::ClientSpellTables::lookup(tables.spellIndex,blastParent->id);
        LocalSpellDefinition child;
        if(source>=0&&decodeClientArcaneBlast(tables,uint32_t(source),*blastParent,&child)){
            const auto previous=std::find_if(out.spells.begin(),out.spells.end(),[](const auto& d){return d.id==36032;});
            if(previous==out.spells.end())out.spells.push_back(std::move(child));else *previous=std::move(child);
        }
    }
    importLocalNpcSpells(tables,out.spells);
    importLocalPetSpells(tables,out.spells);
    size_t supportedMounts=0;
    for(const auto& entry:mountRows) {
        LocalSpellDefinition mount;
        if(detail::decodeClientGroundMount(tables,entry.second,mount)) {
            out.spells.push_back(std::move(mount));++supportedMounts;
        }
    }
    std::sort(out.spells.begin(),out.spells.end(),[](const auto& a,const auto& b){return a.id<b.id;});

    // A row that some other row's aura-42 SPELL_AURA_PROC_TRIGGER_SPELL effect
    // names is a proc child, whatever SkillLineAbility.dbc says about it. The
    // reference builds its spellbook from trainer tables and never from that
    // DBC, so such a row is never offered as a learnable ability there; this
    // importer read it from the skill line and handed it over as one. Impact
    // 12355 is the visible case - a 2 s stun with no cooldown, no category
    // cooldown, no global cooldown and no cost, known by every Mage from level
    // 10. Retiring it keeps the definition, so the accepted-spell census does
    // not move; it only stops the player pressing the child directly.
    if(spells&&spells->isLoaded()) {
        std::set<uint32_t> procChildren;
        for(uint32_t row=0;row<spells->getRecordCount();++row)
            for(unsigned effect=0;effect<3;++effect) {
                if(spells->getUInt32(row,71+effect)!=6||spells->getUInt32(row,95+effect)!=42)continue;
                if(const auto child=spells->getUInt32(row,116+effect))procChildren.insert(child);
            }
        size_t retired=0;
        for(auto& d:out.spells)
            if(!d.triggeredOnly&&procChildren.count(d.id)){d.triggeredOnly=true;++retired;}
        out.procChildrenRetired=retired;
    }
    // the implementation (P05, the source audit section 7 item 6): the
    // same defect through the reference's rank table. Lightning Overload's proc
    // copies 49239/49240 and 49268/49269 are ranks 13-14 and 7-8 of the
    // spell_ranks chains rooted at 45284 and 45297 (spell_ranks.sql:2538-2555,
    // :2903-2906), the two spells the talent's script casts
    // (spell_sha_lightning_overload, spell_shaman.cpp:106-107 and :1439,
    // spell_script_names.sql:146); no trainer offers any rank of either chain
    // (trainer_spell.sql offers 49238 and 49270/49271, never these), and the
    // client itself gives the chain's first rank no class mask in
    // SkillLineAbility - only the last two ranks carry the stray mask that
    // made them "trainable" here. The rule reads exactly that: a rank of a
    // spell_ranks chain that no trainer offers any member of, whose first rank
    // the client never marks learnable and that is not a talent, is a proc
    // copy. The chain test is the reference's; the first-rank test keeps the
    // stand-ins this build still offers for parents it cannot admit (Holy
    // Shock's 25912/25914, Immolation Trap's 13797, Pounce Bleed, Blessed
    // Recovery - chains no trainer offers either, but whose own rank 1 the
    // client marks learnable) until P03 admits those parents; they are
    // recorded, not retired. Retiring keeps the definition, so the accepted
    // census does not move.
    {
        std::set<uint32_t> learnableFirstRanks;
        for(const auto& entry:wanted)if(entry.category==kLocalSkillCategoryClass&&entry.classMask)learnableFirstRanks.insert(entry.spellId);
        for(auto id:talentSpells)learnableFirstRanks.insert(id);
        size_t retired=0;
        for(auto& d:out.spells) {
            if(d.triggeredOnly||d.talentId||!d.clientSpell)continue;
            const auto first=localSpellRankFirst(d.id);
            if(!first||first==d.id||learnableFirstRanks.count(first))continue;
            if(localSpellRankChainOfferedByTrainer(first))continue;
            d.triggeredOnly=true;++retired;
        }
        out.untrainedChainRanksRetired=retired;
    }

    // Recipes. A trade-skill spell this realm can run is one that names both
    // its reagents and the item it produces; anything else - an enchant, a
    // gathering spell, a scripted one - is left out rather than made up.
    for(const auto& entry:recipeRows) {
        if(out.recipes.size()>=kLocalMaxImportedRecipes) break;
        const auto row=std::lower_bound(wanted.begin(),wanted.end(),entry.first,
            [](const detail::AbilityRow& a,uint32_t key){return a.spellId<key;});
        LocalRecipe recipe;
        recipe.spellId=entry.first;
        recipe.skillId=uint16_t(std::min<uint32_t>(row->skillId,65535));
        recipe.requiredSkill=row->requiredSkill;
        recipe.trivialHigh=row->trivialHigh;recipe.trivialLow=row->trivialLow;
        recipe.access=row->recipeAccess;
        if(row->ambiguousRecipe) recipe.unsupportedReason="Ambiguous recipe skill-line requirements";
        const auto u=[&](uint32_t col){return spells->getUInt32(entry.second,col);};
        const auto i=[&](uint32_t col){return spells->getInt32(entry.second,col);};
        // Retain the legacy first product/recipe ID for saved books, but block
        // the whole craft if another effect or malformed amount was discarded.
        for(uint32_t effect=0;effect<3;++effect) {
            if(!u(71+effect))continue;
            if(u(71+effect)!=24 || recipe.createdItemId) {
                recipe.unsupportedReason="Additional recipe effects are not implemented";continue;
            }
            recipe.createdItemId=u(107+effect);
            const int64_t amount=int64_t(i(80+effect))+1;
            recipe.createdCount=uint16_t(std::clamp<int64_t>(amount,1,1000));
            if(amount<1 || amount>1000)recipe.unsupportedReason="Invalid recipe output amount";
        }
        if(!recipe.createdItemId) continue;
        std::map<uint32_t,uint32_t> amounts;
        for(uint32_t slot=0;slot<8;++slot) {
            const auto item=i(52+slot); const auto count=i(60+slot);
            if(!item && !count)continue;
            if(item<=0||count<=0||count>1000) {
                recipe.unsupportedReason="Invalid recipe reagent item/count";continue;
            }
            amounts[uint32_t(item)]+=uint32_t(count);
        }
        for(const auto& [item,count]:amounts)recipe.reagents.push_back({item,uint16_t(count)});
        if(recipe.reagents.empty())continue;
        for(size_t slot=0;slot<recipe.tools.size();++slot)recipe.tools[slot]=u(50+slot);
        const auto toolCat = u(222);
        if (toolCat == 162 && !recipe.tools[0]) recipe.tools[0] = 5956;
        recipe.requiresSpellFocus = u(18);
        const auto name=spells->getString(entry.second,136);
        recipe.name=!name.empty()&&name.size()<=96?name:"Recipe #"+std::to_string(recipe.spellId);
        out.recipes.push_back(std::move(recipe));
    }
    std::sort(out.recipes.begin(),out.recipes.end(),[](const auto& a,const auto& b){return a.spellId<b.spellId;});

    const size_t classAbilities=out.spells.size()-(sizeof(starters)/sizeof(starters[0]))-supportedMounts;
    out.diagnostic="Client spells: "+std::to_string(supported)+" of "+
        std::to_string(sizeof(starters)/sizeof(starters[0]))+" starter abilities supported, "+
        std::to_string(classAbilities)+" further class abilities and "+
        std::to_string(out.recipes.size())+" trade recipes read from your client. Unavailable "
        "abilities show their reason in the spell list. Ground mounts supported: "+std::to_string(supportedMounts)+".";
    if(!tables.ready) out.diagnostic="WotLK spell tables missing or incompatible; starter spells unavailable. Existing saved abilities and melee combat remain available.";
    else if(wanted.empty()) out.diagnostic+=" SkillLineAbility/SkillLine were not read, so trainers have nothing beyond the starter set.";
    else if(abilitiesRejected) out.diagnostic+=" "+std::to_string(abilitiesRejected)+
        " class abilities were left out because this ruleset cannot cast them.";
    const auto blockedRecipes=std::count_if(out.recipes.begin(),out.recipes.end(),[](const auto& r){return !r.unsupportedReason.empty();});
    if(blockedRecipes)out.diagnostic+=" Retained unavailable recipes: "+std::to_string(blockedRecipes)+" (see recipe description).";
    detail::assignLocalSpellRankIdentity(out.spells);
    return out;
}
}

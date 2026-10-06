#pragma once
#include "game/local_warrior_talents_import.hpp"
#include "game/local_talents.hpp"
#include <algorithm>
#include <vector>

namespace wowee::game {
// Reviewed Warrior proc talents. The generic proc dispatcher (local_proc_rules)
// admits each proc family only through a source-reviewed profile; these
// talents run through their own bounded rules below instead, driven by the
// same combat events. Every talent rank and every spell it triggers is pinned
// to its exact 12340 record (kLocalWarriorTalentRecords and the child records
// here), so a client whose data differs keeps the talent blocked.
enum class LocalWarriorProc : uint8_t {
    None, DeepWounds, SwordSpecialization, ImprovedHamstring, Enrage, ImprovedBerserkerRage,
    ShieldSpecialization, SuddenDeath, Trauma, Bloodsurge, SwordAndBoard, WreckingCrew,
    TasteForBlood, DamageShield, Juggernaut
};
// The aura a proc leaves on its owner (warriorProcAura on the child definition).
enum class LocalWarriorProcAura : uint8_t { None, Damage, SuddenDeath, Bloodsurge, SwordAndBoard, Juggernaut };
// Index into LocalRealmPlayer::warriorProcCooldownMs.
inline constexpr size_t kLocalSwordSpecializationCooldown=0,kLocalTasteForBloodCooldown=1;
inline constexpr uint32_t kLocalWarriorProcCooldownMs=6000; // "cannot occur more than once every 6 sec"
inline constexpr uint32_t kLocalDeepWoundsPeriodic=12721,kLocalDamageShieldSpell=59653;

// The spells the proc talents trigger, plus Berserker Rage (18499), whose
// base ability is admitted here because Improved Berserker Rage builds on it.
inline constexpr LocalWarriorTalentRecord kLocalWarriorProcChildRecords[]={
    {0,0,18499,{{{2,9u},{3,31u},{4,327696u},{5,32768u},{28,1u},{29,30000u},{34,139944u},{35,100u},{38,32u},{39,32u},{40,1u},{41,1u},{46,1u},{68,4294967295u},{71,6u},{72,6u},{73,6u},{74,1u},{75,1u},{80,4294967295u},{81,4294967295u},{86,1u},{87,1u},{88,1u},{95,77u},{96,77u},{97,77u},{110,5u},{111,14u},{112,30u},{205,133u},{206,1500u},{208,4u},{209,268435456u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,12162,{{{3,15u},{4,262544u},{6,4u},{28,1u},{35,101u},{39,1u},{46,13u},{68,4294967295u},{71,3u},{86,6u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,12721,{{{3,15u},{4,16u},{6,4u},{8,256u},{10,536870912u},{28,1u},{35,101u},{39,1u},{40,32u},{46,13u},{68,4294967295u},{71,6u},{74,1u},{83,15u},{86,6u},{95,3u},{98,1000u},{208,4u},{210,16u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{229,1065353216u}}}},
    {0,0,12850,{{{3,15u},{4,262544u},{6,4u},{28,1u},{35,101u},{39,1u},{46,13u},{68,4294967295u},{71,3u},{86,6u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,12868,{{{3,15u},{4,262544u},{6,4u},{28,1u},{35,101u},{39,1u},{46,13u},{68,4294967295u},{71,3u},{86,6u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,12880,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,1u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,14201,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,3u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,14202,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,5u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,14203,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,7u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,14204,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,9u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,16459,{{{28,1u},{35,101u},{46,1u},{68,4294967295u},{71,19u},{74,1u},{86,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{229,1065353216u},{230,1065353216u},{231,1065353216u}}}},
    {0,0,23602,{{{4,262160u},{28,1u},{34,20u},{35,100u},{39,1u},{46,1u},{68,4294967295u},{71,30u},{74,1u},{80,49u},{86,1u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,23690,{{{28,1u},{35,101u},{46,1u},{68,4294967295u},{71,30u},{74,1u},{80,99u},{86,1u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u},{231,1065353216u}}}},
    {0,0,23691,{{{28,1u},{35,101u},{46,1u},{68,4294967295u},{71,30u},{74,1u},{80,199u},{86,1u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u},{231,1065353216u}}}},
    {0,0,23694,{{{3,7u},{6,4u},{28,1u},{35,101u},{40,28u},{46,13u},{68,4294967295u},{71,6u},{86,6u},{95,26u},{214,2u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{229,1065353216u},{230,1065353216u},{231,1065353216u}}}},
    {0,0,46856,{{{4,16u},{7,131072u},{28,1u},{35,101u},{39,1u},{40,3u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{75,1u},{80,14u},{81,4294967295u},{86,6u},{95,255u},{110,15u},{122,8208u},{123,8u},{125,8208u},{126,8u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,46857,{{{4,16u},{7,131072u},{28,1u},{35,101u},{39,1u},{40,3u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{75,1u},{80,29u},{81,4294967295u},{86,6u},{95,255u},{110,15u},{122,8208u},{123,8u},{125,8208u},{126,8u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,46916,{{{4,262144u},{10,64u},{28,1u},{34,16u},{35,100u},{36,1u},{40,28u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{75,1u},{80,4294967195u},{81,4294967295u},{86,1u},{95,108u},{110,10u},{122,2097152u},{208,4u},{210,16777216u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{229,1065353216u}}}},
    {0,0,50227,{{{4,16u},{10,64u},{28,1u},{34,16u},{35,100u},{36,1u},{39,1u},{40,28u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{75,1u},{80,4294967195u},{81,4294967295u},{86,1u},{95,108u},{110,14u},{123,512u},{125,1024u},{208,4u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,52437,{{{4,536870928u},{5,1024u},{28,1u},{35,101u},{36,1u},{40,1u},{46,1u},{68,4294967295u},{71,6u},{86,1u},{95,262u},{122,536870912u},{208,4u},{210,33554432u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,57518,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,1u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,57519,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,3u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,57520,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,5u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,57521,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,7u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,57522,{{{1,21u},{2,9u},{3,31u},{4,16777216u},{8,524416u},{9,8u},{28,1u},{35,101u},{38,1u},{39,1u},{40,29u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{80,9u},{86,1u},{95,79u},{110,1u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u},{230,1065353216u}}}},
    {0,0,60503,{{{4,262144u},{6,4u},{28,1u},{34,16u},{35,100u},{36,1u},{38,1u},{39,1u},{40,105u},{41,3u},{46,13u},{68,4294967295u},{71,6u},{86,1u},{95,262u},{122,4u},{208,4u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
    {0,0,65156,{{{4,16u},{28,1u},{34,69648u},{35,100u},{36,1u},{40,1u},{46,1u},{68,4294967295u},{71,6u},{74,1u},{75,1u},{76,1u},{80,24u},{81,4294967295u},{82,4294967295u},{86,1u},{95,107u},{110,7u},{122,35651584u},{125,2097152u},{128,1073741825u},{129,65536u},{208,4u},{216,1065353216u},{217,1065353216u},{218,1065353216u},{225,1u}}}},
};
template<class Tables>
inline bool localWarriorProcChildMatches(const Tables& t,uint32_t id) {
    const auto row=Tables::lookup(t.spellIndex,id);
    if(row<0)return false;
    for(const auto& r:kLocalWarriorProcChildRecords)if(r.spellId==id)return localWarriorTalentRecordMatches(t,uint32_t(row),r);
    return false;
}
template<class Tables>
inline int32_t localWarriorDurationMs(const Tables& t,uint32_t row) {
    const auto id=t.spells->getUInt32(row,40);if(!id)return 0;
    const auto duration=Tables::lookup(t.durationIndex,id);
    return duration<0?-1:t.durations->getInt32(duration,1);
}
// The talent half: kind, chance and child from the pinned rank. Called by
// decodeClientWarriorTalent once the rank record itself has matched.
template<class Tables>
inline bool decodeClientWarriorProcTalent(const Tables& t,uint32_t row,LocalSpellDefinition& d) {
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto amount=[&](uint32_t effect){return int32_t(u(80+effect))+1;};
    const uint32_t rank=d.talentRank;
    // The generic dispatcher must never see these: their own rules run them.
    d.proc={};d.secondaryProc={};
    d.warriorProcChance=uint8_t(std::min<uint32_t>(100,u(35)));
    d.warriorProcChild=u(116)?u(116):u(117);
    switch(d.talentId) {
        case 121: d.warriorProc=uint8_t(LocalWarriorProc::DeepWounds);d.warriorProcAmount=uint16_t(16*rank);
                  d.warriorProcChild=kLocalDeepWoundsPeriodic;d.warriorProcChance=100;break;
        case 123: d.warriorProc=uint8_t(LocalWarriorProc::SwordSpecialization);
                  d.requiredItemClass=2;d.requiredItemSubclasses=u(69);d.requiredInventoryTypes=0;break;
        case 129: d.warriorProc=uint8_t(LocalWarriorProc::ImprovedHamstring);break;
        case 155: d.warriorProc=uint8_t(LocalWarriorProc::Enrage);break;
        case 1541: d.warriorProc=uint8_t(LocalWarriorProc::ImprovedBerserkerRage);d.warriorProcChance=100;break;
        case 1601: d.warriorProc=uint8_t(LocalWarriorProc::ShieldSpecialization);d.passiveBlockPct=uint8_t(amount(0));break;
        case 1662: d.warriorProc=uint8_t(LocalWarriorProc::SuddenDeath);
                   d.warriorProcChance=uint8_t(amount(0));d.warriorProcAmount=uint16_t(amount(1));break;
        case 1859: d.warriorProc=uint8_t(LocalWarriorProc::Trauma);break;
        case 1866: d.warriorProc=uint8_t(LocalWarriorProc::Bloodsurge);break;
        case 1871: d.warriorProc=uint8_t(LocalWarriorProc::SwordAndBoard);break; // its Devastate crit bonus has no castable Devastate
        case 2231: d.warriorProc=uint8_t(LocalWarriorProc::WreckingCrew);break;
        case 2232: d.warriorProc=uint8_t(LocalWarriorProc::TasteForBlood);break;
        case 2246: d.warriorProc=uint8_t(LocalWarriorProc::DamageShield);d.warriorProcAmount=uint16_t(amount(0));
                   d.warriorProcChild=kLocalDamageShieldSpell;d.warriorProcChance=100;break;
        case 2283: d.warriorProc=uint8_t(LocalWarriorProc::Juggernaut);d.warriorProcAmount=uint16_t(amount(2));d.warriorProcChance=100;break;
        default: return false;
    }
    if(!d.warriorProcChance||!d.warriorProcChild)return false;
    d.spellFamily=4;d.passive=true;d.buffSelfOnly=true;d.unsupportedReason.clear();return true;
}
// The child half: the triggered spell's local definition, built from its own
// pinned record. Returns false (and the talent stays blocked) on any mismatch.
template<class Tables>
inline bool decodeClientWarriorProcChild(const Tables& t,const LocalSpellDefinition& talent,LocalSpellDefinition& child) {
    const auto id=talent.warriorProcChild;
    const auto kind=LocalWarriorProc(talent.warriorProc);
    if(kind==LocalWarriorProc::DamageShield)return true; // 59653 is the damage's name only; no aura or record to keep
    const auto row=Tables::lookup(t.spellIndex,id);
    if(row<0||!localWarriorProcChildMatches(t,id))return false;
    const auto u=[&](uint32_t col){return t.spells->getUInt32(row,col);};
    const auto duration=localWarriorDurationMs(t,uint32_t(row));
    if(duration<0)return false;
    child={};child.id=id;child.clientSpell=true;child.triggeredOnly=true;child.allowableClasses=1;
    child.buffSelfOnly=true;child.maxAuraStacks=1;child.range=0;child.schoolMask=1;child.baseLevel=1;
    child.name=t.spells->getString(row,136);child.iconId=u(133);
    child.spellFamily=u(spell335::SpellFamily);
    for(uint32_t k=0;k<3;++k)child.spellFamilyFlags[k]=u(spell335::SpellFamilyFlags+k);
    child.warriorProcParentTalent=uint16_t(talent.talentId);child.durationMs=uint32_t(duration);
    const auto amount=int32_t(u(80))+1;
    switch(kind) {
        case LocalWarriorProc::DeepWounds: // 12721: physical bleed, a tick a second for 6 s
            if(duration!=6000||u(98)!=1000)return false;
            child.periodicIntervalMs=1000;child.periodicIgnoresArmor=true;child.buffSelfOnly=false;return true;
        case LocalWarriorProc::ImprovedHamstring: // 23694: root (aura 26)
            if(duration!=5000||u(95)!=26)return false;
            child.buffSelfOnly=false;child.mechanic=uint8_t(u(3));return true;
        case LocalWarriorProc::Enrage: case LocalWarriorProc::WreckingCrew: // aura 79 on physical damage
            if(duration!=12000||u(95)!=79||u(110)!=1||amount<1||amount>100)return false;
            child.physicalDamageDonePct=uint8_t(amount);child.warriorProcAura=uint8_t(LocalWarriorProcAura::Damage);return true;
        case LocalWarriorProc::SuddenDeath:
            if(duration!=10000)return false;child.warriorProcAura=uint8_t(LocalWarriorProcAura::SuddenDeath);return true;
        case LocalWarriorProc::Bloodsurge:
            if(duration!=5000)return false;child.warriorProcAura=uint8_t(LocalWarriorProcAura::Bloodsurge);return true;
        case LocalWarriorProc::SwordAndBoard:
            if(duration!=5000)return false;child.warriorProcAura=uint8_t(LocalWarriorProcAura::SwordAndBoard);return true;
        case LocalWarriorProc::Juggernaut:
            if(duration!=10000||amount!=25)return false;child.warriorProcAura=uint8_t(LocalWarriorProcAura::Juggernaut);return true;
        case LocalWarriorProc::Trauma: // npc debuff: bleeds on the target hurt this much more
            if(duration!=60000||u(95)!=255||amount<1||amount>100)return false;
            child.buffSelfOnly=false;child.warriorProcAmount=uint16_t(amount);return true;
        case LocalWarriorProc::TasteForBlood: // Overpower opened for the child's duration
            if(duration!=9000)return false;return true;
        case LocalWarriorProc::SwordSpecialization: case LocalWarriorProc::ShieldSpecialization:
        case LocalWarriorProc::ImprovedBerserkerRage:
            child.durationMs=0;
            // 16459 extra attack; 23602 / 23690 / 23691 energize rage in tenths.
            if(kind!=LocalWarriorProc::SwordSpecialization&&(u(71)!=30||amount<=0||amount>1000))return false;
            child.energizeRage=uint8_t(std::max(0,amount)/10);return true;
        default: return false;
    }
}
// Berserker Rage: a 10 s self buff with mechanic immunity to fear (5),
// knockout (14) and sap (30); casting it also removes those (executeCastSpell)
// and it doubles the rage gained from taking damage. Its client record carries
// proc flags for that rage, which is why the generic importer refused it.
inline constexpr uint32_t kLocalBerserkerRage=18499;
template<class Tables>
inline bool decodeClientBerserkerRage(const Tables& t,LocalSpellDefinition& d) {
    const auto row=Tables::lookup(t.spellIndex,kLocalBerserkerRage);
    if(row<0||d.id!=kLocalBerserkerRage||!localWarriorProcChildMatches(t,kLocalBerserkerRage)||
       localWarriorDurationMs(t,uint32_t(row))!=10000)return false;
    d.proc={};d.secondaryProc={};d.classBuff=true;d.buffSelfOnly=true;d.passive=false;d.durationMs=10000;
    d.classBuffMechanicImmunity=(1u<<5)|(1u<<14)|(1u<<30);d.mana=0;d.manaPercent=0;d.resourceType=1;
    d.cooldownMs=30000;d.globalCooldownMs=1500;d.schoolMask=1;d.maxAuraStacks=1;d.range=0;
    d.spellFamily=4;d.spellFamilyFlags={0x10000000u,0,0};d.unsupportedReason.clear();return true;
}
// The allocated rank of a reviewed proc talent the character can use now.
inline const LocalSpellDefinition* localWarriorProcTalent(const LocalRealmPlayer& p,const LocalWorldContent& c,LocalWarriorProc kind) {
    if(p.classId!=1||p.dead||!p.health||!validLocalTalents(p))return nullptr;
    for(auto [id,rank]:p.talents)if(const auto* d=localTalentSpell(c,id,rank);
        d&&d->passive&&d->unsupportedReason.empty()&&d->warriorProc==uint8_t(kind)&&(d->allowableClasses&1u)&&
        localTalentPrerequisitesReady(p,c,*d))return d;
    return nullptr;
}
inline LocalStatAura* localWarriorProcAura(LocalRealmPlayer& p,const LocalWorldContent& c,LocalWarriorProcAura kind) {
    for(auto& a:p.statAuras)if(a.remainingMs&&a.mapId==p.mapId&&a.instanceId==p.instanceId)
        if(const auto* d=c.spell(a.spellId);d&&d->warriorProcAura==uint8_t(kind))return &a;
    return nullptr;
}
}

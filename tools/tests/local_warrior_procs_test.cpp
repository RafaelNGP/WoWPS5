#include "local_group_rewards_fixture.hpp"
#include "game/local_melee.hpp"
#include "game/local_spell_import.hpp"
#include "game/local_talents.hpp"
#include "game/local_warrior_procs.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <source_location>
using namespace wowee;
using namespace wowee::game;
// Reviewed Warrior proc talents: every rank and triggered spell pinned to its
// 12340 record, then each proc driven through the real combat loop.
static void check(bool ok,std::source_location location=std::source_location::current()){
    if(!ok){std::cerr<<"check failed at line "<<location.line()<<'\n';std::abort();}
}
namespace {
std::vector<LocalSpellDefinition> gSpells;
struct World {
    std::shared_ptr<LocalWorldContent> c=rewardContent();
    LocalGameplay game;LocalRealmPlayer p=rewardPlayer(1);LocalRealmNpc n=rewardNpc();
    std::string message;uint64_t seen=0;
    World(std::vector<std::pair<uint32_t,uint8_t>> talents,uint32_t form=2457,bool shield=false,float distance=1) {
        c->spells=gSpells;
        c->npcs[0].health=2000000;c->npcs[0].armor=0;c->npcs[0].damage=40;
        for(uint32_t id:{25u,1168u}){const auto* m=localMeleeItem(id);check(m!=nullptr);
            LocalItemDefinition item;item.id=id;item.inventoryType=m->inventoryType;item.armor=id==1168?500:0;c->items.push_back(item);}
        std::sort(c->items.begin(),c->items.end(),[](const auto& a,const auto& b){return a.id<b.id;});
        game.useContent(c);game.seedGameObjectRandom(7);
        p.classId=1;p.race=1;p.level=80;p.resourceType=LocalResourceType::Rage;game.initializePlayer(p,false);
        std::sort(talents.begin(),talents.end());p.talents=talents;check(validLocalTalents(p));
        p.knownSpells.insert(p.knownSpells.end(),{1464,5308,772,6572,23922,1715,100,12294,18499,47450,1680,2457,71,2458});
        p.inventory.push_back({25,1});p.equipment[15]=25;
        if(shield){p.inventory.push_back({1168,1});p.equipment[16]=1168;}
        p.formSpellId=form;p.health=p.maxHealth=100000;p.mana=p.maxMana=100;
        n.health=n.maxHealth=2000000;n.level=80;n.x=n.homeX=distance;n.attackTimer=1000;
        game.setRemoteNpcs({n});game.tick(.01f,{&p});
    }
    LocalRealmNpc& npc(){return const_cast<LocalRealmNpc&>(game.npcs().front());}
    std::vector<LocalCombatEvent> events(){
        std::vector<LocalCombatEvent> out;for(const auto& e:game.combatEvents())if(e.sequence>seen)out.push_back(e);
        if(!out.empty())seen=out.back().sequence;return out;
    }
    bool cast(uint32_t id,uint64_t target=10){
        p.globalCooldownMs=0;p.cooldowns.clear();p.categoryCooldowns.clear();
        const bool ok=game.execute(p,{LocalAction::CastSpell,target,id},{&p},message);
        if(!ok&&std::getenv("PROC_VERBOSE"))std::cerr<<"cast "<<id<<": "<<message<<'\n';return ok;
    }
    void swing(){p.attackTarget=n.guid;p.attackTimer=0;p.health=p.maxHealth;game.tick(.01f,{&p});}
    void struck(){auto& m=npc();m.targetGuid=p.guid;m.threat[0]={p.guid,1000};m.attackTimer=0;p.health=p.maxHealth;game.tick(.01f,{&p});}
    void advance(unsigned ms){while(ms){const auto step=std::min(ms,100u);p.health=p.maxHealth;game.tick(step/1000.f,{&p});ms-=step;}}
    LocalStatAura* aura(uint32_t id){for(auto& a:p.statAuras)if(a.spellId==id&&a.remainingMs)return &a;return nullptr;}
    bool until(const std::function<void()>& act,const std::function<bool()>& done,int tries=600){
        for(int i=0;i<tries;++i){act();if(done())return true;}return false;
    }
};
}
int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"Usage: local_warrior_procs_test DBC_DIRECTORY\n";return 2;}
    std::map<std::string,pipeline::DBCFile> tables;std::map<std::string,std::vector<uint8_t>> bytes;
    for(const auto* name:{"Spell","SpellRange","SpellCastTimes","SpellDuration","SpellIcon","SkillLineAbility","SkillLine",
                          "Talent","SpellRuneCost","SpellRadius","TalentTab"}){
        std::ifstream in(std::filesystem::path(argv[1])/(std::string(name)+".dbc"),std::ios::binary);
        bytes[name]={std::istreambuf_iterator<char>(in),{}};check(tables[name].load(bytes[name]));
    }
    const auto get=[&](const char* name){return &tables.at(name);};
    const auto import=[&]{
        auto imported=importClientStarterSpells(get("Spell"),get("SpellRange"),get("SpellCastTimes"),get("SpellDuration"),get("SpellIcon"),
            get("SkillLineAbility"),get("SkillLine"),get("Talent"),get("SpellRuneCost"),get("SpellRadius"));
        detail::importClientTalents(imported,get("Talent"),get("TalentTab"),get("Spell"),get("SpellRange"),get("SpellCastTimes"),
            get("SpellDuration"),get("SpellIcon"),get("SpellRuneCost"),get("SpellRadius"));
        return imported.spells;
    };
    gSpells=import();
    const auto find=[&](uint32_t id)->const LocalSpellDefinition*{for(const auto& d:gSpells)if(d.id==id)return &d;return nullptr;};

    // 1. Admission: 14 proc talents (45 ranks), their children, Berserker Rage.
    unsigned ranks=0;
    for(const auto& r:kLocalWarriorTalentRecords){
        const auto* d=find(r.spellId);check(d&&d->unsupportedReason.empty());
        if(d->warriorProc){++ranks;check(d->passive&&d->proc.effect==LocalProcEffect::None&&d->warriorProcChance&&d->warriorProcChild);}
    }
    check(ranks==45);
    for(uint32_t id:{12880u,14204u,57518u,57522u}){const auto* d=find(id);check(d&&d->warriorProcAura==1&&d->physicalDamageDonePct&&d->durationMs==12000&&validLocalProc(*d));}
    check(find(14204)->physicalDamageDonePct==10&&find(57518)->physicalDamageDonePct==2);
    for(uint32_t id:{52437u,46916u,50227u,65156u}){const auto* d=find(id);check(d&&d->warriorProcAura>1&&validLocalProc(*d)&&localHasTimedAura(*d));}
    {const auto* d=find(kLocalDeepWoundsPeriodic);check(d&&d->durationMs==6000&&d->periodicIntervalMs==1000&&d->periodicIgnoresArmor);}
    {const auto* d=find(kLocalBerserkerRage);check(d&&d->unsupportedReason.empty()&&d->classBuff&&d->durationMs==10000&&
        d->classBuffMechanicImmunity==((1u<<5)|(1u<<14)|(1u<<30)));}
    {const auto* t=find(29724);check(t&&t->warriorProcChance==9&&t->warriorProcAmount==10);} // Sudden Death 3: 9%, keeps 10 rage
    {const auto* t=find(12727);check(t&&t->passiveBlockPct==5&&t->warriorProcChance==100);}   // Shield Specialization 5
    // A child record that differs keeps its talent blocked (Enrage rank 1 -> 12880).
    {
        auto edited=bytes["Spell"];
        const auto row=[&]{detail::ClientSpellTables t;t.spells=get("Spell");detail::ClientSpellTables::buildIndex(t.spells,t.spellIndex);
            return detail::ClientSpellTables::lookup(t.spellIndex,12880);}();
        edited[20+size_t(row)*tables["Spell"].getRecordSize()+80*4]^=1;check(tables["Spell"].load(edited));
        const auto mutated=import();
        for(const auto& d:mutated)if(d.id==12317)check(!d.unsupportedReason.empty());
        check(tables["Spell"].load(bytes["Spell"]));
    }

    // 2. Deep Wounds + Trauma + Wrecking Crew on a melee critical.
    {
        World w({{121,3},{1859,2},{2231,5}});
        bool bled=false,traumaMarked=false;uint32_t tick=0;
        check(w.until([&]{w.swing();w.advance(1000);for(const auto& e:w.events())if(e.spell==kLocalDeepWoundsPeriodic&&e.kind==LocalCombatEventKind::PeriodicDamage){bled=true;tick=e.attempted;}},
                      [&]{return bled;}));
        for(const auto& b:w.npc().npcBuffs)if(b.spellId==46857&&b.remainingMs)traumaMarked=true;
        check(traumaMarked&&tick>0);
        check(w.aura(57522)!=nullptr); // Wrecking Crew 5: +10% physical damage for 12 s
        LocalRealmPlayer bare=w.p;bare.talents.clear();
        check(localPhysicalDamageAfterTalents(w.p,*w.c,1000)>=localPhysicalDamageAfterTalents(bare,*w.c,1000)*109/100);
    }
    // 3. Enrage from being struck; it replaces Wrecking Crew, never stacks with it.
    {
        World w({{155,5},{2231,5}});
        check(w.until([&]{w.struck();},[&]{return w.aura(14204)!=nullptr;}));
        check(w.until([&]{w.swing();},[&]{return w.aura(57522)!=nullptr;}));
        check(!w.aura(14204));
    }
    // 4. Sudden Death: Execute at full health, consumed, at least 10 rage kept.
    {
        World w({{1662,3}});
        check(w.until([&]{w.swing();},[&]{return w.aura(52437)!=nullptr;},3000));
        w.p.mana=100;check(w.cast(5308));check(!w.aura(52437));check(w.p.mana>=10);
        w.p.mana=100;check(!w.cast(5308)); // no proc: the health gate is back
    }
    // 5. Bloodsurge: a Heroic Strike hit makes the next Slam instant.
    {
        World w({{1866,3}});
        check(w.until([&]{w.p.mana=100;w.cast(47450);w.swing();},[&]{return w.aura(46916)!=nullptr;}));
        w.p.mana=100;check(w.cast(1464));check(!w.p.castingSpellId&&!w.aura(46916));
    }
    // 6. Taste for Blood 3: a Rend tick opens Overpower.
    {
        World w({{2232,3}});w.p.overpowerWindowMs=0;
        check(w.cast(772));
        check(w.until([&]{w.advance(500);},[&]{return w.p.overpowerWindowMs>0;},60));
        check(w.p.warriorProcCooldownMs[kLocalTasteForBloodCooldown]>0);
    }
    // 7. Sword and Board: Revenge resets Shield Slam and makes it free.
    {
        World w({{1871,3}},71,true);
        check(w.until([&]{w.p.mana=100;w.p.revengeWindowMs=5000;w.cast(6572);},[&]{return w.aura(50227)!=nullptr;}));
        w.p.mana=0;check(w.cast(23922));check(!w.aura(50227));
    }
    // 8. Shield Specialization 5: rage on block/dodge/parry; Damage Shield 2 hits back.
    {
        World w({{1601,5},{2246,2}},71,true);
        LocalRealmPlayer noTalent=w.p;noTalent.talents.clear();
        check(std::abs(localMeleeStats(w.p,*w.c).block-localMeleeStats(noTalent,*w.c).block-5.f)<.01f);
        bool rage=false,shielded=false;
        check(w.until([&]{w.p.mana=0;w.struck();for(const auto& e:w.events()){
                if(e.kind==LocalCombatEventKind::NpcMelee&&(e.outcome==LocalMeleeOutcome::Dodge||e.outcome==LocalMeleeOutcome::Parry||e.outcome==LocalMeleeOutcome::Block)&&w.p.mana>=5)rage=true;
                if(e.spell==kLocalDamageShieldSpell&&e.source==w.p.guid&&e.attempted)shielded=true;}},
            [&]{return rage&&shielded;}));
    }
    // 9. Juggernaut: Charge in combat, +5 s on its cooldown, then a critical-chance Mortal Strike.
    {
        World w({{2283,1}},2457,false,15);
        w.npc().targetGuid=w.p.guid;w.npc().threat[0]={w.p.guid,1000};w.p.attackTarget=w.n.guid;w.game.tick(.01f,{&w.p});
        w.p.mana=0;check(w.cast(100));check(w.aura(65156)!=nullptr);
        bool longer=false;for(const auto& cd:w.p.categoryCooldowns)if(cd.remainingMs>15000)longer=true;check(longer);
        w.p.mana=100;check(w.cast(12294));check(!w.aura(65156));
    }
    // 10. Improved Berserker Rage 2: +20 rage on Berserker Rage, which doubles struck rage.
    {
        World w({{1541,2}},2458);
        w.p.mana=0;check(w.cast(kLocalBerserkerRage));check(w.p.mana==20&&w.aura(kLocalBerserkerRage));
    }
    // 11. Sword Specialization 5: an extra swing without moving the swing timer.
    {
        World w({{123,5}});
        check(w.until([&]{w.swing();},[&]{return w.p.extraAttacks>0;}));
        w.events();w.p.attackTimer=1.f;w.game.tick(.01f,{&w.p});
        unsigned swings=0;for(const auto& e:w.events())if(e.kind==LocalCombatEventKind::PlayerMelee&&e.source==w.p.guid)++swings;
        check(swings==1&&!w.p.extraAttacks&&w.p.attackTimer>.9f);
    }
    // 12. Improved Hamstring 3: Hamstring can root the creature.
    {
        World w({{129,3}});
        check(w.until([&]{w.p.mana=100;w.cast(1715);},[&]{
            for(const auto& ctl:w.npc().controls)if(ctl.spellId==23694&&ctl.kind==uint8_t(LocalNpcControlKind::Root))return true;return false;}));
    }
    std::cout<<"PASS: "<<ranks<<" Warrior proc talent ranks and their children pinned; Deep Wounds, Trauma, Wrecking Crew, Enrage, "
               "Sudden Death, Bloodsurge, Taste for Blood, Sword and Board, Shield Specialization, Damage Shield, Juggernaut, "
               "Improved Berserker Rage with Berserker Rage, Sword Specialization and Improved Hamstring through real combat\n";
}

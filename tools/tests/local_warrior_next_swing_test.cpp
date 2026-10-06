#include "local_group_rewards_fixture.hpp"
#include "game/local_melee.hpp"
#include "game/local_spell_import.hpp"
#include "game/local_talents.hpp"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <source_location>
using namespace wowee;
using namespace wowee::game;
// Heroic Strike / Cleave (SPELL_ATTR0_ON_NEXT_SWING): queued, cancelled by a
// second press, struck by the next main-hand swing instead of a white hit,
// off the global cooldown, rage checked at queue time and spent at the swing.
static void check(bool ok,std::source_location location=std::source_location::current()){
    if(!ok){std::cerr<<"check failed at line "<<location.line()<<'\n';std::abort();}
}
int main(int argc,char** argv){
    if(argc!=2){std::cerr<<"Usage: local_warrior_next_swing_test DBC_DIRECTORY\n";return 2;}
    std::map<std::string,pipeline::DBCFile> tables;
    for(const auto* name:{"Spell","SpellRange","SpellCastTimes","SpellDuration","SpellIcon","SkillLineAbility","SkillLine",
                          "Talent","SpellRuneCost","SpellRadius","TalentTab"}){
        std::ifstream in(std::filesystem::path(argv[1])/(std::string(name)+".dbc"),std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};check(tables[name].load(bytes));
    }
    const auto get=[&](const char* name){return &tables.at(name);};
    auto imported=importClientStarterSpells(get("Spell"),get("SpellRange"),get("SpellCastTimes"),get("SpellDuration"),get("SpellIcon"),
        get("SkillLineAbility"),get("SkillLine"),get("Talent"),get("SpellRuneCost"),get("SpellRadius"));
    detail::importClientTalents(imported,get("Talent"),get("TalentTab"),get("Spell"),get("SpellRange"),get("SpellCastTimes"),
        get("SpellDuration"),get("SpellIcon"),get("SpellRuneCost"),get("SpellRadius"));
    constexpr uint32_t heroicStrike=47450,cleave=47520,battleStance=2457;
    {
        bool hsNext=false,cleaveNext=false,mortalNext=false;
        for(const auto& d:imported.spells){
            if(d.id==heroicStrike)hsNext=d.nextSwing&&d.unsupportedReason.empty();
            if(d.id==cleave)cleaveNext=d.nextSwing&&d.unsupportedReason.empty();
            if(d.id==47486)mortalNext=d.nextSwing;
        }
        check(hsNext&&cleaveNext&&!mortalNext);
    }
    auto c=rewardContent();c->spells=imported.spells;
    std::sort(c->spells.begin(),c->spells.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    c->npcs[0].health=1000000;c->npcs[0].armor=0;c->npcs[0].damage=1;
    const auto* sword=localMeleeItem(25);check(sword!=nullptr);
    LocalItemDefinition weapon;weapon.id=25;weapon.inventoryType=sword->inventoryType;c->items.push_back(weapon);
    std::sort(c->items.begin(),c->items.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    LocalGameplay game;game.useContent(c);
    LocalRealmPlayer p=rewardPlayer(1);p.classId=1;p.race=1;p.level=80;p.resourceType=LocalResourceType::Rage;
    game.initializePlayer(p,false);
    p.knownSpells.insert(p.knownSpells.end(),{heroicStrike,cleave,battleStance});
    p.inventory.push_back({25,1});p.equipment[15]=25;p.formSpellId=battleStance;
    p.health=p.maxHealth;p.mana=p.maxMana=100;
    LocalRealmNpc n=rewardNpc();n.health=n.maxHealth=1000000;n.level=1;n.x=n.homeX=1;n.attackTimer=1000;
    game.setRemoteNpcs({n});
    std::string message;
    const auto npcHealth=[&]{return game.npcs().front().health;};
    const auto cast=[&](uint32_t id){const bool ok=game.execute(p,{LocalAction::CastSpell,n.guid,id},{&p},message);
        if(!ok)std::cerr<<"cast "<<id<<": "<<message<<'\n';return ok;};
    const auto swing=[&]{p.attackTimer=0;game.tick(.01f,{&p});};
    const auto* hs=c->spell(heroicStrike);check(hs!=nullptr);
    const auto cost=localSpellResourceCost(p,*c,*hs);check(cost>0&&cost<100);

    // 1. Queue: nothing happens yet, no rage spent, the swing target is set.
    p.attackTimer=10;const auto before=npcHealth();
    check(cast(heroicStrike));check(message.find("queued")!=std::string::npos);
    check(p.nextSwingSpellId==heroicStrike&&p.nextSwingTarget==n.guid&&p.attackTarget==n.guid);
    check(npcHealth()==before&&p.mana==100);
    // 2. A second press cancels it.
    check(cast(heroicStrike));check(message.find("cancelled")!=std::string::npos&&!p.nextSwingSpellId);

    // 3. The next main-hand swing is the strike; the global cooldown neither
    //    blocks it nor is cleared by it.
    check(cast(heroicStrike));p.globalCooldownMs=1200;
    uint64_t seen=0;for(const auto& e:game.combatEvents())seen=std::max(seen,e.sequence);
    swing();
    check(!p.nextSwingSpellId&&npcHealth()<before);
    check(p.globalCooldownMs>0);
    bool spellHit=false,whiteHit=false;
    for(const auto& e:game.combatEvents())if(e.sequence>seen&&e.source==p.guid){
        if(e.spell==heroicStrike)spellHit=true;
        if(e.kind==LocalCombatEventKind::PlayerMelee&&!e.spell)whiteHit=true;
    }
    check(spellHit&&!whiteHit);
    check(p.mana<=100-cost+5); // the strike's own rage was paid (a crit may refund nothing here)

    // 4. Without the rage by the swing, the queue is dropped for a white hit.
    p.mana=100;p.attackTimer=10;check(cast(heroicStrike));p.mana=0;
    for(const auto& e:game.combatEvents())seen=std::max(seen,e.sequence);
    swing();check(!p.nextSwingSpellId);
    spellHit=false;
    for(const auto& e:game.combatEvents())if(e.sequence>seen&&e.spell==heroicStrike)spellHit=true;
    check(!spellHit);

    // 5. Queueing needs the rage now, a living enemy, and stopping the attack drops it.
    p.mana=0;check(!cast(heroicStrike));check(!p.nextSwingSpellId);
    p.mana=100;p.attackTimer=10;check(cast(cleave));check(p.nextSwingSpellId==cleave);
    p.attackTarget=0;game.tick(.01f,{&p});check(!p.nextSwingSpellId);

    std::cout<<"PASS: Heroic Strike / Cleave next-swing queue: queued without cost, cancelled by a second press, struck by the "
               "main-hand swing instead of a white hit while on global cooldown, dropped without rage or target\n";
}

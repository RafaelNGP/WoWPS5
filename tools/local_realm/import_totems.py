#!/usr/bin/env python3
"""Compile the shaman totem companion: what each totem summon puts down.

A totem summon is a Shaman (SpellFamily 11) SPELL_EFFECT_SUMMON (28) whose
SummonProperties row has Title 4 (SUMMON_TYPE_TOTEM) and a slot 1-4 (fire,
earth, water, air). Its creature (MiscValue) casts creature_template_spell
slot 0 (AzerothCore at the pinned commit; fetched from the repository because
the world source archive does not carry that table). The player's own
Spell.dbc (3.3.5a build 12340) says what that spell does, and only these
shapes are compiled:

  attack       a direct hit on one enemy (Searing Bolt): every cast, in range;
  pulseDamage  a periodic trigger of a caster-centred area hit (Magma Totem);
  pulseHeal    a periodic trigger of the party heal (Healing Stream);
  pulseSnare   a periodic trigger of an area slow on enemies (Earthbind);
  aura         a party area aura of primary stats, armor or mana regeneration
               (Strength of Earth, Stoneskin, Mana Spring).

Other totems (Grounding, Tremor, Earthbind, Windfury, Wrath of Air...) are
left out, so their summons stay unavailable.

    python3 -B tools/local_realm/import_totems.py <DBFilesClient dir> \\
        --output assets/local_realm/totems.json
"""
from __future__ import annotations
import argparse, json, re, struct, sys, tarfile, tempfile, urllib.request
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, PINNED_COMMIT, REPOSITORY
from import_consumables import dbc

HERE = Path(__file__).resolve().parent
EFFECT, DIE, BASE, TARGET, AURA, AMPLITUDE, MISC, MISC_B, TRIGGER = 71, 74, 80, 86, 95, 98, 110, 113, 116
RADIUS, NAME, FAMILY, LEVEL, CAST, DURATION, RANGE, SCHOOL = 92, 136, 208, 39, 28, 40, 46, 225
ELEMENTS = {1: 'fire', 2: 'earth', 3: 'water', 4: 'air'}


def floats(path):
    b = path.read_bytes()
    n, fields, size, _ = struct.unpack_from('<4I', b, 4)
    return {struct.unpack_from('<I', b, 20 + i * size)[0]: struct.unpack_from(f'<{fields}f', b, 20 + i * size) for i in range(n)}


def template_spells(path):
    if path is None:
        url = f'{REPOSITORY.replace("github.com", "raw.githubusercontent.com")}/{PINNED_COMMIT}/data/sql/base/db_world/creature_template_spell.sql'
        text = urllib.request.urlopen(url, timeout=60).read().decode()
    else:
        text = path.read_text()
    out = {}
    for entry, index, spell in re.findall(r'\((\d+),(\d+),(\d+),\d+\)', text):
        if int(index) == 0: out[int(entry)] = int(spell)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dbc_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sources', type=Path, default=HERE / 'world_source_sql.tar.gz')
    ap.add_argument('--template-spells', type=Path, help='creature_template_spell.sql (default: fetched at the pinned commit)')
    args = ap.parse_args()
    spells, string = dbc(args.dbc_dir / 'Spell.dbc')
    props = {k: v[0] for k, v in dbc(args.dbc_dir / 'SummonProperties.dbc')[0].items()}
    durations = {k: v[1][1] for k, v in dbc(args.dbc_dir / 'SpellDuration.dbc')[0].items()}
    casts = {k: v[1][1] for k, v in dbc(args.dbc_dir / 'SpellCastTimes.dbc')[0].items()}
    radii = {k: v[1] for k, v in floats(args.dbc_dir / 'SpellRadius.dbc').items()}
    ranges = {k: v[3] for k, v in floats(args.dbc_dir / 'SpellRange.dbc').items()}
    casts_by = template_spells(args.template_spells)
    # Player ranks only: a shaman (ClassMask 64) row of SkillLineAbility.dbc.
    learnable = {u[2] for u, _ in dbc(args.dbc_dir / 'SkillLineAbility.dbc')[0].values() if u[4] & 64}
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.sources) as tar:
        tar.extract('creature_template_model.sql', tmp, filter='data')
        models = {}
        for r in sql_rows(Path(tmp) / 'creature_template_model.sql', 'creature_template_model'):
            if int(r['idx']) == 0: models[int(r['creatureid'])] = int(r['creaturedisplayid'])

    def amount(s, e): return s[BASE + e] + 1, s[BASE + e] + max(1, s[DIE + e])

    totems, skipped = [], {}
    for spell_id, (u, s) in sorted(spells.items()):
        if u[FAMILY] != 11 or u[EFFECT] != 28 or spell_id not in learnable: continue
        prop = props.get(u[MISC_B])
        if not prop or prop[3] != 4 or prop[4] not in ELEMENTS: continue
        entry = u[MISC]
        cast = casts_by.get(entry)
        name = string(u[NAME])
        if not cast or cast not in spells or entry not in models:
            skipped[name] = 'no creature spell or model'; continue
        cu, cs = spells[cast]
        row = {'spellId': spell_id, 'name': name, 'entry': entry, 'displayId': models[entry], 'element': prop[4] - 1,
               'level': u[LEVEL], 'durationMs': max(0, durations.get(u[DURATION], 0)), 'castSpell': cast}
        if cu[EFFECT] == 2 and cu[TARGET] == 6:
            low, high = amount(cs, 0)
            row.update(kind='attack', low=low, high=high, periodMs=max(1000, casts.get(cu[CAST], 0) or 2000),
                       range=round(ranges.get(cu[RANGE], 20.0), 2), school=cu[SCHOOL])
        elif cu[EFFECT] == 6 and cu[AURA] == 23 and cu[TRIGGER] in spells and cu[AMPLITUDE]:
            tu, ts = spells[cu[TRIGGER]]
            radius = round(radii.get(tu[RADIUS], 0.0), 2)
            if tu[EFFECT] == 2 and tu[TARGET] in (22, 15) and radius:
                low, high = amount(ts, 0)
                row.update(kind='pulseDamage', low=low, high=high, periodMs=cu[AMPLITUDE], radius=radius, school=tu[SCHOOL])
            elif tu[EFFECT] == 3 and tu[TARGET] == 20 and radius and string(tu[NAME]) == 'Healing Stream Totem':  # spell_sha_healing_stream_totem
                low, high = amount(ts, 0)
                row.update(kind='pulseHeal', low=low, high=high, periodMs=cu[AMPLITUDE], radius=radius)
            elif tu[EFFECT] == 3 and tu[TARGET] == 20 and radius and string(tu[NAME]) == 'Mana Tide Totem':  # spell_sha_mana_tide_totem
                # "Regenerate 6% of Total Mana Every 3 secs": the totem aura's own amount.
                row.update(kind='pulseMana', pct=cs[BASE] + 1, periodMs=cu[AMPLITUDE], radius=radius)
            elif tu[EFFECT] == 6 and tu[AURA] == 33 and tu[TARGET] in (22, 15) and radius and -100 < ts[BASE] + 1 < 0:
                row.update(kind='pulseSnare', snareSpell=cu[TRIGGER], snarePct=-(ts[BASE] + 1),
                           snareMs=max(0, durations.get(tu[DURATION], 0)), periodMs=cu[AMPLITUDE], radius=radius)
            else:
                skipped[name] = f'periodic trigger {cu[TRIGGER]}'; continue
        elif cu[EFFECT] in (35, 65):
            stats, armor, mp5, haste, cast_speed, ok = [0] * 5, 0, 0, 0, 0, True
            for e in range(3):
                if not cu[EFFECT + e]: continue
                aura, value = cu[AURA + e], cs[BASE + e] + 1
                if aura == 29 and -1 <= cs[MISC + e] <= 4:
                    for k in range(5):
                        if cs[MISC + e] in (-1, k): stats[k] += value
                elif aura == 22 and cs[MISC + e] & 1: armor += value
                elif aura == 85 and cs[MISC + e] == 0: mp5 += value
                elif aura == 138 and 0 < value <= 100: haste += value  # Windfury Totem: MOD_MELEE_HASTE
                elif aura == 65 and 0 < value <= 100: cast_speed += value  # Wrath of Air: MOD_CASTING_SPEED_NOT_STACK
                elif cs[BASE + e] == -1: pass  # a zero-amount marker (Strength of Earth's aura 52)
                else: ok = False
            radius = round(max(radii.get(cu[RADIUS + e], 0.0) for e in range(3)), 2)
            if not ok or not radius or not (any(stats) or armor or mp5 or haste or cast_speed):
                skipped[name] = f'area aura {cast}'; continue
            row.update(kind='aura', stats=stats, armor=armor, mp5=mp5, radius=radius, periodMs=1000)
            if haste: row['meleeHastePct'] = haste
            if cast_speed: row['castSpeedPct'] = cast_speed
        else:
            skipped[name] = f'creature spell {cast}'; continue
        totems.append(row)
    doc = {'schemaVersion': 1, 'sourceCommit': PINNED_COMMIT, 'clientBuild': 12340, 'totems': totems}
    args.output.write_text(json.dumps(doc, separators=(',', ':'), sort_keys=True) + '\n')
    names = sorted({t['name'] for t in totems})
    print(json.dumps({'ranks': len(totems), 'totems': names, 'skipped': sorted(skipped)}))


if __name__ == '__main__':
    main()

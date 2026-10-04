#!/usr/bin/env python3
"""Compile the hunter pet companion: tameable beasts and the pet spells.

creature_template (AzerothCore, the pinned world_source_sql.tar.gz) marks a
tameable creature with CREATURE_TYPE_FLAG_TAMEABLE (type_flags & 1) on a beast
(type 1) with a pet family; CREATURE_TYPE_FLAG_EXOTIC_PET (0x10000) needs the
Beast Mastery talent and is left out. Only creatures the world catalog spawns
are listed. The player's own Spell.dbc/SpellIcon.dbc (3.3.5a build 12340)
give the pet spells' names, icons, cast times and cooldowns.

    python3 -B tools/local_realm/import_hunter_pets.py <DBFilesClient dir> \
        --output assets/local_realm/hunter_pets.json
"""
from __future__ import annotations
import argparse, json, struct, sys, tarfile, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, PINNED_COMMIT
from import_consumables import dbc

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CATEGORY, RECOVERY, CAST_TIME_INDEX, ICON, NAME = 1, 29, 28, 133, 136
TAMEABLE, EXOTIC = 0x1, 0x10000
# Tame Beast, Call Pet, Dismiss Pet, Revive Pet: what a level 10 hunter
# learns to own a pet. All of them are learned at level 10.
PET_SPELLS = {1515: 10, 883: 10, 2641: 10, 982: 10}


def spawned_entries():
    data = (ROOT / 'assets/local_realm/catalog/spawns.pack').read_bytes()
    return {struct.unpack_from('<I', data, i + 4)[0] for i in range(0, len(data), 28)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dbc_dir', type=Path)
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--sources', type=Path, default=HERE / 'world_source_sql.tar.gz')
    args = ap.parse_args()
    spells, string = dbc(args.dbc_dir / 'Spell.dbc')
    icons, icon_string = dbc(args.dbc_dir / 'SpellIcon.dbc')
    cast_times = {k: v[1][1] for k, v in dbc(args.dbc_dir / 'SpellCastTimes.dbc')[0].items()}
    durations = {k: v[1][1] for k, v in dbc(args.dbc_dir / 'SpellDuration.dbc')[0].items()}
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.sources) as tar:
        tar.extract('creature_template.sql', tmp, filter='data')
        templates = list(sql_rows(Path(tmp) / 'creature_template.sql', 'creature_template'))
    spawned = spawned_entries()
    beasts = sorted(({'entry': t['entry'], 'family': t['family']} for t in templates
                     if t['type'] == 1 and t['type_flags'] & TAMEABLE and not t['type_flags'] & EXOTIC
                     and 0 < t['family'] < 256 and t['entry'] in spawned), key=lambda b: b['entry'])
    out_spells = []
    for spell_id, level in sorted(PET_SPELLS.items()):
        u, _ = spells[spell_id]
        icon = icon_string(icons[u[ICON]][0][1]) if u[ICON] in icons else ''
        cast_ms = max(0, cast_times.get(u[CAST_TIME_INDEX], 0))
        if not cast_ms and spell_id == 1515: cast_ms = max(0, durations.get(u[40], 0))  # the Tame Beast channel
        out_spells.append({'id': spell_id, 'name': string(u[NAME])[:64], 'icon': icon[:128], 'level': level,
                           'castMs': cast_ms, 'cooldownMs': u[RECOVERY]})
    # Basic attacks (Bite, Claw, Smack): which one a family learns comes from
    # its pet skill line (CreatureFamily.dbc SkillLine) in SkillLineAbility.dbc;
    # every rank is a 25-focus physical hit of BasePoints+1..+DieSides.
    families, _ = dbc(args.dbc_dir / 'CreatureFamily.dbc')
    abilities, _ = dbc(args.dbc_dir / 'SkillLineAbility.dbc')
    by_line = {}
    for u, _ in abilities.values(): by_line.setdefault(u[1], []).append(u[2])
    basic_names = ('Bite', 'Claw', 'Smack')
    ranks = {}
    for spell_id, (u, s) in spells.items():
        name = string(u[NAME])
        if name in basic_names and u[71] == 2 and u[42] == 25 and u[41] == 2:
            ranks.setdefault(name, []).append({'spellId': spell_id, 'level': u[39],
                                               'low': s[80] + 1, 'high': s[80] + max(1, s[74])})
    for name in ranks: ranks[name].sort(key=lambda r: r['level'])
    family_attack = []
    for fid, (u, _) in sorted(families.items()):
        kinds = {string(spells[sid][0][NAME]) for line in (u[5], u[6]) for sid in by_line.get(line, []) if sid in spells}
        kind = next((k for k in basic_names if k in kinds), None)
        if kind: family_attack.append({'family': fid, 'attack': kind})
    doc = {'schemaVersion': 1, 'sourceCommit': PINNED_COMMIT, 'clientBuild': 12340,
           'spells': out_spells, 'beasts': beasts, 'familyAttacks': family_attack,
           'basicAttacks': [{'name': k, 'ranks': v} for k, v in sorted(ranks.items())]}
    args.output.write_text(json.dumps(doc, separators=(',', ':'), sort_keys=True) + '\n')
    print(json.dumps({'spells': [s['name'] for s in out_spells], 'beasts': len(beasts), 'familyAttacks': len(family_attack)}))


if __name__ == '__main__':
    main()

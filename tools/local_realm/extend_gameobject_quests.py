#!/usr/bin/env python3
"""Add quests with GameObject interactions (starters, enders, objectives, loot)
to the shipped world catalog.

Follows the pattern of extend_start_item_quests.py: preserves all existing entries
in the catalog and only extends them.
"""
from __future__ import annotations
import argparse, collections, json, struct, sys, tarfile, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from import_azerothcore import sql_rows, clean
from import_world_catalog import record_pack, quest_rewards, quest_reputation_rewards, NEUTRAL_FACTIONS, AGGRESSIVE_FACTIONS, MAGIC, encode
import hashlib

HERE = Path(__file__).resolve().parent


def load(path):
    b = path.read_bytes()
    if b[:8] != MAGIC: raise ValueError(f'{path}: not a catalog pack')
    n, _ = struct.unpack_from('<II', b, 8)
    out = {}
    for i in range(n):
        key, off, size = struct.unpack_from('<IQI', b, 16 + i * 16)
        out[key] = json.loads(b[off:off + size])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--catalog', type=Path, default=HERE.parent.parent / 'assets/local_realm/catalog')
    ap.add_argument('--sources', type=Path, default=HERE / 'world_source_sql.tar.gz')
    ap.add_argument('--script-sources', type=Path, default=HERE / 'world_script_source_sql.tar.gz')
    ap.add_argument('--dry-run', action='store_true')
    args = ap.parse_args()

    quests, contacts, npcs = (load(args.catalog / f) for f in ('quests.pack', 'contacts.pack', 'npcs.pack'))
    items = load(args.catalog / 'items.pack')
    spawns = (args.catalog / 'spawns.pack').read_bytes()
    spawned_npcs = {struct.unpack_from('<I', spawns, i + 4)[0] for i in range(0, len(spawns), 28)}

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(args.sources) as tar:
            for t in ('quest_template', 'quest_template_addon', 'creature_queststarter', 'creature_questender', 'creature_template', 'creature_loot_template'):
                tar.extract(f'{t}.sql', tmp, filter='data')
        with tarfile.open(args.script_sources) as tar:
            for t in ('gameobject', 'gameobject_template', 'gameobject_queststarter', 'gameobject_questender', 'gameobject_loot_template'):
                tar.extract(f'{t}.sql', tmp, filter='data')

        q_rows = list(sql_rows(Path(tmp) / 'quest_template.sql', 'quest_template'))
        qa_rows = {r['id']: r for r in sql_rows(Path(tmp) / 'quest_template_addon.sql', 'quest_template_addon')}
        templates = {t['entry']: t for t in sql_rows(Path(tmp) / 'creature_template.sql', 'creature_template')}

        c_starters, c_enders = {}, {}
        for r in sql_rows(Path(tmp) / 'creature_queststarter.sql', 'creature_queststarter'):
            if r['id'] in npcs and r['id'] in spawned_npcs:
                c_starters.setdefault(r['quest'], r['id'])
        for r in sql_rows(Path(tmp) / 'creature_questender.sql', 'creature_questender'):
            if r['id'] in npcs and r['id'] in spawned_npcs:
                c_enders.setdefault(r['quest'], r['id'])

        go_spawns = list(sql_rows(Path(tmp) / 'gameobject.sql', 'gameobject'))
        go_spawned_entries = {s['id'] for s in go_spawns}

        go_starters, go_enders = {}, {}
        for r in sql_rows(Path(tmp) / 'gameobject_queststarter.sql', 'gameobject_queststarter'):
            if r['id'] in go_spawned_entries:
                go_starters.setdefault(r['quest'], r['id'])
        for r in sql_rows(Path(tmp) / 'gameobject_questender.sql', 'gameobject_questender'):
            if r['id'] in go_spawned_entries:
                go_enders.setdefault(r['quest'], r['id'])

        c_drops_by_npc = collections.defaultdict(list)
        for r in sql_rows(Path(tmp) / 'creature_loot_template.sql', 'creature_loot_template'):
            if r['item'] > 0 and not r['reference'] and r['lootmode'] & 1:
                c_drops_by_npc[r['entry']].append(r)

        attackable = {e for e in spawned_npcs if e in npcs and templates.get(e, {}).get('faction') in NEUTRAL_FACTIONS | AGGRESSIVE_FACTIONS and not templates[e]['npcflag']}
        c_available_items = {r['item'] for e in attackable for r in c_drops_by_npc[templates[e]['lootid']]}

        go_loot_by_entry = collections.defaultdict(list)
        for r in sql_rows(Path(tmp) / 'gameobject_loot_template.sql', 'gameobject_loot_template'):
            if r['item'] > 0 and not r['reference'] and r['lootmode'] & 1 and r['entry'] in go_spawned_entries:
                go_loot_by_entry[r['entry']].append(r)
        go_available_items = {r['item'] for rows in go_loot_by_entry.values() for r in rows}

        all_available_items = c_available_items | go_available_items
        starters = {**c_starters, **go_starters}
        enders = {**c_enders, **go_enders}

        added = {}
        reasons = collections.Counter()
        for q in q_rows:
            qid = q['id']
            if qid in quests:
                continue
            a = qa_rows.get(qid, {})
            start = q['startitem']
            why = None
            objectives = []

            has_c_start, has_go_start = qid in c_starters, qid in go_starters
            has_c_end, has_go_end = qid in c_enders, qid in go_enders

            if not (has_c_start or has_go_start) or not (has_c_end or has_go_end):
                why = 'no starter/ender'
            elif not (has_go_start or has_go_end or any(q[f'requirednpcorgo{i}'] < 0 for i in range(1, 5)) or any(q[f'requireditemid{i}'] in go_available_items for i in range(1, 5))):
                why = 'not a gameobject quest'
            elif start and start not in items:
                why = 'start item not in catalog'
            elif q['timeallowed'] or q['requiredplayerkills']:
                why = 'timed/PvP objective'
            elif q['rewardspell'] or q['rewardtitle']:
                why = 'spell/title reward'
            elif a.get('specialflags', 0) & ~4:
                why = 'repeat/script/exploration'
            elif a.get('exclusivegroup', 0) or a.get('prevquestid', 0) < 0:
                why = 'exclusive/group prerequisite'
            elif any(a.get(k, 0) for k in ('sourcespellid', 'requiredskillid', 'rewardmailtemplateid')):
                why = 'skill/spell/mail condition'
            elif any(q.get(k, 0) for k in ('requireditemid5', 'requireditemid6')):
                why = 'unsupported extra objective'
            elif q['rewardmoney'] < 0:
                why = 'money payment'

            if not why:
                for i in range(1, 5):
                    entry, count = q[f'requirednpcorgo{i}'], q[f'requirednpcorgocount{i}']
                    if entry < 0:
                        go_entry = -entry
                        if go_entry not in go_spawned_entries:
                            why = why or 'gameobject objective not spawned'
                        else:
                            objectives.append({'type': 'gameobject', 'entry': go_entry, 'count': count})
                    elif entry and count:
                        if entry not in attackable:
                            why = why or 'unsupported/nonattackable kill target'
                        else:
                            objectives.append({'type': 'kill', 'entry': entry, 'count': count})

                    item, count = q[f'requireditemid{i}'], q[f'requireditemcount{i}']
                    if item and count:
                        if item != start and item not in all_available_items:
                            why = why or 'item lacks drop'
                        else:
                            objectives.append({'type': 'collect', 'entry': item, 'count': count})

            if len(objectives) > 4:
                why = why or 'more than four objectives'

            try:
                rewards = quest_rewards(q)
                reputation = quest_reputation_rewards(q)
            except ValueError:
                why = why or 'invalid reward'; rewards = {}; reputation = []

            if any(r['itemId'] not in items for r in rewards.get('rewardChoices', []) + rewards.get('additionalRewards', [])) or \
                    (rewards.get('rewardItem') and rewards['rewardItem'] not in items):
                why = why or 'reward item not in catalog'

            if not why and not objectives:
                if not q['logdescription'] and not q['questdescription']:
                    why = 'empty definition'
                else:
                    end_target = enders[qid]
                    objectives = [{'type': 'talk', 'entry': end_target, 'count': 1}]

            if why:
                reasons[why] += 1
                continue

            giver_e = starters[qid]
            turnin_e = enders[qid]
            added[qid] = {
                'id': qid,
                'title': clean(q['logtitle']),
                'description': clean(q['logdescription'] or q['questdescription'], 1024),
                'giverEntry': giver_e,
                'turnInEntry': turnin_e,
                'minLevel': max(1, min(80, q['minlevel'])),
                'allowableRaces': max(0, q['allowableraces']),
                'allowableClasses': max(0, a.get('allowableclasses', 0)),
                'prerequisite': max(0, a.get('prevquestid', 0)),
                'xp': max(50, q['questlevel'] * 80),
                'requiredMinRepFaction': max(0, a.get('requiredminrepfaction', 0)),
                'requiredMinRepValue': a.get('requiredminrepvalue', 0),
                'requiredMaxRepFaction': max(0, a.get('requiredmaxrepfaction', 0)),
                'requiredMaxRepValue': a.get('requiredmaxrepvalue', 0),
                'reputationRequirements': [{'factionId': q[f'requiredfactionid{i}'], 'value': q[f'requiredfactionvalue{i}']}
                                           for i in (1, 2) if q[f'requiredfactionid{i}']],
                'money': max(0, q['rewardmoney']),
                **rewards,
                'reputationRewards': reputation,
                'objectives': objectives,
                'startItem': start,
                'startItemCount': max(1, min(255, a.get('provideditemcount', 0) or 1)) if start else 0,
            }

        while True:
            gone = [qid for qid, q in added.items() if q['prerequisite'] and q['prerequisite'] not in quests and q['prerequisite'] not in added]
            if not gone: break
            for qid in gone: del added[qid]; reasons['prerequisite excluded'] += 1

        print(json.dumps({'added': len(added), 'reasons': dict(reasons.most_common(10))}))

        if args.dry_run:
            return

        quests.update(added)
        for qid, q in added.items():
            for e in (q['giverEntry'], q['turnInEntry']):
                ids = contacts.setdefault(e, [])
                if qid not in ids: ids.append(qid); ids.sort()
                if e in npcs:
                    npcs[e]['questGiver'] = True

        # Ensure creatures dropping quest items for newly added quests carry the loot entry
        for q in added.values():
            for obj in q['objectives']:
                if obj['type'] == 'collect':
                    item_id = obj['entry']
                    for npc_entry in attackable:
                        if npc_entry in npcs:
                            lootid = templates[npc_entry]['lootid']
                            for drop in c_drops_by_npc.get(lootid, []):
                                if drop['item'] == item_id:
                                    loot_list = npcs[npc_entry].setdefault('loot', [])
                                    if not any(x['itemId'] == item_id for x in loot_list):
                                        loot_list.append({'itemId': item_id, 'count': max(1, min(65535, drop['mincount']))})

        record_pack(args.catalog / 'quests.pack', quests)
        record_pack(args.catalog / 'contacts.pack', contacts)
        record_pack(args.catalog / 'npcs.pack', npcs)

        manifest_path = args.catalog / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        for name in ('quests.pack', 'contacts.pack', 'npcs.pack'):
            data = (args.catalog / name).read_bytes()
            manifest['files'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
        manifest.pop('fingerprint', None)
        manifest['fingerprint'] = int.from_bytes(hashlib.sha256(encode(manifest)).digest()[:4], 'little') or 1
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
        print(f"Catalog updated: {len(quests)} total quests, manifest fingerprint {manifest['fingerprint']}")


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Add the quests that hand the player an item (quest_template.StartItem) to
the shipped world catalog.

import_world_catalog.py refused every quest with a StartItem, which is 1,379
quests at the pinned commit: letters to deliver, orders, samples. The realm
now gives that item on accept and takes it back on abandon and at the
turn-in (LocalQuestDefinition::startItem), so those quests are admitted under
the same rules as the rest of the catalog - a spawned creature giver and
ender, kill targets that can be attacked, collected items that a catalog
creature drops - with one addition: the start item itself may be the item
to deliver. Everything is checked against the catalog as shipped (its
spawns, creatures, drops and items), which is only extended:

  quests.pack    the new quest records (with startItem/startItemCount);
  contacts.pack  their giver and ender;
  npcs.pack      questGiver on a creature that becomes one.

Re-run tools/local_realm/import_quest_chains.py afterwards.

    python3 -B tools/local_realm/extend_start_item_quests.py [--catalog DIR]
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
    args = ap.parse_args()
    quests, contacts, npcs = (load(args.catalog / f) for f in ('quests.pack', 'contacts.pack', 'npcs.pack'))
    items = load(args.catalog / 'items.pack')
    spawns = (args.catalog / 'spawns.pack').read_bytes()
    spawned = {struct.unpack_from('<I', spawns, i + 4)[0] for i in range(0, len(spawns), 28)}
    with tempfile.TemporaryDirectory() as tmp, tarfile.open(args.sources) as tar:
        for t in ('quest_template', 'quest_template_addon', 'creature_queststarter', 'creature_questender', 'creature_template'):
            tar.extract(f'{t}.sql', tmp, filter='data')
        rows = {t: list(sql_rows(Path(tmp) / f'{t}.sql', t)) for t in
                ('quest_template', 'quest_template_addon', 'creature_queststarter', 'creature_questender', 'creature_template')}
    templates = {t['entry']: t for t in rows['creature_template']}
    present = {e for e in npcs if e in spawned}
    starters, enders = {}, {}
    for r in rows['creature_queststarter']:
        if r['id'] in present: starters.setdefault(r['quest'], r['id'])
    for r in rows['creature_questender']:
        if r['id'] in present: enders.setdefault(r['quest'], r['id'])
    attackable = {e for e in present if templates.get(e, {}).get('faction') in NEUTRAL_FACTIONS | AGGRESSIVE_FACTIONS
                  and not templates[e]['npcflag']}
    dropped = {s['itemId'] for v in npcs.values() for s in v.get('loot', [])}
    addons = {r['id']: r for r in rows['quest_template_addon']}
    added, reasons = {}, collections.Counter()
    for q in rows['quest_template']:
        qid = q['id']; a = addons.get(qid, {})
        if qid in quests or not q['startitem']: continue
        start = q['startitem']; why = None; objectives = []
        if start not in items: why = 'start item not in catalog'
        elif qid not in starters or qid not in enders: why = 'no catalog creature starter/ender'
        elif q['timeallowed'] or q['requiredplayerkills']: why = 'timed/PvP objective'
        elif q['rewardspell'] or q['rewardtitle']: why = 'spell/title reward'
        elif a.get('specialflags', 0) & ~4: why = 'repeat/script/exploration'
        elif a.get('exclusivegroup', 0) or a.get('prevquestid', 0) < 0: why = 'exclusive/group prerequisite'
        elif any(a.get(k, 0) for k in ('sourcespellid', 'requiredskillid', 'rewardmailtemplateid')): why = 'skill/spell/mail condition'
        elif any(q.get(k, 0) for k in ('requireditemid5', 'requireditemid6')): why = 'unsupported extra objective'
        elif q['rewardmoney'] < 0: why = 'money payment'
        for i in range(1, 5):
            entry, count = q[f'requirednpcorgo{i}'], q[f'requirednpcorgocount{i}']
            if entry < 0: why = why or 'gameobject objective'
            elif entry and count:
                if entry not in attackable: why = why or 'unsupported/nonattackable kill target'
                else: objectives.append({'type': 'kill', 'entry': entry, 'count': count})
            item, count = q[f'requireditemid{i}'], q[f'requireditemcount{i}']
            if item and count:
                if item != start and item not in dropped: why = why or 'item lacks a catalog drop'
                else: objectives.append({'type': 'collect', 'entry': item, 'count': count})
        if len(objectives) > 4: why = why or 'more than four objectives'
        try:
            rewards = quest_rewards(q); reputation = quest_reputation_rewards(q)
        except ValueError:
            why = why or 'invalid reward'; rewards = {}; reputation = []
        if any(r['itemId'] not in items for r in rewards.get('rewardChoices', []) + rewards.get('additionalRewards', [])) or \
                (rewards.get('rewardItem') and rewards['rewardItem'] not in items):
            why = why or 'reward item not in catalog'
        if not why and not objectives:
            if not q['logdescription'] and not q['questdescription']: why = 'empty definition'
            else: objectives = [{'type': 'talk', 'entry': enders[qid], 'count': 1}]
        if why: reasons[why] += 1; continue
        added[qid] = {'id': qid, 'title': clean(q['logtitle']), 'description': clean(q['logdescription'] or q['questdescription'], 1024),
                      'giverEntry': starters[qid], 'turnInEntry': enders[qid], 'minLevel': max(1, min(80, q['minlevel'])),
                      'allowableRaces': max(0, q['allowableraces']), 'allowableClasses': max(0, a.get('allowableclasses', 0)),
                      'prerequisite': max(0, a.get('prevquestid', 0)), 'xp': max(50, q['questlevel'] * 80),
                      'requiredMinRepFaction': max(0, a.get('requiredminrepfaction', 0)),
                      'requiredMinRepValue': a.get('requiredminrepvalue', 0),
                      'requiredMaxRepFaction': max(0, a.get('requiredmaxrepfaction', 0)),
                      'requiredMaxRepValue': a.get('requiredmaxrepvalue', 0),
                      'reputationRequirements': [{'factionId': q[f'requiredfactionid{i}'], 'value': q[f'requiredfactionvalue{i}']}
                                                 for i in (1, 2) if q[f'requiredfactionid{i}']],
                      'money': max(0, q['rewardmoney']), **rewards, 'reputationRewards': reputation, 'objectives': objectives,
                      'startItem': start, 'startItemCount': max(1, min(255, a.get('provideditemcount', 0) or 1))}
    # A prerequisite outside the catalog (old or new) removes the quest, as the importer does.
    while True:
        gone = [qid for qid, q in added.items() if q['prerequisite'] and q['prerequisite'] not in quests and q['prerequisite'] not in added]
        if not gone: break
        for qid in gone: del added[qid]; reasons['prerequisite excluded'] += 1
    quests.update(added)
    for qid, q in added.items():
        for e in (q['giverEntry'], q['turnInEntry']):
            ids = contacts.setdefault(e, [])
            if qid not in ids: ids.append(qid); ids.sort()
            npcs[e]['questGiver'] = True
    record_pack(args.catalog / 'quests.pack', quests)
    record_pack(args.catalog / 'contacts.pack', contacts)
    record_pack(args.catalog / 'npcs.pack', npcs)
    # The manifest's sizes and hashes, and its fingerprint as the importer derives it.
    manifest_path = args.catalog / 'manifest.json'
    manifest = json.loads(manifest_path.read_text())
    for name in ('quests.pack', 'contacts.pack', 'npcs.pack'):
        data = (args.catalog / name).read_bytes()
        manifest['files'][name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
    manifest.pop('fingerprint', None)
    manifest['fingerprint'] = int.from_bytes(hashlib.sha256(encode(manifest)).digest()[:4], 'little') or 1
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps({'added': len(added), 'quests': len(quests), 'excluded': dict(reasons.most_common())}))


if __name__ == '__main__':
    main()

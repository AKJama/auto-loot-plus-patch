"""Generates autosmash.impact / autosmash.shutdown.impact / autosmash.data.json.

Pattern taken from the game's own looping range program 4739af2f (GetParent -> transform ->
ForEachEntityWithFactionInRange -> HasEntityTag filter -> wait with ltime/rvm -> repeat).

Parameter push order of an ecall: inputs reversed, then configs reversed, then outputs reversed.

Usage: python build_program.py [--debug]
  --debug  adds firework markers and a weak hit on nearby enemies to see what the program reaches
"""
import json
import os
import sys

DEBUG = '--debug' in sys.argv
# --safetest: no damage at all; fireworks at the player when a base is detected and at every
# object that would be smashed. Used to verify the base protection before smashing is enabled.
SAFETEST = '--safetest' in sys.argv
# --diag: smashes normally; fireworks at the player every second while the base branch is taken
DIAG = '--diag' in sys.argv
HERE = os.path.dirname(os.path.abspath(__file__))
SMASH_TAG = 'a5c0de5a-1e00-4d0d-9a11-0000000a5001'          # world-only objects: always smash
SHARED_TAG = 'a5c0de5a-1e00-4d0d-9a11-0000000a5003'         # also buildable (barrels, sacks): only away from bases
ALTAR_TAG = 'a5c0de5a-1e00-4d0d-9a11-0000000a5004'          # build inhibitor at every flame altar (has a position)
STONE_TAG = 'a5c0de5a-1e00-4d0d-9a11-0000000a5005'          # rock boulders: skipped while StoneTag::value carries this tag
TREE_TAG = 'a5c0de5a-1e00-4d0d-9a11-0000000a5007'           # world trees (switch "fell trees", set by mod.lua at load)
OFF_TAG = 'a5c0de5a-1e00-4d0d-9a11-0000000a5009'            # carried by nothing: a tag slot set to it matches no object
BASE_RANGE = 175.0                                          # max build zone 160 m + smash radius (Lua rewrites it)
# Live switching (1.1.0): the program is always installed. mod.lua writes the config into the data block
# when the game loads, dinput8.dll rewrites the same slots in memory while the game runs:
#   Range::value 0 = auto smash off, SharedTag::value = OFF_TAG = buildable furniture off,
#   StoneTag::value = STONE_TAG = rock boulders are skipped (stones off), OFF_TAG = smashed.
FIREWORKS = '0324eec6-708b-4887-8ed2-7f49168b4e96'   # VFX_Player_Consumable_Fireworks_05_Impact
ENEMY_FACTIONS = ['Fogger', 'Vukah', 'Scavenger', 'Wildlife', 'WildBeast', 'Skeleton', 'Cyclops', 'Drak', 'Ancients']


def spawn_marker(call_id, pos_var):
    return ['iconst0'] * 5 + [f'iconst {pos_var}', 'iconst0', 'iconst Marker::template',
            'iconst Spawn::copyParentFaction', 'iconst Spawn::copyParentLevel', 'iconst Spawn::addDespawnLink',
            'iconst Spawn::breakImpactHierarchy', 'iconst0', f'ecall {call_id} SpawnEntity', 'pop']


def apply_damage(call_id, setup, target):
    return (['iconst0'] * 8 + ['iconst WeaponCategory::value', 'iconst0', f'iconst {setup}',
            'iconst HitReaction::value', 'iconst Pushback::value'] + ['iconst0'] * 6 +
            ['iconst HitDirection::value', 'iconst HitNormal::value', 'iconst Target::position',
             f'iconst {target}', 'iconst Root::entityId', 'iconst0', f'ecall {call_id} ApplyDamage', 'pop'])


def faction_loop_call(call_id, faction_var, index_var, entity_var, node='ForEachEntityWithFactionInRange',
                      range_var='Range::value'):
    return [f'iconst {range_var}', 'iconst Root::transform', f'iconst {faction_var}', f'iconst {index_var}',
            'iconst0', f'iconst {entity_var}', f'ecall {call_id} {node}']


code = ['label_0:',
        'iconst Root::entityId', 'ecall 3831000001 GetParent', 'pop',
        'iconst Root::entityId', 'iconst Root::position', 'iconst Root::transform', 'ecall 3831000002 GetTransform', 'pop']
if DEBUG:
    code += spawn_marker(3831000010, 'Root::position')
    code += ['iconst0', 'gstore EnemyLoop::index', 'label_e:'] + \
        faction_loop_call(3831000006, 'EnemyLoop::faction', 'EnemyLoop::index', 'EnemyLoop::entityId') + ['brt label_eb']
# Player in a base (the game's safe zone)? Then nothing is smashed at all.
# Up to 1.1.3 "near a build zone" (IsNearBuildingZone) and a flame altar search stopped ALL smashing. The game
# reports "near a build zone" also far away from any base (e.g. at the big bridge), so it looked as if smashing
# had stopped; the altar search never found its targets. Tested 22.10.2026 with fireworks markers. Now
# "near a build zone" only protects buildable objects (the player's own furniture stands in a base).
code += ['iconst Root::entityId', 'iconst InSafeZone::state', 'ecall 3831000017 IsInState', 'brt label_base']
code += ['iconst0', 'gstore PropLoop::index', 'label_1:'] + \
    faction_loop_call(3831000003, 'PropLoop::faction', 'PropLoop::index', 'PropLoop::entityId') + ['brt label_4']
code += ['label_wait:', 'gload WaitDuration::value', 'ltime', 'iadd',
         'label_2:', 'dup', 'ltime', 'ileq', 'brt label_3', 'rvm', 'br label_2',
         'label_3:', 'pop', 'rvm', 'br label_0', 'halt']
# base detected: in safetest mark the player, then wait
code += ['label_base:']
if SAFETEST or DIAG:
    code += spawn_marker(3831000019, 'Root::position')
code += ['br label_wait', 'halt']
# prop found: objects carrying the stone tag are skipped (slot = OFF_TAG while stones are collected),
# world-only objects outside a base, buildable ones (switch "also smash buildable furniture") only when the
# player is not near a build zone either
code += ['label_4:', 'iconst StoneTag::value', 'iconst PropLoop::entityId', 'ecall 3831000020 HasEntityTag', 'brt label_1',
         'iconst SmashTag::value', 'iconst PropLoop::entityId', 'ecall 3831000013 HasEntityTag', 'brt label_5',
         'iconst SharedTag::value', 'iconst PropLoop::entityId', 'ecall 3831000016 HasEntityTag', 'brt label_shared',
         # trees (1.1.7): only tagged when "fell trees" is on; protected like buildable furniture. A trigger by
         # player state did not work ("Mining" never fired with the axe, sneaking is no good, 23.10.2026); a real
         # hotkey needs a state bit set by the DLL (open)
         'iconst TreeTag::value', 'iconst PropLoop::entityId', 'ecall 3831000021 HasEntityTag', 'brt label_shared',
         'br label_1',
         'label_shared:', 'iconst Root::entityId', 'iconst NearBuildZone::state', 'ecall 3831000018 IsInState', 'brt label_1',
         'label_5:', 'iconst PropLoop::entityId', 'iconst Target::position', 'iconst0', 'ecall 3831000004 GetTransform', 'pop']
if DEBUG or SAFETEST:
    code += spawn_marker(3831000011, 'Target::position')
if not SAFETEST:
    code += apply_damage(3831000005, 'Smash::damageSetup', 'PropLoop::entityId')
code += ['br label_1', 'halt']
if DEBUG:
    code += ['label_eb:', 'iconst EnemyLoop::entityId', 'iconst Target::position', 'iconst0',
             'ecall 3831000007 GetTransform', 'pop'] + spawn_marker(3831000012, 'Target::position') + \
        apply_damage(3831000008, 'Probe::damageSetup', 'EnemyLoop::entityId') + ['br label_e', 'halt']

shutdown = faction_loop_call(3831000003, 'PropLoop::faction', 'PropLoop::index', 'PropLoop::entityId',
                             'DestroyFactionQuery') + ['pop']
if DEBUG:
    shutdown += faction_loop_call(3831000006, 'EnemyLoop::faction', 'EnemyLoop::index', 'EnemyLoop::entityId',
                                  'DestroyFactionQuery') + ['pop']
shutdown += ['halt']

zero3 = {'x': 0.0, 'y': 0.0, 'z': 0.0}
def setup(blunt, wood):
    return {'physicalCutDamage': 0.0, 'physicalPierceDamage': 0.0, 'physicalBluntDamage': blunt,
            'magicalFireDamage': 0.0, 'magicalIceDamage': 0.0, 'magicalFogDamage': 0.0,
            'magicalLightningDamage': 0.0, 'healing': 0.0, 'poisonDamage': 0.0,
            'woodDamage': wood, 'stoneDamage': wood, 'metalDamage': wood, 'explosionDamage': wood}
def var(name, typ, data):
    return {'name': name, 'type': typ, 'configId': 0, 'data': data}

data = [
    var('Root::entityId', 'keen::ecs::EntityId', {'id': 0}),
    var('Root::transform', 'keen::WorldTransform', {'position': {'x': 0, 'y': 0, 'z': 0},
        'orientation': {'x': 0.0, 'y': 0.0, 'z': 0.0, 'w': 0.0}, 'scale': {'x': 0.0, 'y': 0.0, 'z': 0.0}}),
    var('Root::position', 'keen::float3', zero3),
    var('Range::value', 'keen::float32', 15.0),
    var('WaitDuration::value', 'keen::Time', {'value': 1000000000}),
    var('PropLoop::index', 'keen::uint32', 0),
    var('PropLoop::entityId', 'keen::ecs::EntityId', {'id': 0}),
    var('PropLoop::faction', 'keen::FactionMask', ['Environment']),
    var('SmashTag::value', 'keen::ecs::EntityTagReference', SMASH_TAG),
    var('SharedTag::value', 'keen::ecs::EntityTagReference', SHARED_TAG),
    var('InSafeZone::state', 'keen::actor::StateFlag', 'InSaveZone'),
    var('AltarTag::value', 'keen::ecs::EntityTagReference', ALTAR_TAG),
    var('BaseRange::value', 'keen::float32', BASE_RANGE),
    var('AltarLoop::index', 'keen::uint32', 0),
    var('AltarLoop::entityId', 'keen::ecs::EntityId', {'id': 0}),
    var('NearBuildZone::state', 'keen::actor::StateFlag', 'IsNearBuildingZone'),
    var('Target::position', 'keen::float3', zero3),
    var('HitNormal::value', 'keen::float3', {'x': 0.0, 'y': 1.0, 'z': 0.0}),
    var('HitDirection::value', 'keen::impact::MajorDirection', 'BackToFront'),
    var('WeaponCategory::value', 'keen::WeaponCategoryReference', '3832918e-d03e-43c8-9230-eb35bc366107'),
    var('HitReaction::value', 'keen::impact::HitReactionType', 'None'),
    var('Pushback::value', 'keen::impact::PushBackForce', 'None'),
    var('Smash::damageSetup', 'keen::impact::DamageSetup', setup(100000.0, 100000)),
    var('StoneTag::value', 'keen::ecs::EntityTagReference', OFF_TAG),
    # appended after the 544-byte block the DLL patches live: its offsets stay unchanged
    var('TreeTag::value', 'keen::ecs::EntityTagReference', TREE_TAG),
]
query_ids = [3831000003]
MARKER_VARS = [
    var('Marker::template', 'keen::ecs::TemplateReference', FIREWORKS),
    var('Spawn::copyParentFaction', 'keen::bool', False),
    var('Spawn::copyParentLevel', 'keen::bool', False),
    var('Spawn::addDespawnLink', 'keen::bool', False),
    var('Spawn::breakImpactHierarchy', 'keen::bool', True),
]
if (SAFETEST or DIAG) and not DEBUG:
    data += MARKER_VARS
if DEBUG:
    data += [
        var('EnemyLoop::index', 'keen::uint32', 0),
        var('EnemyLoop::entityId', 'keen::ecs::EntityId', {'id': 0}),
        var('EnemyLoop::faction', 'keen::FactionMask', ENEMY_FACTIONS),
        var('Probe::damageSetup', 'keen::impact::DamageSetup', setup(5.0, 0)),
        var('Marker::template', 'keen::ecs::TemplateReference', FIREWORKS),
        var('Spawn::copyParentFaction', 'keen::bool', False),
        var('Spawn::copyParentLevel', 'keen::bool', False),
        var('Spawn::addDespawnLink', 'keen::bool', False),
        var('Spawn::breakImpactHierarchy', 'keen::bool', True),
    ]
    query_ids.append(3831000006)

with open(os.path.join(HERE, 'autosmash.impact'), 'w') as f:
    f.write('\n'.join(code) + '\n')
with open(os.path.join(HERE, 'autosmash.shutdown.impact'), 'w') as f:
    f.write('\n'.join(shutdown) + '\n')
with open(os.path.join(HERE, 'autosmash.data.json'), 'w') as f:
    json.dump({'data': data, 'queryCallIds': query_ids, 'usedStreams': []}, f, indent=2)
print(f"{'safetest' if SAFETEST else 'debug' if DEBUG else 'release'} program: {len(code)} lines, {len(data)} vars")

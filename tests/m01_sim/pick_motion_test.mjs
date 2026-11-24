import assert from 'node:assert/strict';
import test from 'node:test';
import { createPickMotion, samplePickMotion } from '../../web/m01_sim/pick-motion.mjs';

const meta = { pickBegin: 2400, beltWidth: 800, objects: [{ x: 250, y: 335, height: 66 }] };
const contact = [2410, 332.8125, 65.854836];
const actions = [
  ['admitted', 2086], ['contact', 2172, contact], ['holding', 2172],
  ['released', 2257], ['robot-ready', 2302],
];

function build(states) {
  return createPickMotion({ meta, events: states.map(([state, t, point]) =>
    ({ type: 'pick-action', t, data: { state, point } })) });
}

function sample(motion, time) {
  return samplePickMotion(motion, time, time, 2172);
}

test('contact is anchored to the reported point and transfers the original bottle', () => {
  const motion = build(actions);
  assert.equal(sample(motion, 2085).stage, 'standby');
  assert.equal(sample(motion, 2120).stage, 'approach');
  assert.equal(sample(motion, 2150).stage, 'descend');
  const atContact = sample(motion, 2172);
  assert.equal(atContact.stage, 'holding');
  assert.deepEqual(atContact.tool, contact);
  assert.deepEqual(atContact.bottle, [2422, 335, 66]);

  const transfer = sample(motion, 2225);
  assert.equal(transfer.stage, 'transfer');
  assert.equal(transfer.bottle[0] - transfer.tool[0], 12);
  assert.ok(transfer.bottle[2] > atContact.bottle[2]);

  const release = sample(motion, 2257);
  assert.equal(release.stage, 'released');
  assert.deepEqual(release.bottle, [motion.bin.x, motion.bin.y, 66]);
  const home = sample(motion, 2302);
  assert.equal(home.stage, 'ready');
  assert.deepEqual(home.tool, motion.home);
  assert.deepEqual(home.bottle, release.bottle);
  assert.deepEqual(sample(motion, 2172), atContact);
});

test('uncertain or missing controller effects never animate a completed pick', () => {
  for (const [events, expectedStage] of [
    [[], 'standby'],
    [[['admitted', 2086]], 'standby'],
    [[['pending', 2086]], 'pending'],
    [actions.slice(0, 2), 'contact'],
    [actions.slice(0, 3), 'holding'],
    [actions.slice(0, 4), 'released'],
  ]) {
    const pose = sample(build(events), 2550);
    assert.equal(pose.stage, expectedStage);
  }
});

test('MuJoCo replay follows recorded bottle and tool poses through physical completion', () => {
  const physicalMeta = { ...meta, pickBackend: 'mujoco' };
  const events = [
    { type: 'pick-action', t: 2360, data: { state: 'holding' } },
    { type: 'pick-physics', t: 2360, data: { stage: 'lift', cup: [2600, 332, 66],
      bottle: [2610, 335, 33], bottleQuat: [1, 0, 0, 0], beltMm: 2360 } },
    { type: 'pick-physics', t: 2370, data: { stage: 'lift', cup: [2605, 332, 68],
      bottle: [2615, 335, 35], bottleQuat: [1, 0, 0, 0], beltMm: 2370 } },
    { type: 'pick-result', t: 3681, data: { state: 'terminal', outcome: 'picked' } },
  ];
  const motion = createPickMotion({ meta: physicalMeta, events });
  assert.equal(sample(motion, 2360).stage, 'holding');
  assert.deepEqual(sample(motion, 2365).tool, [2602.5, 332, 67]);
  assert.deepEqual(sample(motion, 2365).bottle, [2612.5, 335, 67]);
  assert.equal(sample(motion, 2365).travel, 2365);
  assert.equal(sample(motion, 3681).stage, 'picked');
  assert.equal(sample(createPickMotion({ meta: physicalMeta, events: [] }), 2360), null);
});

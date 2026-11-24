"""Private line protocol for the C++ single-bottle simulator's MuJoCo controller."""
import sys

import numpy as np

from .pet_pick import EXPECTED, PickPhysics, Task


def serve(case):
    physics = PickPhysics(None, case, retract_on_miss=True)
    print("ready", flush=True)
    last_tick = -1
    emitted_events = 0
    snapshot = physics.sample()
    for line in sys.stdin:
        kind, *fields = line.split()
        if kind == "pick" and physics.task is None:
            attempt, epoch, x, y, z, reference, mm_per_count, opens, closes, minimum, maximum, receipt = map(float, fields)
            opens = max(opens, receipt + minimum)
            closes = min(closes, receipt + maximum)
            if not opens < closes:
                raise ValueError("empty command contact window")
            physics.task = Task(1, int(attempt), np.array([x, y, z]) / 1000, reference, int(epoch),
                                mm_per_count / 1000, receipt / 1000, opens / 1000, closes / 1000,
                                physics.belt_stop_s, physics.belt_m_s)
            print("accepted", flush=True)
            continue
        if kind != "step" or len(fields) != 1:
            raise ValueError("invalid controller request")
        tick = int(fields[0])
        if not last_tick < tick <= 6000:
            raise ValueError("controller clock must advance within the run")
        if last_tick < 0 and tick > 0:
            emit_frame(snapshot)
        last_tick = tick
        # Evaluate actions through tick, then retain the integrated state for the
        # next request. A command accepted before this step may act on this tick.
        while round(physics.data.time * 1000) <= tick and physics.outcome is None:
            physics.step()
        events = physics.events[emitted_events:]
        emitted_events = len(physics.events)
        for event in events:
            name, at = event["event"], round(event["time_ms"])
            if name == "contact":
                print("event", at, name, *(value * 1000 for value in event["point_m"]), event["encoder"])
            elif name == "pick_failed":
                print("event", at, name, event["reason"])
            else:
                print("event", at, name)
        if tick % 10 == 0 or events or physics.outcome:
            sample = physics.sample()
            # mj_step has integrated the next state. A terminal result is
            # stamped at its evaluated time, before the following integration.
            at = round(sample["time_ms"])
            if at <= tick:
                emit_frame(sample)
            elif snapshot is not None and round(snapshot["time_ms"]) == tick:
                emit_frame(snapshot, stage=physics.stage, holding=sample["holding"])
        if physics.outcome:
            print("event", round(physics.data.time * 1000), "result", physics.outcome)
        print(".", flush=True)
        if physics.outcome:
            return
        snapshot = physics.sample()


def emit_frame(sample, *, stage=None, holding=None):
    print("frame", round(sample["time_ms"]), stage or sample["stage"],
          sample["belt_m"] * 1000, *(value * 1000 for value in sample["cup_m"]),
          *(value * 1000 for value in sample["bottle_m"]), *sample["bottle_quat"],
          int(sample["holding"] if holding is None else holding))


if __name__ == "__main__":
    if len(sys.argv) != 2 or sys.argv[1] not in EXPECTED:
        raise SystemExit("usage: python -m sim.mujoco.controller normal|slow|offset")
    serve(sys.argv[1])

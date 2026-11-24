"""Run the archived production PET task against a MuJoCo scene."""
import argparse
from contextlib import nullcontext
from dataclasses import dataclass
import json
from pathlib import Path
import time

import mujoco
import numpy as np

SCENE = Path(__file__).with_name("pet_pick.xml")
HOME = np.array([2.62, 0.54, 0.235])
BIN = np.array([2.65, 0.97, 0.19])
LIMITS = np.array([1.9, 1.5, 0.85])
ACCELERATION = np.array([22., 20., 12.])
EXPECTED = {"normal": "picked", "slow": "missed_contact_window", "offset": "no_contact"}


@dataclass(frozen=True)
class Task:
    id: int
    attempt: int
    point_m: np.ndarray
    reference_count: float
    epoch: int
    m_per_count: float
    handoff_s: float
    open_s: float
    close_s: float
    belt_stop_s: float
    belt_m_s: float


def read_task(path):
    report = json.loads(path.read_text(encoding="utf-8"))
    meta = report["meta"]
    material = meta["objects"]
    if (report["schema"] != 1 or not report["summary"]["passed"]
            or (meta["layout"], meta["scenario"]) != ("production", "pet-pick")
            or (meta["beltLength"], meta["beltWidth"], meta["speed"]) != (5500, 800, 1000)
            or (meta["pickBegin"], meta["pickEnd"]) != (2400, 2900)
            or len(material) != 1 or material[0]["material"] != "PET"
            or tuple(material[0][key] for key in ("x", "y", "length", "width", "height"))
            != (250, 335, 210, 66, 66)):
        raise ValueError("需要通过校验的 800 mm、1 m/s 单 PET 瓶抓取报告（pet-pick）。")
    tasks = [event["data"] for event in report["events"] if event["type"] == "pick-task"]
    if len(tasks) != 1:
        raise ValueError("报告必须包含一个 pick-task。")
    task = tasks[0]
    basis = next((event["data"] for event in report["events"] if event["type"] == "pick-position"
                  and event["data"].get("valid") and event["data"]["epoch"] == task["epoch"]
                  and event["data"]["id"] == task["id"]), None)
    if basis is None or not 0 < basis["mmPerCount"] or not (
        0 < task["handoffMs"] < task["openMs"] < task["closeMs"] < meta["motion"][-1]["endMs"]
    ):
        raise ValueError("任务的参考计数、运动范围或接触窗无效。")
    return Task(task["id"], task["attempt"], np.array(task["point"]) / 1000,
                task["referenceCount"], task["epoch"], basis["mmPerCount"] / 1000,
                task["handoffMs"] / 1000, task["openMs"] / 1000, task["closeMs"] / 1000,
                meta["motion"][-1]["endMs"] / 1000, meta["speed"] / 1000)


class PickPhysics:
    def __init__(self, task, case, *, retract_on_miss=False):
        self.task = task
        self.case = case
        self.retract_on_miss = retract_on_miss
        self.miss = None
        self.belt_m_s = task.belt_m_s if task else 1.
        self.belt_stop_s = task.belt_stop_s if task else 3.2
        self.model = mujoco.MjModel.from_xml_path(str(SCENE))
        self.data = mujoco.MjData(self.model)
        self.axes = [self.model.joint("gantry_" + axis).id for axis in "xyz"]
        self.qpos = self.model.jnt_qposadr[self.axes]
        self.qvel = self.model.jnt_dofadr[self.axes]
        self.motor = [self.model.actuator(axis + "_motor").id for axis in "xyz"]
        self.belt_joint = self.model.joint("belt_x").id
        self.belt_motor = self.model.actuator("belt_motor").id
        self.bottle = self.model.body("bottle").id
        self.cup = self.model.body("cup").id
        self.cup_geom = self.model.geom("cup_face").id
        self.seal = self.model.equality("suction").id
        self.bottle_geoms = {self.model.geom("bottle_" + part).id for part in ("body", "neck", "cap")}
        self.bin_floor = self.model.geom("bin_floor").id
        self.bin_wall = self.model.geom("bin_end_wall").id
        self.data.qpos[self.qpos] = HOME
        self.data.qvel[self.model.jnt_dofadr[self.belt_joint]] = self.belt_m_s
        self.data.qvel[self.model.joint("bottle_free").dofadr[0]] = self.belt_m_s
        self.speed = LIMITS.copy()
        if case == "slow":
            self.speed[2] = 0.08
        self.stage = "waiting"
        self.events = []
        self.samples = []
        self.outcome = None
        self.contact_seen = False
        self.depth_reached = False
        self.landed_at = None
        self.inside_since = None
        self.home_at = None
        self.lift_xy = None
        mujoco.mj_forward(self.model, self.data)

    def belt_position(self):
        return float(self.data.qpos[self.model.jnt_qposadr[self.belt_joint]])

    def encoder(self):
        return round(self.belt_position() / self.task.m_per_count)

    def sample(self):
        return {"time_ms": round(self.data.time * 1000, 4), "stage": self.stage,
                "belt_m": self.belt_position(), "cup_m": self.data.xpos[self.cup].tolist(),
                "bottle_m": self.data.xpos[self.bottle].tolist(),
                "bottle_quat": self.data.xquat[self.bottle].tolist(),
                "holding": bool(self.data.eq_active[self.seal])}

    def event(self, name, **details):
        self.events.append({"event": name, "time_ms": round(self.data.time * 1000, 4), **details})

    def track_point(self):
        point = self.task.point_m.copy()
        point[0] += (self.encoder() - self.task.reference_count) * self.task.m_per_count
        if self.case == "offset":
            point[1] += 0.10
        return point

    def drive(self, position, belt_tracking=False):
        feedforward = np.array([self.belt_m_s if belt_tracking else 0., 0., 0.])
        position = np.clip(position, self.model.jnt_range[self.axes, 0],
                           self.model.jnt_range[self.axes, 1])
        desired = np.clip(24 * (position - self.data.qpos[self.qpos]) + feedforward,
                          -self.speed, self.speed)
        velocity = self.data.ctrl[self.motor]
        change = ACCELERATION * self.model.opt.timestep
        self.data.ctrl[self.motor] = velocity + np.clip(desired - velocity, -change, change)

    def try_attach(self):
        for contact in self.data.contact:
            geoms = {int(contact.geom1), int(contact.geom2)}
            if self.cup_geom not in geoms or not geoms & self.bottle_geoms or contact.dist > 0:
                continue
            # Compare velocities at the actual contact point, including bottle rotation.
            jacobian_cup = np.zeros((3, self.model.nv))
            jacobian_bottle = np.zeros_like(jacobian_cup)
            mujoco.mj_jac(self.model, self.data, jacobian_cup, None, contact.pos, self.cup)
            mujoco.mj_jac(self.model, self.data, jacobian_bottle, None, contact.pos, self.bottle)
            relative = (jacobian_cup - jacobian_bottle) @ self.data.qvel
            normal_z = abs(float(contact.frame[2]))
            if not self.contact_seen:
                self.event("contact", point_m=contact.pos.tolist(), encoder=self.encoder(),
                           normal_z=normal_z, relative_speed_m_s=float(np.linalg.norm(relative)))
                self.contact_seen = True
            if normal_z < 0.87 or np.linalg.norm(relative[:2]) > 0.12 or abs(relative[2]) > 0.30:
                continue
            self.attach()
            self.event("holding", cup_m=self.data.xpos[self.cup].tolist())
            self.lift_xy = self.data.qpos[self.qpos[:2]].copy()
            self.stage = "lift"
            return

    def attach(self):
        # Store body2's current pose in body1's frame before activating the weld.
        rel = self.model.eq_data[self.seal]
        rel[:3] = 0.
        rel[3:6] = self.data.xmat[self.cup].reshape(3, 3).T @ (
            self.data.xpos[self.bottle] - self.data.xpos[self.cup])
        inverse = np.empty(4)
        mujoco.mju_negQuat(inverse, self.data.xquat[self.cup])
        mujoco.mju_mulQuat(rel[6:10], inverse, self.data.xquat[self.bottle])
        self.data.eq_active[self.seal] = True

    def update_landing(self):
        bottle = self.data.xpos[self.bottle]
        velocity = np.zeros(6)
        mujoco.mj_objectVelocity(self.model, self.data, mujoco.mjtObj.mjOBJ_BODY,
                                 self.bottle, velocity, 0)
        floor_contact = any(self.bin_floor in (contact.geom1, contact.geom2)
                            and bool({contact.geom1, contact.geom2} & self.bottle_geoms)
                            and contact.dist <= 0 for contact in self.data.contact)
        half_size = self.model.geom_size[self.bin_floor, :2] - self.model.geom_size[self.bin_wall, 0]
        rim = self.data.geom_xpos[self.bin_wall, 2] + self.model.geom_size[self.bin_wall, 2]
        inside = floor_contact and np.linalg.norm(velocity[3:]) < 0.08
        for geom in self.bottle_geoms:
            center = self.data.geom_xpos[geom]
            radius, half_length = self.model.geom_size[geom, :2]
            axis = self.data.geom_xmat[geom].reshape(3, 3)[:, 2]
            reach = radius + np.abs(axis) * half_length
            inside = (inside and np.all(np.abs(center[:2] - BIN[:2]) + reach[:2] <= half_size)
                      and center[2] + reach[2] < rim)
        if inside:
            if self.inside_since is None:
                self.inside_since = self.data.time
            if self.landed_at is None and self.data.time - self.inside_since >= 0.06:
                self.landed_at = self.data.time
                self.event("landed_in_bin", bottle_m=bottle.tolist())
        else:
            self.inside_since = None

    def step(self):
        now = round(self.data.time, 9)
        axis = self.data.qpos[self.qpos]
        self.data.ctrl[self.belt_motor] = self.belt_m_s if now < self.belt_stop_s else 0.
        target = HOME
        follow = False
        if self.stage == "waiting" and self.task and now >= self.task.handoff_s:
            self.stage = "approach"
            self.event("command_started", encoder=self.encoder())
        if self.stage in ("approach", "descend") and now > self.task.close_s:
            self.miss = ("no_seal" if self.contact_seen else
                         "no_contact" if self.depth_reached else "missed_contact_window")
            if not self.retract_on_miss:
                self.outcome = self.miss
                return
            self.event("pick_failed", reason=self.miss)
            self.stage = "retract"
        if self.stage in ("approach", "descend"):
            point = self.track_point()
            follow = 2.30 <= point[0] < 2.90
            target = np.array([point[0], point[1], 0.105])
            aligned = (abs(axis[0] - point[0]) <= 0.015 and abs(axis[1] - point[1]) <= 0.006
                       and abs(self.data.qvel[self.qvel[0]] - self.belt_m_s) < 0.12)
            if self.stage == "approach" and aligned and now >= self.task.open_s and 2.4 <= point[0] <= 2.9:
                self.stage = "descend"
                self.event("descend_started")
            if self.stage == "descend":
                target[2] = point[2] - 0.009
                self.depth_reached |= axis[2] < point[2] - 0.003
                if 2.4 <= axis[0] <= 2.9:
                    self.try_attach()
        elif self.stage == "lift":
            target = np.r_[self.lift_xy, 0.23]
            if axis[2] > 0.185:
                self.stage = "transfer"
        elif self.stage == "transfer":
            target = BIN
            if np.linalg.norm(axis - BIN) < 0.012 and np.linalg.norm(self.data.qvel[self.qvel]) < 0.20:
                self.data.eq_active[self.seal] = False
                self.event("released", cup_m=self.data.xpos[self.cup].tolist())
                self.stage = "return"
        elif self.stage in ("return", "retract"):
            if self.stage == "return":
                self.update_landing()
            if (self.home_at is None and np.linalg.norm(axis - HOME) < 0.007
                    and np.linalg.norm(self.data.qvel[self.qvel]) < 0.06):
                self.home_at = now
                self.event("home")
            if self.home_at is not None:
                if self.stage == "retract":
                    self.outcome = self.miss
                elif self.landed_at is not None and self.inside_since is not None:
                    self.outcome = "picked"
            if self.outcome:
                return
        if self.task and now >= self.task.close_s + (3.0 if self.retract_on_miss else 2.5):
            self.outcome = "missed_bin" if self.stage == "return" else "motion_timeout"
            return
        self.drive(target, belt_tracking=follow)
        mujoco.mj_step(self.model, self.data)
        # Synchronize derived poses and contacts to the integrated state.
        mujoco.mj_forward(self.model, self.data)
        if not self.retract_on_miss and round(self.data.time * 1000) % 10 == 0:
            self.samples.append({**self.sample(), "encoder": self.encoder()})

    def run(self, view):
        if view:
            import mujoco.viewer as viewer
            window = viewer.launch_passive(self.model, self.data)
        else:
            window = nullcontext()
        with window as display:
            if display is not None:
                with display.lock():
                    display.cam.type = mujoco.mjtCamera.mjCAMERA_FIXED
                    display.cam.fixedcamid = self.model.camera("station").id
            started = time.monotonic()
            next_draw = 0.
            while self.outcome is None:
                if display is not None and not display.is_running():
                    self.outcome = "viewer_closed"
                    break
                self.step()
                if display is not None and self.data.time >= next_draw:
                    display.sync()
                    next_draw += 0.012
                    time.sleep(max(0., started + self.data.time * 2 - time.monotonic()))
            if display is not None and display.is_running():
                display.sync()
                time.sleep(1.5)
        self.event("result", outcome=self.outcome)

    def result(self, source):
        return {"schema": 1, "engine": f"MuJoCo {mujoco.__version__}", "input_report": str(source),
                "case": self.case, "expected": EXPECTED[self.case], "outcome": self.outcome,
                "passed": self.outcome == EXPECTED[self.case], "step_ms": self.model.opt.timestep * 1000,
                "task": {"id": self.task.id, "attempt": self.task.attempt, "epoch": self.task.epoch,
                         "reference_count": self.task.reference_count, "point_m": self.task.point_m.tolist(),
                         "handoff_ms": self.task.handoff_s * 1000,
                         "contact_window_ms": [self.task.open_s * 1000, self.task.close_s * 1000]},
                "axis_speed_limits_m_s": self.speed.tolist(), "events": self.events, "samples": self.samples}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="现有 pet-pick 的 report.json")
    parser.add_argument("--output", required=True, type=Path, help="物理验证报告 JSON 路径")
    parser.add_argument("--case", default="normal", choices=EXPECTED)
    parser.add_argument("--view", action="store_true", help="打开 MuJoCo 窗口，以半速同步显示动作")
    args = parser.parse_args()
    if args.input.resolve() == args.output.resolve():
        parser.error("物理报告路径必须与输入报告不同。")
    try:
        physics = PickPhysics(read_task(args.input), args.case)
        physics.run(args.view)
        result = physics.result(args.input)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    except (OSError, KeyError, ValueError, TypeError) as error:
        parser.exit(2, f"{error}\n")
    print(f"{'PASS' if result['passed'] else 'FAIL'} {args.case}: {result['outcome']} · {args.output}")
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Exercise the M06 -> M07 -> MuJoCo -> M07 path through the simulator."""
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def run(binary, root, label, scenario, mode):
    path = root / label
    subprocess.run([str(binary), "--layout", "production", "--scenario", scenario,
                    "--mujoco", mode, "--output", str(path)], check=True)
    report = json.loads((path / "report.json").read_text(encoding="utf-8"))
    assert report["summary"]["passed"], report["summary"]["errors"]
    assert report["meta"]["pickBackend"] == "mujoco"
    events = report["events"]
    assert sum(e["type"] == "pick-command" for e in events) == 1
    assert any(file.is_file() for file in (path / "archive" / "critical" / "m07").rglob("head"))
    frames = [e for e in events if e["type"] == "pick-physics"]
    assert frames[0]["t"] == 0 and len(frames) > 250
    assert all(a["t"] < b["t"] for a, b in zip(frames, frames[1:]))
    assert all(len(frame["data"]["bottleQuat"]) == 4 for frame in frames)
    actions = {e["data"]["state"]: e["t"] for e in events if e["type"] == "pick-action"}
    results = {e["data"]["state"]: e for e in events if e["type"] == "pick-result"}
    return report, frames, actions, results


def main():
    binary = Path(sys.argv[1]).resolve()
    with TemporaryDirectory(prefix="m01-mujoco-") as directory:
        root = Path(directory)
        report, frames, actions, results = run(binary, root, "normal", "pet-pick", "normal")
        task = next(e["data"] for e in report["events"] if e["type"] == "pick-task")
        receipt = next(e["data"] for e in report["events"]
                       if e["type"] == "pick-action" and e["data"]["state"] == "receipt")
        assert task["openMs"] <= actions["contact"] <= actions["holding"] <= task["closeMs"]
        assert receipt["contactOpenMs"] <= actions["contact"] <= actions["holding"] <= receipt["contactCloseMs"]
        assert actions["receipt"] < actions["contact"] < actions["released"] < results["home"]["t"]
        assert results["home"]["t"] > 3200
        assert results["terminal"]["data"]["outcome"] == "picked"
        assert actions["resources-released"] >= max(results["home"]["t"], results["landed-in-bin"]["t"])
        assert report["meta"]["durationMs"] >= actions["resources-released"] == frames[-1]["t"]
        by_time = {frame["t"]: frame["data"] for frame in frames}
        assert by_time[actions["holding"]]["held"]
        assert not by_time[actions["released"]]["held"]
        assert not by_time[actions["resources-released"]]["held"]

        for mode, expected in (("slow", "missed_contact_window"), ("offset", "no_contact")):
            _, frames, actions, results = run(binary, root, mode, "pet-pick", mode)
            assert results["failed"]["data"]["outcome"] == results["terminal"]["data"]["outcome"] == expected
            assert results["failed"]["t"] < results["home"]["t"] <= actions["resources-released"]
            assert "holding" not in actions and "released" not in actions and "landed-in-bin" not in results
            assert all(not frame["data"]["held"] for frame in frames)

        _, frames, actions, results = run(binary, root, "receipt", "pet-receipt-loss", "normal")
        assert actions.keys() == {"pending"} and not results
        assert all(not frame["data"]["held"] for frame in frames)
    print("MuJoCo integration: pick, slow axis, offset and uncertain receipt verified")


if __name__ == "__main__":
    main()

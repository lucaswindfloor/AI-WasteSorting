"""Check the PET acceptance scenarios through the normal simulator CLI."""

import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


def check_run(binary, scenario, path):
    process = subprocess.run([binary, "--layout", "production", "--scenario", scenario, "--output", str(path)],
                             text=True, capture_output=True, timeout=150)
    assert (path / "report.json").exists(), process.stdout + process.stderr
    report = json.loads((path / "report.json").read_text())
    assert process.returncode == 0 and report["summary"]["passed"], report["summary"]["errors"]
    meta, summary = report["meta"], report["summary"]
    assert meta["beltWidth"] == 800 and meta["speed"] == 1000 and meta["m03Rows"] == 512
    assert (summary["rgbRows"], summary["independentHsi"], summary["depth"], summary["verifiedBlocks"]) == (
        3200, 640, 800, 4640)
    assert [item["material"] for item in meta["objects"]] == ["PET"]

    def events(kind):
        return [event for event in report["events"] if event["type"] == kind]

    associations = [event["data"] for event in events("pick-window")]
    complete = [event for event in associations if event["state"] == "matched"]
    assert len(complete) >= 3 and len({event["id"] for event in complete}) == 1
    assert associations[0]["state"] == associations[-1]["state"] == "partial"
    positions = [event["data"] for event in events("pick-position") if event["data"]["valid"]]
    assert all(abs(row["point"][0] - row["referencePoint"][0] -
                   (row["count"] - row["referenceCount"]) * 0.05) < 1e-4 for row in positions)
    assert all(a["count"] <= b["count"] and a["point"][0] <= b["point"][0]
               for a, b in zip(positions, positions[1:]))
    assert all(abs(row["point"][0] - row["count"] * 0.05 - 241.388) < 8 and
               abs(row["point"][1] - 335) < 4 for row in positions)
    tasks = [event["data"] for event in events("pick-task")]
    actions = events("pick-action")
    states = [event["data"]["state"] for event in actions]
    if scenario == "pet-pick":
        assert len(tasks) == 1
        task = tasks[0]
        assert task["id"] == complete[0]["id"] and task["sourceSamples"] > 1
        assert states == ["receipt", "admitted", "contact", "holding", "placement-ended", "released", "finished",
                          "tool-empty", "tool-healthy", "robot-ready", "resources-released"]
        contact = next(event for event in actions if event["data"]["state"] == "contact")
        assert task["openMs"] <= contact["t"] <= task["closeMs"]
        assert 2400 <= contact["data"]["point"][0] <= 2900 and contact["data"]["point"][2] > 0
    elif scenario == "pet-depth-loss":
        assert not tasks and not actions
        assert any(event["data"]["state"] == "depth-missing" for event in events("pick-eligibility"))
    elif scenario == "pet-encoder-loss":
        assert not actions
        assert any(not event["data"]["valid"] for event in events("pick-position"))
    else:
        assert len(tasks) == 1 and states == ["pending"]
    print(f"{scenario}: verified {len(complete)} window matches, {len(tasks)} handoffs, {len(actions)} actions")


if __name__ == "__main__":
    executable = str(Path(sys.argv[1]).resolve())
    with TemporaryDirectory(prefix="waste-pick-test-") as directory:
        for mode in ("pet-pick", "pet-depth-loss", "pet-encoder-loss", "pet-receipt-loss"):
            check_run(executable, mode, Path(directory) / mode)

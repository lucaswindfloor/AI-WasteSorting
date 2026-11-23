"""Exercise the simulator binary through its CLI and inspect the emitted evidence."""

import csv
import json
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from tempfile import TemporaryDirectory


def check_geometry(data):
    meta = data["meta"]
    bottle, can = meta["objects"]
    stations = bottle["footprint"]
    assert [along for along, _ in stations] == sorted({along for along, _ in stations})
    assert stations[0][0] == 0 and stations[-1][0] == 1
    assert max(width for _, width in stations) == 1
    assert stations[-3][1] > stations[-4][1]  # Cap widens again after the neck.
    width = meta["previewWidth"]
    belt_width = meta["beltWidth"]

    def on_material(x, lateral):
        for obj in (bottle, can):
            along = (x - obj["x"]) / obj["length"] + 0.5
            if not 0 < along < 1:
                continue
            for (a, w0), (b, w1) in zip(obj["footprint"], obj["footprint"][1:]):
                if a <= along <= b:
                    cross_width = obj["width"] * (w0 + (w1 - w0) * (along - a) / (b - a))
                    if abs(lateral - obj["y"]) < cross_width / 2:
                        return True
                    break
        return False

    for event in data["events"]:
        row = event["data"]
        if event["type"] == "result":
            assert row["displacementMm"] == row["trigger"] * meta["pitch"]
            x = meta["scanX"] - row["displacementMm"]
            for sensor in ("rgb", "hsi"):
                pixels = row[sensor + "Pixels"]
                if not pixels:
                    assert sensor == "hsi" and row["hsiQuality"] == "missing"
                    continue
                native = meta[sensor + "Width"]
                actual = ([pixels[i * 3 + 1] > 100 for i in range(width)] if sensor == "rgb"
                          else [value > 2500 for value in pixels])
                expected = [on_material(x, (int((i + 0.5) * native / width) + 0.5) * belt_width / native)
                            for i in range(width)]
                assert actual == expected, (meta["scenario"], row["trigger"], sensor)
        elif event["type"] == "depth":
            if meta["depthMode"] == "encoder":
                expected = row["trigger"] * meta["pitch"]
            else:
                session = next(part for part in meta["motion"] if part["startMs"] <= row["receivedMs"]
                               <= part["endMs"] + 0.760001)
                exposure = row["receivedMs"] - 0.76
                fraction = (exposure - session["startMs"]) / (session["endMs"] - session["startMs"])
                expected = session["begin"] + fraction * (session["end"] - session["begin"])
            assert abs(row["displacementMm"] - expected) < 1e-5


def run_case(binary, root, scenario, args, expected):
    out = root / scenario
    command = [binary, "--scenario", scenario, "--output", str(out), *args]
    run = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert run.returncode == 0, f"{scenario}: {run.stdout}\n{run.stderr}"
    data = json.loads((out / "report.json").read_text())
    check_geometry(data)
    summary = data["summary"]
    assert summary["passed"] and not summary["errors"], summary
    assert (summary["paired"], summary["missing"], summary["late"], summary["rejectedOld"]) == expected
    assert summary["archiveGaps"] == expected[1] and summary["manifest"] > 0
    assert data["events"][-1]["type"] == "seal"
    with (out / "ledger.csv").open(newline="") as source:
        rows = {int(row["trigger"]): row for row in csv.DictReader(source)}
    assert list(rows) == list(range(1, 241))
    assert all(int(row["m09_manifest"]) > 0 for row in rows.values())
    assert all(int(row["encoder_count"]) == 30 * int(row["local_trigger"]) for row in rows.values())
    if scenario in {"missing", "mixed"}:
        assert rows[87]["hsi_quality"] == "missing" and rows[88]["hsi_quality"] == "valid"
    if scenario in {"late", "mixed"}:
        assert rows[153]["hsi_quality"] == "missing" and rows[154]["hsi_quality"] == "valid"
        late = [item["data"] for item in data["events"] if item["type"] == "diagnostic"
                and item["data"]["issue"] == "Late"]
        assert late == [{"issue": "Late", "source": "HSI", "trigger": 153, "hasEvidence": True}]
    if scenario == "reconnect":
        assert rows[120]["generation"] == "1" and rows[121]["generation"] == "2"
        assert rows[121]["local_trigger"] == "1" and rows[121]["encoder_count"] == "30"
        assert rows[120]["rgb_session"] != rows[121]["rgb_session"]
    if scenario == "slow-archive":
        assert summary["peakArchiveQueue"] >= 2
    depths = [item["data"] for item in data["events"] if item["type"] == "depth"]
    assert len(depths) == summary["depth"] and any(item["masked"] > 0 for item in depths)
    if data["meta"]["depthMode"] == "encoder":
        assert all(item["trigger"] > 0 for item in depths)
    else:
        assert all(item["trigger"] == 0 for item in depths)
    final_health = next(event["data"] for event in reversed(data["events"]) if event["type"] == "health")
    assert final_health["bytes"] == 0 and final_health["inflight"] == final_health["archiveQueue"] == 0
    print(f"{scenario}: {summary['paired']} pairs, {summary['verifiedBlocks']} checked archive blocks")
    return data


def main():
    binary = str(Path(sys.argv[1]).resolve())
    cases = [
        ("normal", ["--packet-lines", "1", "--speed-mm-s", "3000"], (240, 0, 0, 0)),
        ("missing", ["--speed-mm-s", "500"], (239, 1, 0, 0)),
        ("late", ["--speed-mm-s", "2000", "--packet-lines", "8"], (239, 1, 1, 0)),
        ("mixed", [], (238, 2, 1, 0)),
        ("slow-archive", ["--packet-lines", "4", "--depth", "encoder"], (240, 0, 0, 0)),
        ("reconnect", ["--depth", "encoder", "--speed-mm-s", "2000"], (240, 0, 0, 1)),
    ]
    with TemporaryDirectory(prefix="waste-m01-sim-test-") as path:
        root = Path(path)
        with ThreadPoolExecutor(max_workers=2) as pool:
            jobs = [pool.submit(run_case, binary, root, *case) for case in cases]
            reports = [job.result() for job in jobs]
        original = (root / "mixed" / "ledger.csv").read_bytes()
        duplicate = subprocess.run([binary, "--output", str(root / "mixed")], capture_output=True, timeout=10)
        assert duplicate.returncode == 2 and (root / "mixed" / "ledger.csv").read_bytes() == original
        again = subprocess.run([binary, "--output", str(root / "again")], capture_output=True, timeout=90)
        assert again.returncode == 0
        assert json.loads((root / "again" / "report.json").read_text()) == reports[3]
    print("M01 simulator regression passed")


if __name__ == "__main__":
    main()

"""Exercise the simulator binary through its CLI and inspect the emitted evidence."""

import csv
import json
import math
import subprocess
import sys
from collections import Counter
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
            for sensor in ("rgb", "hsi"):
                x = (meta["hsiScanX"] - row["hsiDisplacementMm"] if sensor == "hsi"
                     else meta["scanX"] - row["displacementMm"])
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


def check_calibration(data, expected):
    rows = {event["data"]["trigger"]: event["data"] for event in data["events"] if event["type"] == "m02"}
    metrics = data["summary"]["m02"]
    assert len(rows) == metrics["processed"] == 240 and metrics["covered"] == expected
    e05 = [event["data"] for event in data["events"] if event["type"] == "m02-depth"]
    assert len(e05) == metrics["depthProjected"] == data["summary"]["depth"]
    assert metrics["validZeroPoints"] > 0 and metrics["maskedPoints"] > 0
    for depth in e05:
        assert depth["observation"]["exposureMs"] < depth["observation"]["receivedMs"]
        if depth["trigger"]:
            assert depth["observation"]["shiftMm"] == 0
            assert depth["observation"]["aligned"]
        else:
            assert depth["observation"]["shiftMm"] is None
    scenario = data["meta"]["scenario"]
    duration = 0 if scenario == "transport-delay" else 1
    sample = rows[100]
    for sensor in ("rgb", "hsi"):
        width = data["meta"][sensor + "Width"]
        scan_x = data["meta"]["hsiScanX" if sensor == "hsi" else "scanX"]
        for x, y, z, quality, column in sample[sensor]["measured"]:
            assert quality == "valid" and z == 0
            assert math.isclose(x, scan_x, abs_tol=1e-6)
            assert math.isclose(y, (column + 0.5) * data["meta"]["beltWidth"] / width, abs_tol=1e-6)
    if scenario in {"transport-delay", "exposure-delay", "compensated", "exposure-limit"}:
        assert math.isclose(sample["hsi"]["exposureMs"] - sample["rgb"]["exposureMs"], duration, abs_tol=1e-6)
        assert sample["hsi"]["shiftMm"] == -2 * duration
        assert sample["hsi"]["aligned"][0][0] == (140 if scenario == "exposure-delay" else 142)
    if scenario == "transport-delay":
        assert sample["hsi"]["receivedMs"] > sample["rgb"]["receivedMs"]
        assert sample["status"] == "available"
    if scenario == "exposure-delay":
        assert sample["status"] == "out_of_coverage" and not sample["pairs"]
    if scenario == "compensated":
        assert sample["hsi"]["measured"][0][0] == 144 and sample["pairs"]
    if scenario == "exposure-limit":
        assert sample["status"] == "exposure_separation" and not sample["pairs"]
    if scenario == "clock-missing":
        assert all(row["status"] == "unverified_time" and row["hsi"]["exposureMs"] is None for row in rows.values())
    if scenario == "count-interpolation":
        assert sample["hsi"]["countBasis"] == "interpolation"
        assert abs(sample["hsi"]["count"] - 3015) <= 0.01
        assert rows[240]["status"] == "unverified_count"
    if scenario == "invalid-white":
        assert metrics["spectralValid"] == 0 and sample["spectrum"]["reason"] == "invalid_reference"
    elif scenario != "clock-missing":
        spectrum = sample["spectrum"]
        assert math.isclose(spectrum["corrected"], (spectrum["raw"] - 100) / 30000, abs_tol=1e-6)
    if scenario in {"missing", "mixed"}:
        assert rows[87]["status"] == "missing_input"
    if scenario in {"late", "mixed"}:
        assert rows[153]["status"] == "missing_input"
    if scenario == "reconnect":
        assert rows[120]["geometry"] != rows[121]["geometry"]
        assert rows[120]["generation"] == 1 and rows[121]["generation"] == 2


def check_inference(data):
    meta, summary = data["meta"], data["summary"]["m03"]
    events = [event for event in data["events"] if event["type"] == "m03-window"]
    if meta["m03Mode"] == "off":
        assert summary is None and not events
        assert not any(event["type"].startswith("m03-") for event in data["events"])
        return
    windows = [event["data"] for event in events]
    mode, scenario, masks = meta["m03Mode"], meta["scenario"], meta["m03Mask"]
    branches = 2 if mode == "split" else 1
    generations = 2 if scenario in {"reconnect", "model-switch"} else 1
    assert len(windows) == (15 if generations == 1 else 14) * branches
    totals = Counter(row["state"] for row in windows)
    for field, state in (("detected", "completed"), ("empty", "no_detection"), ("invalid", "input_invalid"),
                         ("expired", "expired"), ("queueFull", "queue_full"), ("retired", "retired")):
        assert summary[field] == totals[state]
    assert totals["retired"] == branches * generations
    assert len({(w["first"], w["generation"], w["model"]["package"]) for w in windows}) == len(windows)
    last_health = next(event["data"] for event in reversed(data["events"]) if event["type"] == "m03-health")
    assert last_health["waiting"] == last_health["running"] == last_health["bytes"] == 0

    acquisition = {event["data"]["trigger"]: event["data"] for event in data["events"] if event["type"] == "result"}
    observed = set()
    for event in events:
        row = event["data"]
        assert row["last"] <= (120 if row["generation"] == 1 and generations == 2 else 240)
        assert row["model"]["version"] == str(row["generation"])
        assert row["reference"] == f"sim-dark-white.g{row['generation']}"
        assert (row["state"] == "completed") == bool(row["evidence"])
        if row["state"] == "retired":
            assert row["last"] - row["first"] + 1 < meta["m03Rows"]
            continue
        assert row["last"] - row["first"] + 1 == meta["m03Rows"]
        if row["state"] in {"completed", "no_detection"}:
            assert row["reason"] == ("" if row["state"] == "completed" else "no_valid_candidates")
            assert row["firstReceivedMs"] <= row["readyMs"] <= row["preparedMs"] <= row["startedMs"]
            assert row["startedMs"] <= row["backendMs"] <= row["finishedMs"] <= event["t"] < row["deadlineMs"]
        for candidate in row["evidence"]:
            observed.add(candidate["label"])
            x0, y0, x1, y1 = candidate["bounds"]
            assert x0 <= x1 and 0 <= y0 <= y1 <= meta["beltWidth"]
            assert candidate["points"] >= 3
            sensors = {"sim-rgb": {"rgb"}, "sim-hsi": {"hsi"}, "sim-joint": {"rgb", "hsi"}}
            assert {sensor for sensor in ("rgb", "hsi") if candidate[sensor]} == sensors[row["model"]["package"]]
            for sensor in sensors[row["model"]["package"]]:
                source = candidate[sensor]
                trigger = source["trigger"]
                assert row["first"] <= trigger <= row["last"]
                original = acquisition[trigger]
                assert original["segment"] == source["segment"]
                assert all(original[sensor][key] == source[key] for key in ("frame", "row", "session"))
                assert 0 <= source["column"] < meta[sensor + "Width"]
                distance = (trigger - original["local"] + source["count"] / 30) * meta["pitch"]
                x, y, _ = source["belt"]
                assert x0 - 1e-5 <= x - distance <= x1 + 1e-5 and y0 <= y <= y1

    if scenario in {"normal", "transport-delay", "compensated", "model-switch"}:
        assert "PET" in observed and "AL" in observed and totals["no_detection"] > 0
    if scenario == "exposure-delay":
        if mode == "joint":
            assert totals["input_invalid"] == 14
            assert all(row["reason"] == "too_many_missing_rows" and len(row["quality"]["missingTriggers"]) == 32
                       for row in windows if row["state"] == "input_invalid")
        else:
            assert totals["completed"] + totals["no_detection"] == 28 and observed == {"object", "PET", "AL"}
    if scenario == "mixed":
        if masks == "strict":
            assert totals["input_invalid"] == 4
            assert {trigger for row in windows if row["quality"] for trigger in row["quality"]["missingTriggers"]} == {87, 153}
        else:
            assert not totals["input_invalid"] and totals["completed"] > 0
    if scenario == "invalid-white":
        if masks == "strict":
            assert totals["input_invalid"] == 14
            assert all(row["reason"] == "insufficient_valid_samples" for row in windows if row["state"] == "input_invalid")
        else:
            assert not totals["input_invalid"] and totals["completed"] > 0
        assert all(row["quality"]["hsi"]["valid"] * meta["hsiBands"] ==
                   row["quality"]["hsi"]["total"] * (meta["hsiBands"] - 1) for row in windows if row["quality"])
    if scenario == "inference-overload":
        assert totals["expired"] > 0 and totals["queue_full"] > 0 and not observed
        assert summary["peakWaiting"] == 2 and summary["peakRunning"] == 1
    if scenario == "model-switch":
        deployments = [event for event in data["events"] if event["type"] == "m03-model"]
        rejected = next(event for event in deployments if event["data"]["action"] == "rejected")
        enabled = next(event for event in deployments if event["data"]["action"] == "activated" and event["data"]["generation"] == 2)
        initial = next(event for event in deployments if event["data"]["action"] == "activated" and event["data"]["generation"] == 1)
        assert rejected["t"] < enabled["t"] and summary["rejectedModels"] == 1
        assert [identity["version"] for identity in rejected["data"]["active"]] == ["1"]
        assert initial["data"]["models"][0]["digest"] != enabled["data"]["models"][0]["digest"]
        assert any(event["t"] > enabled["t"] and event["data"]["generation"] == 1 and
                   event["data"]["state"] in {"completed", "no_detection"} for event in events)


def run_case(binary, root, scenario, args, expected, covered, variant=""):
    out = root / (scenario + variant)
    command = [binary, "--scenario", scenario, "--output", str(out), *args]
    run = subprocess.run(command, capture_output=True, text=True, timeout=90)
    assert run.returncode == 0, f"{scenario}: {run.stdout}\n{run.stderr}"
    data = json.loads((out / "report.json").read_text())
    check_geometry(data)
    check_calibration(data, covered)
    check_inference(data)
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
    print(f"{scenario}: {summary['paired']} pairs, {covered} common-coverage triggers, {summary['verifiedBlocks']} checked archive blocks")
    return data


def run_plant(binary, root, speed, scenario):
    out = root / f"plant-{speed}"
    command = [binary, "--layout", "production", "--speed-mm-s", str(speed),
               "--scenario", scenario, "--output", str(out)]
    run = subprocess.run(command, capture_output=True, text=True, timeout=180)
    assert run.returncode == 0, run.stdout + run.stderr
    report = json.loads((out / "report.json").read_text())
    meta, summary = report["meta"], report["summary"]

    def events(kind):
        return [event["data"] for event in report["events"] if event["type"] == kind]

    assert meta["layout"] == "production"
    assert (meta["beltLength"], meta["beltWidth"], meta["scanX"], meta["hsiScanX"], meta["depthScanX"]) == (
        5500, 1000, 750, 750, 750)
    assert meta["scanX"] < meta["beltLength"] / 4
    assert meta["depthScanX"] < meta["pickBegin"] < meta["pickEnd"] < meta["beltLength"]
    assert [(item["length"], item["width"]) for item in meta["objects"]] == [(210, 66), (122, 66)]
    assert (meta["rgbWidth"], meta["packetLines"], meta["pitch"], meta["m03Rows"], meta["m03Step"]) == (
        8192, 128, 1, 512, 128)
    missing = scenario == "rgb-frame-loss"
    rgb = [entry for entry in events("packet") if entry["source"] == "RGB"]
    assert len(rgb) == 20 - missing
    assert all(entry["rows"] == 128 and entry["bytes"] == 128 * 8192 * 3 for entry in rgb)
    assert summary["passed"] and not summary["errors"]
    assert summary["rgbRows"] == 2560 - 128 * missing and summary["archiveGaps"] == 128 * missing
    assert {row["trigger"] for row in events("result") if row["rgbQuality"] == "missing"} == (
        set(range(385, 513)) if missing else set())
    assert all(row["hsi"] is None for row in events("result"))
    hsi = events("m02-hsi")
    assert len(hsi) == summary["independentHsi"] == 2560 // meta["hsiPitch"]
    assert all(row["scanX"] == meta["scanX"] and row["corrected"] is not None for row in hsi)
    assert hsi[1]["count"] - hsi[0]["count"] == meta["hsiPitch"] * 20
    rgb_aligned = [row["rgb"] for row in events("m02") if row["rgb"] is not None]
    assert len(rgb_aligned) == summary["rgbRows"]
    depth = [entry["observation"] for entry in events("m02-depth")]
    assert len(depth) == summary["depth"] > 0
    assert all(row["measured"][0][0] == meta["scanX"] for row in rgb_aligned + depth)
    complete = [row for row in events("m03-window") if row["state"] != "retired"]
    assert [(row["first"], row["last"]) for row in complete] == [
        (first, first + 511) for first in range(1, 2050, 128)]
    assert all(row["finishedMs"] < row["deadlineMs"] for row in complete)
    assert all(row["readyMs"] >= row["last"] * 1000 / speed - 0.01 for row in complete)
    assert {row["first"] for row in complete if row["state"] == "input_invalid"} == (
        {1, 129, 257, 385} if missing else set())
    candidates = [candidate for row in complete for candidate in row["evidence"]]
    assert any(300 < c["rgb"]["belt"][1] < 370 for c in candidates)  # PET in its lateral lane.
    assert any(615 < c["rgb"]["belt"][1] < 685 for c in candidates)  # Al in its lateral lane.
    print(f"production {speed} mm/s {scenario}: {summary['rgbRows']} RGB, {len(hsi)} HSI, {len(complete)} M03 windows")


def main():
    binary = str(Path(sys.argv[1]).resolve())
    cases = [
        ("normal", ["--packet-lines", "1", "--speed-mm-s", "3000"], (240, 0, 0, 0), 240),
        ("missing", ["--speed-mm-s", "500"], (239, 1, 0, 0), 239),
        ("late", ["--speed-mm-s", "2000", "--packet-lines", "8"], (239, 1, 1, 0), 239),
        ("mixed", [], (238, 2, 1, 0), 238),
        ("slow-archive", ["--packet-lines", "4", "--depth", "encoder"], (240, 0, 0, 0), 240),
        ("reconnect", ["--depth", "encoder", "--speed-mm-s", "2000"], (240, 0, 0, 1), 240),
        ("transport-delay", ["--speed-mm-s", "2000"], (240, 0, 0, 0), 240),
        ("exposure-delay", ["--speed-mm-s", "2000"], (240, 0, 0, 0), 0),
        ("compensated", ["--speed-mm-s", "2000"], (240, 0, 0, 0), 240),
        ("exposure-limit", ["--speed-mm-s", "2000"], (240, 0, 0, 0), 0),
        ("clock-missing", [], (240, 0, 0, 0), 0),
        ("count-interpolation", [], (240, 0, 0, 0), 239),
        ("invalid-white", [], (240, 0, 0, 0), 240),
        ("invalid-white", ["--m03-mask", "allow"], (240, 0, 0, 0), 240, "-masked"),
        ("mixed", ["--m03-mask", "allow"], (238, 2, 1, 0), 238, "-masked"),
        ("exposure-delay", ["--m03", "split", "--speed-mm-s", "2000"], (240, 0, 0, 0), 0, "-split"),
        ("model-switch", ["--speed-mm-s", "2000"], (240, 0, 0, 0), 240),
        ("inference-overload", ["--speed-mm-s", "2000"], (240, 0, 0, 0), 240),
        ("normal", ["--m03", "hsi"], (240, 0, 0, 0), 240, "-hsi"),
        ("normal", ["--m03", "off"], (240, 0, 0, 0), 240, "-off"),
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
        run_plant(binary, root, 1000, "normal")
        run_plant(binary, root, 3000, "rgb-frame-loss")
    print("M01/M02/M03 simulator regression passed")


if __name__ == "__main__":
    main()

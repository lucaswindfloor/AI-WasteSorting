"""Run BCAF RGB-512/HSI-5 on a calibrated simulator capture or a SpectralWaste NPZ."""

import argparse
from collections.abc import Mapping
import hashlib
import json
from pathlib import Path
import sys
from time import perf_counter

import numpy as np
from PIL import Image
import torch
from torch.nn import functional as F
import yaml


RGB_SIZE, HSI_SIZE, HSI_BANDS = 512, 256, 224
CLASSES = ("background", "film", "basket", "cardboard", "video_tape", "filament", "bag")
COLORS = [(0, 0, 0), (63, 220, 255), (247, 155, 54), (255, 221, 63),
          (230, 95, 137), (145, 138, 255), (110, 223, 128)]
CONFIG = "ModelConfigs/Fusion/BCAF_RGB_512_HSI_5.yaml"


def load_stats(path):
    stats = yaml.safe_load(path.read_text(encoding="utf-8-sig"))["standardize"]
    mean = torch.tensor(stats["hsi_global_means"], dtype=torch.float32)
    std = torch.tensor(stats["hsi_global_stds"], dtype=torch.float32)
    if (mean.shape != (HSI_BANDS,) or std.shape != mean.shape or
            not torch.isfinite(mean).all() or not torch.isfinite(std).all() or (std < 0).any()):
        raise ValueError("HSI stats must contain 224 finite means and nonnegative standard deviations")
    return mean[:, None, None], std.masked_fill(std == 0, 1e-6)[:, None, None]


def load_model(repo, checkpoint, device):
    sys.path.insert(0, str(repo.resolve()))
    from pyfunctions.build_model import CrossAttentionFusionModel

    config = yaml.safe_load((repo / CONFIG).read_text(encoding="utf-8-sig"))
    for sensor in ("rgb", "hsi"):
        key = f"{sensor}_model_config_path"
        config[key] = str(repo / config[key])
        config[f"{sensor}_checkpoint_path"] = None
    clean = {}
    if checkpoint:
        state = torch.load(checkpoint, map_location="cpu", weights_only=True)
        for name in ("state_dict", "model_state_dict", "model"):
            if isinstance(state, Mapping) and isinstance(state.get(name), Mapping):
                state = state[name]
                break
        if not isinstance(state, Mapping):
            raise ValueError("checkpoint must contain a BCAF state dictionary")
        prefixes = ("module.", "_orig_mod.", "model.", "net.", "student.", "ema_model.", "unwrapped_model.")
        for key, value in state.items():
            while prefix := next((p for p in prefixes if key.startswith(p)), None):
                key = key.removeprefix(prefix)
            if key in clean:
                raise ValueError(f"duplicate parameter after normalizing checkpoint keys: {key}")
            clean[key] = value
    torch.manual_seed(0)
    model = CrossAttentionFusionModel(config, len(CLASSES), torch.device("cpu"))
    if checkpoint:
        model.load_state_dict(clean, strict=True)
    return model.to(device).eval(), config


def resize_chw(image, size):
    return F.interpolate(image[None], size=size, mode="bilinear", align_corners=False, antialias=True)[0]


def normalize_rgb(rgb):
    return ((rgb - torch.tensor([.485, .456, .406])[:, None, None]) /
            torch.tensor([.229, .224, .225])[:, None, None])


class SimulatorCapture:
    def __init__(self, run):
        report = json.loads((run / "report.json").read_text(encoding="utf-8"))
        self.meta = meta = report["meta"]
        if (not report["summary"]["passed"] or meta.get("bcafCapture") != 1 or
                meta["layout"] != "production" or meta["hsiBands"] != HSI_BANDS):
            raise ValueError("expected a passing production run created with --export-bcaf")
        count = report["summary"]["expected"]
        rgb_file = run / "bcaf-rgb.u8"
        hsi_file = run / "bcaf-hsi.f32le"
        hsi_count = report["summary"]["independentHsi"]
        record_size = 1 + meta["hsiWidth"] * HSI_BANDS
        if (rgb_file.stat().st_size != count * meta["rgbWidth"] * 3 or
                hsi_count < 2 or hsi_file.stat().st_size != 4 * hsi_count * record_size):
            raise ValueError("BCAF capture files have incomplete rows")
        self.rgb = np.memmap(rgb_file, mode="r", dtype=np.uint8, shape=(count, meta["rgbWidth"], 3))
        self.hsi = np.memmap(hsi_file, mode="r", dtype="<f4", shape=(hsi_count, record_size))
        self.travel = np.array(self.hsi[:, 0])
        if not np.isfinite(self.travel).all() or not (np.diff(self.travel) > 0).all():
            raise ValueError("HSI position records must be finite and increasing")
        self.windows = [event["data"] for event in report["events"] if event["type"] == "m03-window"
                        and event["data"]["last"] - event["data"]["first"] + 1 == meta["m03Rows"]]

    def prepare(self, first, last, stats):
        meta = self.meta
        pitch = meta["pitch"]
        begin, end = (first - 0.5) * pitch, (last + 0.5) * pitch
        hsi_height = 2 * round((end - begin) * HSI_SIZE / meta["beltWidth"] / 2)
        if not 0 < hsi_height <= HSI_SIZE:
            raise ValueError("the production ROI must be wider across the belt than along it")
        rgb_height = 2 * hsi_height
        rgb_top, hsi_top = (RGB_SIZE - rgb_height) // 2, (HSI_SIZE - hsi_height) // 2
        target = begin + (np.arange(hsi_height) + 0.5) * (end - begin) / hsi_height
        upper = np.searchsorted(self.travel, target)
        lower = np.clip(upper - 1, 0, len(self.travel) - 1)
        upper = np.clip(upper, 0, len(self.travel) - 1)
        step = self.travel[upper] - self.travel[lower]
        hsi_pitch = meta["hsiPitch"]
        if (np.any(step > hsi_pitch + 1e-4) or target[0] < self.travel[0] - hsi_pitch or
                target[-1] > self.travel[-1] + hsi_pitch):
            raise ValueError("HSI observations do not cover this window")
        # At the start/end of capture, extend the closest line by at most one nominal HSI pitch.
        weight = np.divide(target - self.travel[lower], step, out=np.zeros(hsi_height), where=step > 0)
        left, right = int(lower[0]), int(upper[-1]) + 1
        spectra = np.array(self.hsi[left:right, 1:]).reshape(-1, meta["hsiWidth"], HSI_BANDS)
        if not np.isfinite(spectra).all():
            raise ValueError("HSI observations contain invalid spectral samples")
        hsi = resize_chw(torch.from_numpy(spectra).permute(2, 0, 1), (right - left, HSI_SIZE))
        hsi = torch.lerp(hsi[:, lower - left], hsi[:, upper - left],
                         torch.from_numpy(weight.astype(np.float32))[None, :, None])
        if stats:
            mean, std = stats
            hsi = torch.where(hsi != 0, (hsi - mean) / std, hsi)
        hsi = F.pad(hsi, (0, 0, hsi_top, hsi_top, 0, 1))
        rgb = torch.from_numpy(np.array(self.rgb[first - 1:last])).permute(2, 0, 1).float() / 255
        rgb = resize_chw(rgb, (rgb_height, RGB_SIZE))
        inputs = {"rgb": F.pad(normalize_rgb(rgb), (0, 0, rgb_top, rgb_top)), "hsi": hsi}
        geometry = {
            "rgbValidRows": [rgb_top, rgb_top + rgb_height],
            "hsiValidRows": [hsi_top, hsi_top + hsi_height],
            "beltXEdgesMm": [meta["scanX"] - begin, meta["scanX"] - end],
            "beltYEdgesMm": [0, meta["beltWidth"]],
            "hsiSourceRows": right - left,
            "hsiEdgeExtendedRows": int(np.count_nonzero((target < self.travel[0]) | (target > self.travel[-1]))),
        }
        return inputs, rgb, slice(rgb_top, rgb_top + rgb_height), geometry


def prepare_spectralwaste(npz_file, config, stats):
    from pyfunctions.augmentations import Augmentation

    with np.load(npz_file, allow_pickle=False) as data:
        rgb, hsi = (torch.from_numpy(np.asarray(data[key], dtype=np.float32)) for key in ("rgb", "hsi"))
    if rgb.ndim != 3 or hsi.ndim != 3 or rgb.shape[0] != 3 or hsi.shape[0] != HSI_BANDS:
        raise ValueError("SpectralWaste NPZ must contain CHW RGB and 224-band CHW HSI")
    mean, std = stats
    transform = Augmentation(config=config["augmentation"], is_train=False, data_modality_to_load="rgb_hsi",
                             hsi_stats_dict={"standardize": {"hsi_global_means": mean.flatten().tolist(),
                                                             "hsi_global_stds": std.flatten().tolist()}})
    inputs = transform({"image": {"rgb": rgb, "hsi": hsi}})
    rgb = rgb / 255 if rgb.max() > 1 else rgb
    return inputs, resize_chw(rgb, (RGB_SIZE, RGB_SIZE)), slice(None), {}


@torch.inference_mode()
def predict(model, device, inputs, valid_rows):
    expected = {"rgb": (3, RGB_SIZE, RGB_SIZE), "hsi": (HSI_BANDS + 1, HSI_SIZE, HSI_SIZE)}
    for key, shape in expected.items():
        if inputs[key].shape != shape or not torch.isfinite(inputs[key]).all():
            raise ValueError(f"{key} must be finite with CHW shape {shape}")
    batch = {key: tensor[None].to(device) for key, tensor in inputs.items()}
    if device.type == "cuda":
        torch.cuda.synchronize(device)
    start = perf_counter()
    logits = model(batch)
    if device.type == "cuda":
        torch.cuda.synchronize(device)
    elapsed = perf_counter() - start
    if logits.shape != (1, len(CLASSES), RGB_SIZE, RGB_SIZE) or not torch.isfinite(logits).all():
        raise ValueError("BCAF returned invalid segmentation logits")
    mask = logits[0, :, valid_rows, :].argmax(dim=0).to(torch.uint8).cpu().numpy()
    return mask, round(elapsed * 1000, 2)


def save_images(output, name, rgb, mask):
    pixels = (rgb.permute(1, 2, 0).clamp(0, 1).numpy() * 255).round().astype(np.uint8)
    mask_image = Image.fromarray(mask).convert("P")
    mask_image.putpalette(np.array(COLORS, dtype=np.uint8).reshape(-1).tolist())
    mask_image.save(output / f"{name}-mask.png")
    overlay = Image.blend(Image.fromarray(pixels), mask_image.convert("RGB"), 0.55)
    overlay = Image.composite(overlay, Image.fromarray(pixels), Image.fromarray((mask != 0).astype(np.uint8) * 255))
    overlay.save(output / f"{name}-overlay.png")
    counts = np.bincount(mask.flatten(), minlength=len(CLASSES))
    return {label: int(counts[i]) for i, label in enumerate(CLASSES)}


def run(args):
    capture = SimulatorCapture(args.run) if args.run else None
    stats = load_stats(args.hsi_stats) if args.hsi_stats else None
    device = torch.device(args.device)
    torch.set_num_threads(args.threads)
    model, config = load_model(args.bcaf_repo, args.checkpoint, device)
    digest = None
    if args.checkpoint:
        with args.checkpoint.open("rb") as f:
            digest = hashlib.file_digest(f, "sha256").hexdigest()

    args.output.mkdir(parents=True, exist_ok=False)
    source = args.run if capture else args.npz
    report = {
        "input": str(source), "backend": "BCAF-RGB512-HSI5", "device": str(device),
        "weightMode": "untrained_smoke_test" if args.smoke_random else "checkpoint",
        "checkpointSha256": digest, "classIds": list(CLASSES),
        "hsiNormalization": str(args.hsi_stats) if stats else "simulated_reflectance_without_training_stats",
        "inputShapesNchw": {"rgb": [1, 3, 512, 512], "hsi": [1, 225, 256, 256]},
        "logitsShapeNchw": [1, len(CLASSES), 512, 512],
        "windows": [],
    }
    if capture:
        items = ((f"{w['first']:04d}-{w['last']:04d}", w) for w in capture.windows)
    else:
        items = [("spectralwaste", {})]
    completed = 0
    for name, window in items:
        result = {"id": name}
        report["windows"].append(result)
        if capture:
            result.update({key: window[key] for key in ("first", "last", "generation", "readyMs")})
            if window["state"] not in ("completed", "no_detection"):
                result.update(status="skipped", reason=window["reason"] or window["state"])
                continue
            try:
                inputs, rgb, valid, geometry = capture.prepare(window["first"], window["last"], stats)
            except ValueError as error:
                result.update(status="skipped", reason=str(error))
                continue
        else:
            inputs, rgb, valid, geometry = prepare_spectralwaste(args.npz, config, stats)
        mask, elapsed = predict(model, device, inputs, valid)
        counts = save_images(args.output, name, rgb, mask)
        result.update(status="completed", forwardMs=elapsed, geometry=geometry, classPixelCounts=counts,
                      mask=f"{name}-mask.png", overlay=f"{name}-overlay.png")
        print(f"{name}: {elapsed:.2f} ms", flush=True)
        completed += 1
        if args.limit == completed:
            break
    destination = args.output / "bcaf-report.json"
    destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not completed:
        raise ValueError(f"no usable inference window; see {destination}")
    print(f"{completed} BCAF forward passes ({report['weightMode']}); {destination}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--run", type=Path, help="simulation run made with --export-bcaf")
    source.add_argument("--npz", type=Path, help="one RGB/HSI pair from BCAF's SpectralWaste preprocessing")
    weights = parser.add_mutually_exclusive_group(required=True)
    weights.add_argument("--checkpoint", type=Path, help="complete BCAF RGB-512/HSI-5 weights")
    weights.add_argument("--smoke-random", action="store_true", help="check the graph using untrained weights")
    stats = parser.add_mutually_exclusive_group()
    stats.add_argument("--hsi-stats", type=Path, help="HSI training statistics in the same units as the source")
    stats.add_argument("--sim-reflectance", action="store_true", help="use the synthetic reflectance directly")
    parser.add_argument("--bcaf-repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="new directory for predictions")
    parser.add_argument("--device", default="cpu", help="cpu or cuda:0")
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--limit", type=int, help="maximum number of completed windows")
    args = parser.parse_args()
    if args.npz and not args.hsi_stats:
        parser.error("--npz requires --hsi-stats")
    if args.run and args.checkpoint and not (args.hsi_stats or args.sim_reflectance):
        parser.error("choose --sim-reflectance or provide compatible --hsi-stats")
    if args.threads < 1 or (args.limit is not None and args.limit < 1):
        parser.error("--threads and --limit must be positive")
    try:
        run(args)
    except (OSError, ValueError, RuntimeError, KeyError) as error:
        parser.exit(2, f"BCAF: {error}\n")


if __name__ == "__main__":
    main()

"""Offline CPU model comparison; does not download assets or modify game files.

Requires numpy, Pillow, opencv-python-headless and onnxruntime. Supply a JSON list
of {name, path, kind: yolo|pphumanseg|selfie|modnet, size?, source?, license?}.
No ground-truth masks are assumed: coverage and images are diagnostics, not IoU.
"""
import argparse
import hashlib
import json
import os
import platform
import time
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort
from PIL import Image, ImageDraw


def prepare(rgb, kind, size, keep_aspect=False):
    h, w = rgb.shape[:2]
    if kind == "yolo":
        scale = size / max(h, w)
        nw, nh = round(w * scale), round(h * scale)
        ox, oy = (size - nw) // 2, (size - nh) // 2
        img = np.full((size, size, 3), 114, np.uint8)
        img[oy:oy + nh, ox:ox + nw] = cv2.resize(rgb, (nw, nh))
        crop = (ox / size, oy / size, nw / size, nh / size)
    else:
        # Use each published model's input recipe, not YOLO's letterbox by default.
        target = ((int(w/min(h,w)*size)//32)*32, (int(h/min(h,w)*size)//32)*32) if keep_aspect else (size,size)
        img = cv2.resize(rgb, target, interpolation=cv2.INTER_AREA if keep_aspect else cv2.INTER_LINEAR)
        crop = (0, 0, 1, 1)
    img = img.astype(np.float32) / 255
    if kind in ("pphumanseg", "modnet"):
        img = (img - .5) / .5
    if kind != "selfie":
        img = img.transpose(2, 0, 1)
    return np.ascontiguousarray(img[None]), crop


def decode_yolo(outputs):
    d, proto = outputs[0][0], outputs[1][0]
    candidates = []
    for i in np.flatnonzero((d[4] >= .35) & (d[4] <= 1)):
        if np.any(d[5:84, i] > d[4, i]):
            continue
        x, y, w, h = d[:4, i]
        if not np.all(np.isfinite([x, y, w, h])) or w <= 0 or h <= 0:
            continue
        box = np.clip(np.array([x-w/2, y-h/2, x+w/2, y+h/2]) / 4, 0, 160)
        candidates.append((float(d[4, i]), int(i), box))
    candidates.sort(key=lambda v: v[0], reverse=True)
    selected = []
    for score, i, box in candidates[:128]:
        reject = False
        for _, _, b in selected:
            overlap = np.maximum(0, np.minimum(box[2:], b[2:]) - np.maximum(box[:2], b[:2])).prod()
            union = (box[2:] - box[:2]).prod() + (b[2:] - b[:2]).prod() - overlap
            if overlap / max(1e-6, union) > .45:
                reject = True
                break
        if not reject:
            selected.append((score, i, box))
            if len(selected) == 16:
                break
    mask = np.zeros((160, 160), np.float32)
    for _, i, b in selected:
        x0, y0 = np.floor(b[:2]).astype(int)
        x1, y1 = np.ceil(b[2:]).astype(int)
        # No BLAS pool: the same channel-major accumulation as the C++ worker.
        region = np.zeros((y1-y0, x1-x0), np.float32)
        for c in range(32):
            region += d[84+c, i] * proto[c, y0:y1, x0:x1]
        value = 1 / (1 + np.exp(-np.clip(region, -20, 20)))
        mask[y0:y1, x0:x1] = np.maximum(mask[y0:y1, x0:x1], value)
    return mask, len(selected)


def decode(outputs, kind, crop):
    count = None
    if kind == "yolo":
        mask, count = decode_yolo(outputs)
    elif kind == "pphumanseg":
        mask = outputs[0][0, 1]
    else:
        mask = outputs[0].squeeze()
    if not np.all(np.isfinite(mask)):
        raise ValueError("nonfinite model output")
    x, y, w, h = crop
    mh, mw = mask.shape
    mask = mask[round(y*mh):round((y+h)*mh), round(x*mw):round((x+w)*mw)]
    # Host uses a 160-square, full-frame warped mask. Apply its probability ramp.
    mask = cv2.resize(mask, (160, 160), interpolation=cv2.INTER_LINEAR)
    t = np.clip((mask-.2)/.6, 0, 1)
    return t*t*(3-2*t), count


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--models", type=Path, required=True)
    ap.add_argument("--images", nargs="+", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--threads", nargs="+", type=int, default=[2, 4, 8])
    ap.add_argument("--runs", type=int, default=20)
    ap.add_argument("--warmup", type=int, default=3)
    args = ap.parse_args()
    cv2.setNumThreads(1)
    args.out.mkdir(parents=True, exist_ok=True)
    configs = json.loads(args.models.read_text(encoding="utf-8-sig"))
    rows, previews = [], {p: [] for p in args.images}
    for config in configs:
        path = Path(config["path"])
        kind = config["kind"]
        size = config.get("size", {"yolo": 640, "pphumanseg": 192, "selfie": 256, "modnet": 512}[kind])
        for threads in args.threads:
            options = ort.SessionOptions()
            options.intra_op_num_threads = threads
            options.inter_op_num_threads = 1
            options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
            options.add_session_config_entry("session.intra_op.allow_spinning", "0")
            options.add_session_config_entry("session.inter_op.allow_spinning", "0")
            session = ort.InferenceSession(str(path), options, providers=["CPUExecutionProvider"])
            for image in args.images:
                rgb = np.asarray(Image.open(image).convert("RGB"))
                timings, cpu_times = [], []
                for iteration in range(args.warmup + args.runs):
                    cpu0, t0 = time.process_time(), time.perf_counter()
                    input_tensor, crop = prepare(rgb, kind, size, config.get("keep_aspect", False))
                    t1 = time.perf_counter()
                    outputs = session.run(None, {session.get_inputs()[0].name: input_tensor})
                    t2 = time.perf_counter()
                    mask, count = decode(outputs, kind, crop)
                    t3 = time.perf_counter()
                    if iteration >= args.warmup:
                        timings.append([(t1-t0)*1000, (t2-t1)*1000, (t3-t2)*1000, (t3-t0)*1000])
                        cpu_times.append((time.process_time()-cpu0)*1000)
                measurements = np.asarray(timings)
                row = {"model": config["name"], "kind": kind, "model_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                       "bytes": path.stat().st_size, "source": config.get("source"), "license": config.get("license"),
                       "image": str(image), "image_sha256": hashlib.sha256(image.read_bytes()).hexdigest(),
                       "threads": threads, "input_shape": input_tensor.shape, "output_shapes": [v.shape for v in outputs],
                       "pre_ms": float(np.median(measurements[:, 0])), "infer_ms": float(np.median(measurements[:, 1])),
                       "post_ms": float(np.median(measurements[:, 2])), "total_median_ms": float(np.median(measurements[:, 3])),
                       "total_p95_ms": float(np.percentile(measurements[:, 3], 95)), "cpu_mean_ms": float(np.mean(cpu_times)),
                       "mask_fraction": float(np.mean(mask>.5)), "detections": count}
                rows.append(row)
                print(json.dumps({k: row[k] for k in ("model", "threads", "infer_ms", "total_median_ms", "cpu_mean_ms", "mask_fraction")}), flush=True)
                if threads == args.threads[0]:
                    name = f"{image.stem}-{config['name']}"
                    Image.fromarray((mask*255).astype(np.uint8)).save(args.out / f"{name}-mask.png")
                    thumb = np.array(Image.fromarray(rgb).resize((640, 360)), dtype=np.float32)
                    alpha = cv2.resize(mask, (640, 360))[..., None] * .55
                    overlay = thumb*(1-alpha) + np.array([20, 235, 85])*alpha
                    panel = Image.new("RGB", (640, 390), (24, 24, 24))
                    panel.paste(Image.fromarray(overlay.astype(np.uint8)), (0, 30))
                    ImageDraw.Draw(panel).text((8, 8), f"{config['name']} / {row['total_median_ms']:.1f} ms / {threads} threads", fill="white")
                    panel.save(args.out / f"{name}-overlay.jpg")
                    previews[image].append(panel)
    for image, panels in previews.items():
        sheet = Image.new("RGB", (640*2, 390*((len(panels)+1)//2)))
        for i, panel in enumerate(panels):
            sheet.paste(panel, ((i%2)*640, (i//2)*390))
        sheet.save(args.out / f"{image.stem}-comparison.jpg")
    report = {"platform": platform.platform(), "cpu": platform.processor(), "logical_cpus": os.cpu_count(),
              "onnxruntime": ort.__version__, "runs": args.runs, "warmup": args.warmup, "rows": rows,
              "limits": "Offline screenshots include HUD and existing NR output. No GT, motion sequence or in-game FPS measurement. Python postprocessing timing is not a C++ decode benchmark."}
    (args.out / "results.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")


if __name__ == "__main__":
    main()

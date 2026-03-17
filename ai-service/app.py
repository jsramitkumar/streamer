
"""AI Detection Service — YOLOv8 via ultralytics.

Environment variables:
  MODEL_PATH   – path to YOLO model weights (default: yolov8n.pt)
  CONF_THRESH  – detection confidence threshold (default: 0.40)
  MAX_IMG_BYTES– maximum accepted image size in bytes (default: 5242880 = 5 MB)
  PORT         – port to listen on (default: 5001)
"""

import logging
import os
import sys

import cv2
import numpy as np
from flask import Flask, jsonify, request
from ultralytics import YOLO

logging.basicConfig(
    level=logging.INFO,
    format='{"ts": "%(asctime)s", "level": "%(levelname)s", "msg": "%(message)s"}',
    datefmt="%Y-%m-%dT%H:%M:%S",
    stream=sys.stdout,
)
logger = logging.getLogger(__name__)

# ─── Configuration ────────────────────────────────────────────────────────────
MODEL_PATH    = os.environ.get("MODEL_PATH",    "yolov8n.pt")
CONF_THRESH   = float(os.environ.get("CONF_THRESH",   "0.40"))
MAX_IMG_BYTES = int(os.environ.get("MAX_IMG_BYTES",   str(5 * 1024 * 1024)))
PORT          = int(os.environ.get("PORT",            "5001"))
# ──────────────────────────────────────────────────────────────────────────────

logger.info("Loading model: %s", MODEL_PATH)
try:
    model = YOLO(MODEL_PATH)
    # Warm up — run once so the first real request is fast
    dummy = np.zeros((64, 64, 3), dtype=np.uint8)
    model(dummy, verbose=False)
    logger.info("Model ready")
except Exception as exc:
    logger.error("Failed to load model: %s", exc)
    sys.exit(1)

CLASS_NAMES = model.names  # dict {int: str}

app = Flask(__name__)


@app.route("/healthz")
def health():
    return jsonify({"status": "ok", "model": MODEL_PATH})


@app.route("/detect", methods=["POST"])
def detect():
    raw = request.data

    # ── Input validation ──────────────────────────────────────────────────────
    if not raw:
        return jsonify({"error": "Empty body"}), 400

    if len(raw) > MAX_IMG_BYTES:
        return jsonify({"error": "Payload too large"}), 413

    # Validate JPEG magic bytes
    if len(raw) < 4 or raw[0] != 0xFF or raw[1] != 0xD8:
        return jsonify({"error": "Invalid image format — expected JPEG"}), 422
    # ─────────────────────────────────────────────────────────────────────────

    arr = np.frombuffer(raw, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        return jsonify({"error": "Could not decode image"}), 422

    try:
        results = model(img, conf=CONF_THRESH, verbose=False)
    except Exception as exc:
        logger.error("Inference error: %s", exc)
        return jsonify({"error": "Inference failed"}), 500

    out = []
    for r in results:
        for b in r.boxes:
            x1, y1, x2, y2 = b.xyxy[0].tolist()
            cls_id = int(b.cls[0])
            out.append({
                "x":      int(x1),
                "y":      int(y1),
                "w":      int(x2 - x1),
                "h":      int(y2 - y1),
                "label":  CLASS_NAMES.get(cls_id, str(cls_id)),
                "conf":   round(float(b.conf[0]), 3),
            })

    return jsonify(out)


if __name__ == "__main__":
    # Development only — use gunicorn in production (see Dockerfile)
    app.run(host="0.0.0.0", port=PORT)


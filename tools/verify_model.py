#!/usr/bin/env python3
"""
Verify that the committed risk_model_weights.h reproduces the int8 inference
implemented in RiskModel.cpp. Prints the expected six outputs for a fixed
feature vector; compare with tools/verify_model.cpp.
"""
import os
import re

import numpy as np

HEADER = os.path.join(os.path.dirname(__file__), "..", "firmware",
                      "HyperWatchCore", "src", "risk_model_weights.h")


def load():
    text = open(HEADER).read()

    def arr(name):
        m = re.search(rf"static const \w+ {name}\[(\d+)\] = \{{(.*?)\}};", text, re.S)
        return np.array([int(x) for x in re.findall(r"-?\d+", m.group(2))],
                        dtype=np.int64)

    def const(name):
        m = re.search(rf"static constexpr \w+ {name} = (-?[0-9.]+)f?;", text)
        return float(m.group(1))

    def iconst(name):
        m = re.search(rf"static constexpr \w+ {name} = (-?[0-9]+);", text)
        return int(m.group(1))

    return {
        "W1": arr("W1").reshape(16, 13),
        "B1": arr("B1"),
        "W2": arr("W2").reshape(8, 16),
        "B2": arr("B2"),
        "W3": arr("W3").reshape(6, 8),
        "B3": arr("B3"),
        "INPUT_SCALE": const("INPUT_SCALE"),
        "INPUT_ZERO_POINT": iconst("INPUT_ZERO_POINT"),
        "L1_OUT_SCALE": const("L1_OUT_SCALE"),
        "L1_OUT_ZERO_POINT": iconst("L1_OUT_ZERO_POINT"),
        "L2_OUT_SCALE": const("L2_OUT_SCALE"),
        "L2_OUT_ZERO_POINT": iconst("L2_OUT_ZERO_POINT"),
        "L1_W_SCALE": const("L1_W_SCALE"),
        "L2_W_SCALE": const("L2_W_SCALE"),
        "L3_W_SCALE": const("L3_W_SCALE"),
    }


def q(x, scale, zp):
    return np.clip(np.round(x / scale) + zp, -128, 127).astype(np.int64)


def predict(features, p):
    inp = q(np.array(features, dtype=np.float64), p["INPUT_SCALE"], p["INPUT_ZERO_POINT"])
    a1 = p["W1"] @ (inp - p["INPUT_ZERO_POINT"]) + p["B1"]
    z1 = np.maximum(a1.astype(np.float64) * (p["INPUT_SCALE"] * p["L1_W_SCALE"]), 0)
    h1 = q(z1, p["L1_OUT_SCALE"], p["L1_OUT_ZERO_POINT"])

    a2 = p["W2"] @ (h1 - p["L1_OUT_ZERO_POINT"]) + p["B2"]
    z2 = np.maximum(a2.astype(np.float64) * (p["L1_OUT_SCALE"] * p["L2_W_SCALE"]), 0)
    h2 = q(z2, p["L2_OUT_SCALE"], p["L2_OUT_ZERO_POINT"])

    a3 = p["W3"] @ (h2 - p["L2_OUT_ZERO_POINT"]) + p["B3"]
    logits = a3.astype(np.float64) * (p["L2_OUT_SCALE"] * p["L3_W_SCALE"])
    return 1.0 / (1.0 + np.exp(-logits))


if __name__ == "__main__":
    p = load()

    # Same raw input as tools/verify_model.cpp.
    hr, hrv, spo2, temp, slope = 120.0, 20.0, 92.0, 38.0, 0.2
    activity, sleep, day, gravity, rad = 80.0, 5.0, 100.0, 0.0, 1.5
    age, cvd, symptom = 55.0, 1.0, 1.0

    features = [
        (hr - 40) / 140.0,
        hrv / 120.0,
        (spo2 - 85) / 15.0,
        (temp - 34.0) / 6.0,
        (slope + 0.5) / 1.0,
        activity / 100.0,
        sleep / 12.0,
        day / 365.0,
        gravity,
        rad / 5.0,
        (age - 18) / 62.0,
        cvd,
        symptom,
    ]
    out = predict(features, p)
    names = ["cardio", "bone", "immune", "radiation", "sleep", "cardiac_event"]
    print("Python int8 prediction (cardiac_event shown as 0-100):")
    for name, value in zip(names, out):
        print(f"  {name:14s} {value * 100:7.3f}")

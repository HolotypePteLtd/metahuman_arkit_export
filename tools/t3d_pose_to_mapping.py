#!/usr/bin/env python3
"""Convert UE PoseAsset T3D (PA_MetaHuman_ARKit_Mapping) to CLI --mapping JSON.

Parses PoseFNames / per-pose CurveData / curve-name list from the T3D, then
emits the 52 ARKit target recipes as GUI-space DNA control deltas
(pose curves - Default base curves).

Only CTRL_* curves become DNA controls; other curves in the asset are RigLogic
blendshape/mesh channels driven internally by the rig.

Usage: t3d_pose_to_mapping.py <PA_MetaHuman_ARKit_Mapping.T3D> <out_mapping.json>
"""
import json
import re
import sys

# Exactly the 52 CLI target names (cli/native/targets.h), order irrelevant.
ARKIT_52 = [
    "EyeBlinkLeft", "EyeLookDownLeft", "EyeLookInLeft", "EyeLookOutLeft",
    "EyeLookUpLeft", "EyeSquintLeft", "EyeWideLeft",
    "EyeBlinkRight", "EyeLookDownRight", "EyeLookInRight", "EyeLookOutRight",
    "EyeLookUpRight", "EyeSquintRight", "EyeWideRight",
    "JawForward", "JawLeft", "JawRight", "JawOpen",
    "MouthFunnel", "MouthPucker", "MouthLeft", "MouthRight",
    "MouthSmileLeft", "MouthSmileRight", "MouthFrownLeft", "MouthFrownRight",
    "MouthDimpleLeft", "MouthDimpleRight", "MouthStretchLeft", "MouthStretchRight",
    "MouthRollLower", "MouthRollUpper", "MouthShrugLower", "MouthShrugUpper",
    "MouthPressLeft", "MouthPressRight",
    "MouthLowerDownLeft", "MouthLowerDownRight",
    "MouthUpperUpLeft", "MouthUpperUpRight",
    "BrowDownLeft", "BrowDownRight", "BrowInnerUp",
    "BrowOuterUpLeft", "BrowOuterUpRight",
    "CheekPuff", "CheekSquintLeft", "CheekSquintRight",
    "NoseSneerLeft", "NoseSneerRight", "TongueOut", "MouthClose",
]


def to_dna_name(ue_curve: str) -> str:
    # CTRL_expressions_eyeBlinkL -> CTRL_expressions.eyeBlinkL
    # Second path segment starts after the first underscore in the expression
    # name itself, i.e. after the *prefix group*: CTRL_<group>_<name>.
    head, _, rest = ue_curve.partition("_")
    return f"{head}_{rest.split('_')[0]}.{rest.split('_', 1)[1]}" if "_" in rest else ue_curve


def block(line: str, key: str) -> str:
    """Content of key=( ... ) with balanced parens, first occurrence."""
    m = re.search(re.escape(key) + r"=\(", line)
    if not m:
        raise SystemExit(f"missing {key}= in T3D")
    i, depth = m.end(), 1
    while depth:
        c = line[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        i += 1
    return line[m.end():i - 1]


def split_groups(text: str):
    """Split '(g1),(g2),...' into top-level paren group contents."""
    groups, depth, start = [], 0, None
    for j, c in enumerate(text):
        if c == "(":
            depth += 1
            if depth == 1:
                start = j
        elif c == ")":
            if depth == 1:
                groups.append(text[start + 1:j])
            depth -= 1
    return groups


def floats_of(text: str, key: str):
    m = re.search(re.escape(key) + r"=\(([\d.,\-eE ]*)\)", text)
    if not m:
        return None
    return [float(x) for x in m.group(1).split(",") if x.strip()]


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    t3d_path, out_path = sys.argv[1], sys.argv[2]

    line = None
    with open(t3d_path, encoding="utf-8", errors="replace") as f:
        for ln in f:
            if "PoseContainer=(" in ln:
                line = ln
                break
    if line is None:
        raise SystemExit("PoseContainer not found in T3D")

    pose_names = re.findall(r'"([^"]*)"', block(line, "PoseFNames"))
    curve_names = re.findall(r'Name="([^"]*)"', block(line, "Curves"))
    pose_groups = split_groups(block(line, "Poses"))
    if not (len(pose_names) == len(pose_groups)):
        raise SystemExit(f"pose count mismatch: {len(pose_names)} vs {len(pose_groups)}")

    def curve_values(group: str):
        vals = floats_of(group, "CurveData")
        if vals is None:
            raise SystemExit("pose without CurveData")
        if len(vals) != len(curve_names):
            raise SystemExit(
                f"curve count mismatch: {len(vals)} vs {len(curve_names)}")
        return vals

    poses = {name: curve_values(g) for name, g in zip(pose_names, pose_groups)}
    if "Default" not in poses:
        raise SystemExit("Default (base) pose missing")
    base = poses["Default"]

    targets = {}
    stats = []
    for name in ARKIT_52:
        if name == "MouthClose":
            # Not a pose in the PoseAsset. The UE plugin injects it at frame 70
            # (jawOpen=1 + 4x lipsTogether=1) and exports it minus the JawOpen
            # frame; cli/docs/BAKING.md documents the equivalent recipe:
            controls = {
                "CTRL_expressions.jawOpen": 1.0,
                "CTRL_expressions.mouthLipsTogetherUL": 1.0,
                "CTRL_expressions.mouthLipsTogetherUR": 1.0,
                "CTRL_expressions.mouthLipsTogetherDL": 1.0,
                "CTRL_expressions.mouthLipsTogetherDR": 1.0,
            }
            targets[name] = {
                "controls": controls,
                "subtract": {"CTRL_expressions.jawOpen": 1.0},
            }
            stats.append((name, len(controls)))
            continue
        if name not in poses:
            raise SystemExit(f"ARKit pose missing from T3D: {name}")
        controls = {}
        for idx, cname in enumerate(curve_names):
            if not cname.startswith("CTRL_"):
                continue
            delta = poses[name][idx] - base[idx]
            if abs(delta) > 1e-6:
                controls[to_dna_name(cname)] = round(delta, 6)
        if not controls:
            raise SystemExit(f"{name}: recipe empty after base subtraction")
        # Known rig-generation drift: newer PoseAssets use tongueRollDown,
        # older DNA rigs have tongueRoll with the same meaning.
        if "CTRL_expressions.tongueRollDown" in controls:
            v = controls.pop("CTRL_expressions.tongueRollDown")
            controls["CTRL_expressions.tongueRoll"] = v
        targets[name] = {"controls": controls}
        stats.append((name, len(controls)))

    missing = [n for n in ARKIT_52 if n not in targets]
    if missing:
        raise SystemExit(f"unconverted targets: {missing}")

    # CTRL_expressions.* names are RAW controls in the DNA (gui controls are
    # the CTRL_L_eye_blink.ty style; see cli/docs/BAKING.md).
    doc = {"control_space": "raw", "targets": targets}
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")

    print(f"poses parsed: {len(poses)} (incl. Default + helpers)")
    print(f"curves: {len(curve_names)} ({sum(c.startswith('CTRL_') for c in curve_names)} CTRL)")
    print(f"targets written: {len(targets)}")
    for name, n in stats:
        print(f"  {name}: {n} controls")


if __name__ == "__main__":
    main()

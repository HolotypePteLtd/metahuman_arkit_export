"""Canonical 52-dim ARKit blendshape list + randomization ranges.

These are the Apple-spec ARKit facial blendshape names, matching the curves
found in MetaHuman's `AS_MetaHuman_ARKit_Mapping` AnimSequence. Used by the
render pipeline to:
  - Randomize per-sample coefficient vectors.
  - Bake those vectors into a per-batch AnimSequence via
    MHExpressionEval.BakeRenderAnimSequence.
  - Record coefficients in per-sample JSON as the ground-truth label.

Name convention: camelCase (lowercase first letter), matching the curves
present in AS_MetaHuman_ARKit_Mapping. Verified via smoke test with
`jawOpen`.

Random range convention: ARKit coefs are in [0, 1] per spec. We bias some
channels away from full range where natural expressions don't typically
saturate (gaze direction, mouthClose). tongueOut is disabled because
FLAME's face mesh doesn't include the tongue.
"""

# (arkit_name, lo, hi) — lo is almost always 0 since ARKit is unipolar
ARKIT_CURVES = [
    # --- Eye: blink / close ---
    ("eyeBlinkLeft",         0.0, 1.0),
    ("eyeBlinkRight",        0.0, 1.0),
    ("eyeSquintLeft",        0.0, 0.8),
    ("eyeSquintRight",       0.0, 0.8),
    ("eyeWideLeft",          0.0, 0.6),
    ("eyeWideRight",         0.0, 0.6),

    # --- Eye: gaze direction (ARKit's "Look*" family) ---
    # Bias low — full-scale gaze is unnatural in held expressions.
    ("eyeLookDownLeft",      0.0, 0.4),
    ("eyeLookDownRight",     0.0, 0.4),
    ("eyeLookInLeft",        0.0, 0.4),
    ("eyeLookInRight",       0.0, 0.4),
    ("eyeLookOutLeft",       0.0, 0.4),
    ("eyeLookOutRight",      0.0, 0.4),
    ("eyeLookUpLeft",        0.0, 0.4),
    ("eyeLookUpRight",       0.0, 0.4),

    # --- Jaw ---
    ("jawForward",           0.0, 0.5),
    ("jawLeft",              0.0, 0.4),
    ("jawRight",             0.0, 0.4),
    ("jawOpen",              0.0, 1.0),

    # --- Mouth: shape ---
    ("mouthClose",           0.0, 0.5),
    ("mouthFunnel",          0.0, 0.8),
    ("mouthPucker",          0.0, 0.8),
    ("mouthLeft",            0.0, 0.4),
    ("mouthRight",           0.0, 0.4),
    ("mouthRollLower",       0.0, 0.7),
    ("mouthRollUpper",       0.0, 0.7),
    ("mouthShrugLower",      0.0, 0.8),
    ("mouthShrugUpper",      0.0, 0.8),

    # --- Mouth: smile / frown / dimple ---
    ("mouthSmileLeft",       0.0, 1.0),
    ("mouthSmileRight",      0.0, 1.0),
    ("mouthFrownLeft",       0.0, 0.8),
    ("mouthFrownRight",      0.0, 0.8),
    ("mouthDimpleLeft",      0.0, 0.7),
    ("mouthDimpleRight",     0.0, 0.7),
    ("mouthStretchLeft",     0.0, 0.7),
    ("mouthStretchRight",    0.0, 0.7),

    # --- Mouth: lip press / up-down ---
    ("mouthPressLeft",       0.0, 0.6),
    ("mouthPressRight",      0.0, 0.6),
    ("mouthLowerDownLeft",   0.0, 0.8),
    ("mouthLowerDownRight",  0.0, 0.8),
    ("mouthUpperUpLeft",     0.0, 0.8),
    ("mouthUpperUpRight",    0.0, 0.8),

    # --- Brow ---
    ("browDownLeft",         0.0, 1.0),
    ("browDownRight",        0.0, 1.0),
    ("browInnerUp",          0.0, 1.0),
    ("browOuterUpLeft",      0.0, 1.0),
    ("browOuterUpRight",     0.0, 1.0),

    # --- Cheek ---
    ("cheekPuff",            0.0, 0.8),
    ("cheekSquintLeft",      0.0, 0.8),
    ("cheekSquintRight",     0.0, 0.8),

    # --- Nose ---
    ("noseSneerLeft",        0.0, 0.8),
    ("noseSneerRight",       0.0, 0.8),

    # --- Tongue (disabled — FLAME face mesh lacks tongue geometry) ---
    ("tongueOut",            0.0, 0.0),
]

assert len(ARKIT_CURVES) == 52, \
    f"ARKIT_CURVES must be exactly 52 entries, got {len(ARKIT_CURVES)}"

# Name-only list in canonical order (stable across Python dict operations).
ARKIT_NAMES = [name for name, _, _ in ARKIT_CURVES]

# Canonical channel family groupings for component-isolated export.
# These mirror the current CTRL-based component split (blink/jaw/expr/head):
#  - blink_channels : only the four eye-closure shapes
#  - jaw_channels   : all jawFwd/Left/Right/Open (cumulative jaw motion)
#  - expr_channels  : everything except blink, jaw, head, tongue
#                     (so mouth/brow/cheek/nose/gaze + pucker etc.)
BLINK_CHANNELS = ["eyeBlinkLeft", "eyeBlinkRight",
                  "eyeSquintLeft", "eyeSquintRight"]

JAW_CHANNELS = ["jawForward", "jawLeft", "jawRight", "jawOpen"]

TONGUE_CHANNELS = ["tongueOut"]

EXPR_CHANNELS = [n for n in ARKIT_NAMES
                 if n not in (set(BLINK_CHANNELS)
                              | set(JAW_CHANNELS)
                              | set(TONGUE_CHANNELS))]

# Sanity: partition covers the full set.
assert (set(BLINK_CHANNELS) | set(JAW_CHANNELS)
        | set(EXPR_CHANNELS) | set(TONGUE_CHANNELS)) == set(ARKIT_NAMES)


def sample_random_coefs(rng, mask=None):
    """Draw a 52-dim ARKit coefficient vector, one scalar per channel, uniform
    in [lo, hi] per channel. Returns a dict {arkit_name: float}.

    If mask is provided (a container of channel names), only those channels
    are randomized; all others are set to 0.0. Useful for component-isolated
    component-export frames (e.g. blink-only = mask=BLINK_CHANNELS).
    """
    out = {}
    allowed = set(mask) if mask is not None else None
    for name, lo, hi in ARKIT_CURVES:
        if allowed is not None and name not in allowed:
            out[name] = 0.0
        elif hi <= lo:
            out[name] = 0.0       # disabled channel (tongueOut)
        else:
            out[name] = float(rng.uniform(lo, hi))
    return out


def build_unit_expression_schedule(value=1.0, names=None):
    """Return a neutral pose followed by one unit ARKit pose per channel.

    The result is a list of ``(label, coefficients)`` tuples.  ``neutral`` is
    always the first entry and has an empty coefficient dictionary.  By
    default, channels whose configured range is disabled (currently only
    ``tongueOut``) are omitted.

    ``names`` may select a subset while preserving the caller's order.  This
    is useful for small image-editing pilots without changing the canonical
    52-channel ordering used elsewhere.
    """
    value = float(value)
    if not 0.0 < value <= 1.0:
        raise ValueError(f"unit expression value must be in (0, 1], got {value}")

    enabled = {name for name, lo, hi in ARKIT_CURVES if hi > lo}
    if names is None:
        selected = [name for name in ARKIT_NAMES if name in enabled]
    else:
        selected = list(names)
        if not selected:
            raise ValueError("unit expression schedule needs at least one channel")
        unknown = [name for name in selected if name not in ARKIT_NAMES]
        if unknown:
            raise ValueError(f"unknown ARKit channel(s): {unknown}")
        disabled = [name for name in selected if name not in enabled]
        if disabled:
            raise ValueError(f"disabled ARKit channel(s): {disabled}")
        duplicates = sorted({name for name in selected if selected.count(name) > 1})
        if duplicates:
            raise ValueError(f"duplicate ARKit channel(s): {duplicates}")

    schedule = [("neutral", {})]
    schedule.extend((name, {name: value}) for name in selected)
    return schedule


def coefs_to_flat(per_frame_coefs):
    """Convert a list of per-frame coef dicts into the flat row-major array
    expected by BakeRenderAnimSequence.

    per_frame_coefs: list of length NumFrames, each a dict {arkit_name: float}
                     (missing names default to 0)
    returns: (names_list, flat_values_list)
             names_list is ARKIT_NAMES; flat_values_list has
             NumFrames * len(ARKIT_NAMES) floats, row-major.
    """
    names = ARKIT_NAMES
    flat = []
    for coefs in per_frame_coefs:
        for n in names:
            flat.append(float(coefs.get(n, 0.0)))
    return names, flat


# ----------------------------------------------------------------------------
# ARKit → MetaHuman Face ControlRig translation
# ----------------------------------------------------------------------------
# Copied from old/parse_dna.py's build_arkit_to_gui_map ARKIT_MAP. That module
# is the authoritative source for MH's ARKit → GUI-control mapping (was used
# to drive the offline RigLogic evaluator that produced arkit GLB exports).
# The .ty/.tx suffixes here refer to DNA GUI control channels; Sequencer's
# ControlRig track exposes the same controls with .Y/.X channels, so we
# normalise below.
_ARKIT_TO_CTRL_RAW = {
    "eyeBlinkLeft":        [("CTRL_L_eye_blink.ty", 1.0)],
    "eyeBlinkRight":       [("CTRL_R_eye_blink.ty", 1.0)],
    "eyeSquintLeft":       [("CTRL_L_eye_squintInner.ty", 1.0)],
    "eyeSquintRight":      [("CTRL_R_eye_squintInner.ty", 1.0)],
    "eyeWideLeft":         [("CTRL_L_eye_eyelidU.ty", -1.0)],
    "eyeWideRight":        [("CTRL_R_eye_eyelidU.ty", -1.0)],
    "eyeLookDownLeft":     [("CTRL_L_eye.ty", -1.0)],
    "eyeLookDownRight":    [("CTRL_R_eye.ty", -1.0)],
    "eyeLookUpLeft":       [("CTRL_L_eye.ty", 1.0)],
    "eyeLookUpRight":      [("CTRL_R_eye.ty", 1.0)],
    "eyeLookInLeft":       [("CTRL_L_eye.tx", 1.0)],
    "eyeLookInRight":      [("CTRL_R_eye.tx", -1.0)],
    "eyeLookOutLeft":      [("CTRL_L_eye.tx", -1.0)],
    "eyeLookOutRight":     [("CTRL_R_eye.tx", 1.0)],
    "jawOpen":             [("CTRL_C_jaw.ty", 1.0)],
    "jawForward":          [("CTRL_C_jaw_fwdBack.ty", 1.0)],
    "jawLeft":             [("CTRL_C_jaw.tx", 1.0)],
    "jawRight":            [("CTRL_C_jaw.tx", -1.0)],
    "mouthSmileLeft":      [("CTRL_L_mouth_cornerPull.ty", 1.0)],
    "mouthSmileRight":     [("CTRL_R_mouth_cornerPull.ty", 1.0)],
    "mouthFrownLeft":      [("CTRL_L_mouth_cornerDepress.ty", 1.0)],
    "mouthFrownRight":     [("CTRL_R_mouth_cornerDepress.ty", 1.0)],
    "mouthDimpleLeft":     [("CTRL_L_mouth_dimple.ty", 1.0)],
    "mouthDimpleRight":    [("CTRL_R_mouth_dimple.ty", 1.0)],
    "mouthStretchLeft":    [("CTRL_L_mouth_stretch.ty", 1.0)],
    "mouthStretchRight":   [("CTRL_R_mouth_stretch.ty", 1.0)],
    "mouthLeft":           [("CTRL_C_mouth.tx", 1.0)],
    "mouthRight":          [("CTRL_C_mouth.tx", -1.0)],
    "mouthClose":          [("CTRL_L_mouth_lipsTogetherD.ty", 1.0),
                            ("CTRL_R_mouth_lipsTogetherD.ty", 1.0),
                            ("CTRL_L_mouth_lipsTogetherU.ty", 1.0),
                            ("CTRL_R_mouth_lipsTogetherU.ty", 1.0)],
    "mouthFunnel":         [("CTRL_L_mouth_funnelD.ty", 1.0),
                            ("CTRL_R_mouth_funnelD.ty", 1.0),
                            ("CTRL_L_mouth_funnelU.ty", 1.0),
                            ("CTRL_R_mouth_funnelU.ty", 1.0)],
    "mouthPucker":         [("CTRL_L_mouth_purseD.ty", 1.0),
                            ("CTRL_R_mouth_purseD.ty", 1.0),
                            ("CTRL_L_mouth_purseU.ty", 1.0),
                            ("CTRL_R_mouth_purseU.ty", 1.0)],
    "mouthRollLower":      [("CTRL_L_mouth_lipsRollD.ty", 1.0),
                            ("CTRL_R_mouth_lipsRollD.ty", 1.0)],
    "mouthRollUpper":      [("CTRL_L_mouth_lipsRollU.ty", 1.0),
                            ("CTRL_R_mouth_lipsRollU.ty", 1.0)],
    "mouthShrugLower":     [("CTRL_L_jaw_ChinRaiseD.ty", 1.0),
                            ("CTRL_R_jaw_ChinRaiseD.ty", 1.0)],
    "mouthShrugUpper":     [("CTRL_L_jaw_ChinRaiseU.ty", 1.0),
                            ("CTRL_R_jaw_ChinRaiseU.ty", 1.0)],
    "mouthPressLeft":      [("CTRL_L_mouth_pressD.ty", 1.0),
                            ("CTRL_L_mouth_pressU.ty", 1.0)],
    "mouthPressRight":     [("CTRL_R_mouth_pressD.ty", 1.0),
                            ("CTRL_R_mouth_pressU.ty", 1.0)],
    "mouthLowerDownLeft":  [("CTRL_L_mouth_lowerLipDepress.ty", 1.0)],
    "mouthLowerDownRight": [("CTRL_R_mouth_lowerLipDepress.ty", 1.0)],
    "mouthUpperUpLeft":    [("CTRL_L_mouth_upperLipRaise.ty", 1.0)],
    "mouthUpperUpRight":   [("CTRL_R_mouth_upperLipRaise.ty", 1.0)],
    "browDownLeft":        [("CTRL_L_brow_down.ty", 1.0)],
    "browDownRight":       [("CTRL_R_brow_down.ty", 1.0)],
    "browInnerUp":         [("CTRL_L_brow_raiseIn.ty", 1.0),
                            ("CTRL_R_brow_raiseIn.ty", 1.0),
                            ("CTRL_L_brow_lateral.ty", 1.0),
                            ("CTRL_R_brow_lateral.ty", 1.0)],
    "browOuterUpLeft":     [("CTRL_L_brow_raiseOut.ty", 1.0)],
    "browOuterUpRight":    [("CTRL_R_brow_raiseOut.ty", 1.0)],
    "cheekPuff":           [("CTRL_L_mouth_lipsBlow.ty", 1.0),
                            ("CTRL_R_mouth_lipsBlow.ty", 1.0)],
    "cheekSquintLeft":     [("CTRL_L_eye_cheekRaise.ty", 1.0)],
    "cheekSquintRight":    [("CTRL_R_eye_cheekRaise.ty", 1.0)],
    "noseSneerLeft":       [("CTRL_L_nose_wrinkleUpper.ty", 1.0)],
    "noseSneerRight":      [("CTRL_R_nose_wrinkleUpper.ty", 1.0)],
    "tongueOut":           [("CTRL_C_tongue_inOut.ty", 1.0)],
}


def _normalise_ctrl_name(dna_name):
    """DNA GUI name → Sequencer ControlRig channel name.

    DNA uses lower-case axis suffixes (.ty, .tx). Sequencer's ControlRig
    channels use upper-case (.Y, .X). Controls exposed as a single slider
    (e.g. eye blink) only have a .Y channel in Sequencer and often get
    referenced without the suffix in the existing render_metahuman.py
    CTRL list (e.g. "CTRL_L_eye_blink" rather than "CTRL_L_eye_blink.Y").
    We preserve the suffix here so the render-side code can strip it or
    not per its own convention.
    """
    if dna_name.endswith(".ty"):
        return dna_name[:-3] + ".Y"
    if dna_name.endswith(".tx"):
        return dna_name[:-3] + ".X"
    if dna_name.endswith(".tz"):
        return dna_name[:-3] + ".Z"
    return dna_name


ARKIT_TO_CTRL = {
    arkit_name: [(_normalise_ctrl_name(n), w) for (n, w) in pairs]
    for arkit_name, pairs in _ARKIT_TO_CTRL_RAW.items()
}

# Sanity-check: every ARKit channel in ARKIT_NAMES has a mapping entry
_missing = [n for n in ARKIT_NAMES if n not in ARKIT_TO_CTRL]
assert not _missing, f"ARKit channels missing CTRL mapping: {_missing}"


def arkit_to_ctrl(arkit_coefs):
    """Translate an ARKit coef dict to a {ctrl_channel: value} dict.

    Multiple ARKit shapes may drive the same CTRL channel (e.g. eyeLookUp*
    and eyeLookDown* share a single .Y channel with opposite signs). Their
    contributions sum.

    Inputs
    ------
    arkit_coefs : dict mapping ARKit shape name → float in [0, 1]
                  (per ARKit spec; we don't clip here — caller's job).

    Returns
    -------
    dict mapping Sequencer CTRL channel name (e.g. "CTRL_L_eye_blink.Y")
    → float value. Keys with near-zero values are omitted.
    """
    out = {}
    for arkit_name, coef in arkit_coefs.items():
        if coef == 0.0:
            continue
        mapping = ARKIT_TO_CTRL.get(arkit_name)
        if mapping is None:
            continue
        for ctrl_name, weight in mapping:
            out[ctrl_name] = out.get(ctrl_name, 0.0) + coef * weight
    # Drop channels that ended up at zero from cancellation
    return {k: v for k, v in out.items() if abs(v) > 1e-9}

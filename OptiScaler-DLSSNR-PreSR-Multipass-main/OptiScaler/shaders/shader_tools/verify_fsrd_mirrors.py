"""Verify every hand-maintained mirror between the FSRD C++ and its HLSL.

The FSR-RR preprocessor passes data to its shaders through structures that are written twice:
once as a C++ struct the caller fills in, once as an HLSL cbuffer the shader reads, and once
again as positional resource slots the shader addresses by register number. Nothing in either
compiler can see the other side, so a field that is inserted, reordered or renumbered in one
place and not the other still builds, still runs, and silently changes which parameter a shader
reads or which texture it binds. That is the failure this checks for.

It also checks the flag words, which are the same hazard in its purest form: C++ sets a bit,
the shader tests a bit, and a value that drifts between the two compiles cleanly while changing
which branch runs.

Run it before building anything that touches these files:

    python verify_fsrd_mirrors.py

Exits non-zero and prints every mismatch it found. A parse that does not understand a line is
an error rather than a skip, because a verifier that quietly ignores what it cannot read is
worse than no verifier at all.
"""

import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
PRE = os.path.join(ROOT, "OptiScaler", "shaders", "fsrd_preprocess", "precompile")
DATA_H = os.path.join(ROOT, "OptiScaler", "shaders", "fsrd_preprocess", "FSRDShaderData.h")
PRE_H = os.path.join(ROOT, "OptiScaler", "shaders", "fsrd_preprocess", "FSRDPreprocessor_Dx12.h")
FEATURE_CPP = os.path.join(ROOT, "OptiScaler", "upscalers", "fsr31", "FSRDFeature_Dx12.cpp")

# Modes the name table lists that are features of the upscaler rather than the conversion or
# the composition, and so live in neither enum.
# Modes the upscaler side owns rather than either flag word: the name table lists them, and
# they are not conversion or composition debug modes.
OTHER_MODES = {"None", "FfxDebug", "AmbientOcclusionInput", "AmbientOcclusionOutput",
               "DenoiserOutput", "DenoiserBypass", "UpscalerBypass", "RawColor", "DlssBias",
               "DlssColorBeforeParticles", "DlssColorBeforeTransparency",
               "DlssTransparencyLayer"}

CONV_HLSL = os.path.join(PRE, "FSRDInputConv.hlsl")
COMP_HLSL = os.path.join(PRE, "FSRDOutputComp.hlsl")
FLOOR_HLSL = os.path.join(PRE, "FSRDFloor.hlsl")
SEED_HLSL = os.path.join(PRE, "FSRDFloorSeed.hlsl")
TRUST_EVIDENCE_HLSL = os.path.join(PRE, "FSRDAlbedoTrustEvidence.hlsl")
TRUST_PROPAGATE_HLSL = os.path.join(PRE, "FSRDAlbedoTrustPropagate.hlsl")
UPSCALER_DEPTH_HLSL = os.path.join(PRE, "FSRDUpscalerDepth.hlsl")
PREPROCESSOR_CPP = os.path.join(ROOT, "OptiScaler", "shaders", "fsrd_preprocess",
                                "FSRDPreprocessor_Dx12.cpp")

errors = []


def fail(msg):
    errors.append(msg)


def read(path):
    return io.open(path, encoding="utf-8", errors="replace").read().replace("\r\n", "\n")


def strip_line_comment(line):
    out = []
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if c == '"':
            in_str = not in_str
        if not in_str and line.startswith("//", i):
            break
        out.append(c)
        i += 1
    return "".join(out)


def brace_body(text, start_marker, open_char="{", close_char="}"):
    """Body of the first block whose opening brace follows start_marker."""
    at = text.find(start_marker)
    if at < 0:
        raise RuntimeError("marker not found: " + start_marker)
    at = text.find(open_char, at)
    if at < 0:
        raise RuntimeError("no opening brace after: " + start_marker)
    depth = 0
    for i in range(at, len(text)):
        if text[i] == open_char:
            depth += 1
        elif text[i] == close_char:
            depth -= 1
            if depth == 0:
                return text[at + 1:i]
    raise RuntimeError("unbalanced braces after: " + start_marker)


# ---------------------------------------------------------------- constants layout

# C++ side: DirectXMath POD members, naturally aligned inside an alignas(16) struct.
CPP_SIZES = {
    "XMFLOAT4X4": (64, 4),
    "XMFLOAT4": (16, 4),
    "XMFLOAT3": (12, 4),
    "XMFLOAT2": (8, 4),
    "XMUINT4": (16, 4),
    "XMUINT3": (12, 4),
    "XMUINT2": (8, 4),
    "XMINT2": (8, 4),
    "uint64_t": (8, 8),
    "uint32_t": (4, 4),
    "int32_t": (4, 4),
    "float": (4, 4),
    "bool": (1, 1),
}
CPP_FIELD = re.compile(
    r"^(?:(XMFLOAT4X4|XMFLOAT4|XMFLOAT3|XMFLOAT2|XMUINT4|XMUINT3|XMUINT2|XMINT2|uint64_t|uint32_t|int32_t|float|bool))\s+"
    r"(\w+)\s*(?:\[(\d+)\])?\s*(?:=\s*[^;]*)?;$")

# Shader side: constant-buffer packing. A member starts at the next offset allowed by its own
# alignment and never straddles a 16-byte boundary.
HLSL_SIZES = {
    "float4x4": (64, 16),
    "float4": (16, 16),
    "float3": (12, 4),
    "float2": (8, 8),
    "float": (4, 4),
    "uint4": (16, 16),
    "uint3": (12, 4),
    "uint2": (8, 8),
    "uint": (4, 4),
    "int4": (16, 16),
    "int3": (12, 4),
    "int2": (8, 8),
    "int": (4, 4),
    "bool": (4, 4),
}
HLSL_FIELD = re.compile(
    r"^(float4x4|float4|float3|float2|float|uint4|uint3|uint2|uint|int4|int3|int2|int|bool)\s+(\w+)\s*(?:\[(\d+)\])?;$")


def cpp_struct_fields(body, where):
    fields = []
    offset = 0
    align_of = 0
    for raw in body.split("\n"):
        line = strip_line_comment(raw).strip()
        if not line:
            continue
        m = CPP_FIELD.match(line)
        if not m:
            fail("%s: cannot parse C++ field line: %s" % (where, line))
            continue
        type_name, name, array = m.group(1), m.group(2), m.group(3)
        size, align = CPP_SIZES[type_name]
        count = int(array) if array else 1
        total = size * count
        offset = (offset + align - 1) // align * align
        fields.append((name, 0, offset, total))
        offset += total
        align_of = max(align_of, align)
    offset = (offset + 15) // 16 * 16 if offset % 16 else offset
    return fields, offset


def hlsl_cbuffer_fields(body, where):
    fields = []
    offset = 0
    additive_tail = False
    additive_guard_seen = False
    additive_member_seen = False
    for raw in body.split("\n"):
        line = strip_line_comment(raw).strip()
        if not line:
            continue
        # Conversion's enabled variant uses the original CB's last padding
        # float. Verify/serialize the complete runtime layout while the original
        # compiled variant omits the member and retains its frozen DXIL. This
        # explicitly recognizes only that guard, not arbitrary preprocessing.
        if line == "#if FSRD_ADDITIVE_SPLIT_ENABLED" and not additive_guard_seen:
            additive_tail = True
            additive_guard_seen = True
            continue
        if line == "#endif" and additive_tail:
            if not additive_member_seen:
                fail("%s: additive cbuffer guard has no AdditiveLightSplit member" % where)
            additive_tail = False
            continue
        m = HLSL_FIELD.match(line)
        if not m:
            fail("%s: cannot parse HLSL member line: %s" % (where, line))
            continue
        type_name, name, array = m.group(1), m.group(2), m.group(3)
        if additive_tail:
            if additive_member_seen or type_name != "float" or name != "AdditiveLightSplit" or array:
                fail("%s: additive cbuffer guard must contain only float AdditiveLightSplit" % where)
            additive_member_seen = True
        elif additive_guard_seen:
            fail("%s: AdditiveLightSplit guard must be the cbuffer's final member" % where)
        size, align = HLSL_SIZES[type_name]
        count = int(array) if array else 1
        for i in range(count):
            want = (offset + align - 1) // align * align
            if want % 16 and (want % 16) + size > 16:
                want = (want + 15) // 16 * 16
            offset = want
            label = name if count == 1 else "%s[%d]" % (name, i)
            fields.append((label, 0, offset, size))
            offset += size
    offset = (offset + 15) // 16 * 16 if offset % 16 else offset
    return fields, offset


CONSTANT_PAIRS = [
    ("FloorSeed", "struct alignas(16) Constants", SEED_HLSL, "cbuffer CB_Median", {}),
    ("FloorFilter", "struct alignas(16) Constants", FLOOR_HLSL, "cbuffer CB_Analysis", {}),
    ("Conversion", "struct alignas(16) Constants", CONV_HLSL, "cbuffer CB_Packing", {}),
    ("Composition", "struct alignas(16) Constants", COMP_HLSL, "cbuffer CB_Comp", {}),
    ("TrustEvidence", "struct alignas(16) Constants", TRUST_EVIDENCE_HLSL, "cbuffer CB_AlbedoTrust", {}),
    ("TrustPropagate", "struct alignas(16) Constants", TRUST_PROPAGATE_HLSL, "cbuffer CB_AlbedoTrust", {}),
    ("UpscalerDepth", "struct alignas(16) Constants", UPSCALER_DEPTH_HLSL, "cbuffer CB_UpscalerDepth", {}),
]


def check_constants():
    data = read(DATA_H)
    for ns, struct_marker, shader_path, cbuffer_marker, _ in CONSTANT_PAIRS:
        ns_at = data.find("namespace " + ns)
        if ns_at < 0:
            fail("no namespace %s in %s" % (ns, DATA_H))
            continue
        cpp_body = brace_body(data[ns_at:], struct_marker)
        cpp_fields, cpp_size = cpp_struct_fields(cpp_body, "C++ %s::Constants" % ns)
        shader = read(shader_path)
        hlsl_body = brace_body(shader, cbuffer_marker)
        hlsl_fields, hlsl_size = hlsl_cbuffer_fields(
            hlsl_body, "HLSL %s in %s" % (cbuffer_marker, os.path.basename(shader_path)))

        cpp_assert = None
        seg = data[ns_at:]
        m = re.search(r"static_assert\(sizeof\(Constants\)\s*==\s*(\d+)", seg)
        if m:
            cpp_assert = int(m.group(1))

        cpp_names = [f[0] for f in cpp_fields]
        hlsl_names = [f[0] for f in hlsl_fields]
        if cpp_names != hlsl_names:
            fail("%s::Constants member order differs from %s:\n    C++  %s\n    HLSL %s"
                 % (ns, cbuffer_marker, cpp_names, hlsl_names))
            continue
        for a, b in zip(cpp_fields, hlsl_fields):
            if a[2] != b[2]:
                fail("%s::Constants member %s is at offset %d in C++ but %d in HLSL"
                     % (ns, a[0], a[2], b[2]))
        if cpp_size != hlsl_size:
            fail("%s::Constants is %d bytes in C++ but %d in HLSL" % (ns, cpp_size, hlsl_size))
        if cpp_assert is not None and cpp_assert != cpp_size:
            fail("%s::Constants static_assert says %d but the layout computes to %d"
                 % (ns, cpp_assert, cpp_size))
        if cpp_assert is None:
            fail("%s::Constants has no sizeof static_assert" % ns)


# ---------------------------------------------------------------- flag words

CONV_FLAG_NAMES = {
    "IsDepthLinear": "FLAGS_LINEAR_DEPTH",
    "IsRoughnessPacked": "FLAGS_PACKED_ROUGHNESS",
    "RightHanded": "FLAGS_NEGATIVE_VIEW_DEPTH",
    "HasSpecHitDistance": "FLAGS_HAS_SPEC_HIT_DISTANCE",
    "SpecularSignalIndirect": "FLAGS_SPECULAR_SIGNAL_INDIRECT",
    "HasEmissiveInput": "FLAGS_HAS_EMISSIVE_INPUT",
    "FloorEnabled": "FLAGS_FLOOR_ENABLED",
    "MotionVectorsJittered": "FLAGS_MOTION_VECTORS_JITTERED",
    "DisplayResolutionMotion": "FLAGS_DISPLAY_RESOLUTION_MOTION",
    "NormalsViewSpace": "FLAGS_NORMALS_VIEW_SPACE",
    "TitleLinearDepth": "FLAGS_TITLE_LINEAR_DEPTH",
    "HasResponsivityMask": "FLAGS_HAS_RESPONSIVITY_MASK",
    "HasCombinedSpecHitDistance": "FLAGS_HAS_COMBINED_SPEC_HIT_DISTANCE",
    "HasBiasMask": "FLAGS_HAS_BIAS_MASK",
    "HalfDiffuse": "FLAGS_HALF_DIFFUSE",
    "HalfSpecular": "FLAGS_HALF_SPECULAR",
    "ApproximateSpecHitDistance": "FLAGS_APPROXIMATE_SPEC_HIT_DISTANCE",
    "ApproximateRayHitDistance": "FLAGS_APPROXIMATE_RAY_HIT_DISTANCE",
    "UnsupportedAlbedo": "FLAGS_UNSUPPORTED_ALBEDO",
    "Debug": "FLAGS_DEBUG",
    "DebugModeMask": "FLAGS_DEBUG_MODE_MASK",
}

CONV_DEBUG_NAMES = {
    "DebugOutRadiance": "FLAGS_DEBUG",
    "DebugInSpecHitDist": "FLAGS_DEBUG_IN_SPEC_HIT_DIST",
    "DebugInMotion": "FLAGS_DEBUG_IN_MOTION",
    "DebugInNormals": "FLAGS_DEBUG_IN_NORMALS",
    "DebugInRoughness": "FLAGS_DEBUG_IN_ROUGHNESS",
    "DebugInDiffAlbedo": "FLAGS_DEBUG_IN_DIFF_ALBEDO",
    "DebugInSpecAlbedo": "FLAGS_DEBUG_IN_SPEC_ALBEDO",
    "DebugOutSignalSplit": "FLAGS_DEBUG_OUT_SIGNAL_SPLIT",
    "DebugOutLinearDepth": "FLAGS_DEBUG_OUT_LINEAR_DEPTH",
    "DebugOutMotion": "FLAGS_DEBUG_OUT_MOTION",
    "DebugOutNormals": "FLAGS_DEBUG_OUT_NORMALS",
    "DebugOutSpecAlbedo": "FLAGS_DEBUG_OUT_SPEC_ALBEDO",
    "DebugOutDiffAlbedo": "FLAGS_DEBUG_OUT_DIFF_ALBEDO",
    "DebugOutDepthDelta": "FLAGS_DEBUG_OUT_DEPTH_DELTA",
    "DebugNormDepth": "FLAGS_DEBUG_NORM_DEPTH",
    "DebugAlbedoError": "FLAGS_DEBUG_ALBEDO_OVERSHOOT",
    "DebugFloorColor": "FLAGS_DEBUG_FLOOR_COLOR",
    "DebugRawIndirectSpecular": "FLAGS_DEBUG_RAW_INDIRECT_SPEC",
    "DebugEffectiveRoughness": "FLAGS_DEBUG_EFFECTIVE_ROUGHNESS",
    "DebugRawRoughness": "FLAGS_DEBUG_RAW_ROUGHNESS",
    "DebugEmissiveMask": "FLAGS_DEBUG_EMISSIVE_MASK",
    "DebugAppliedRoughness": "FLAGS_DEBUG_APPLIED_ROUGHNESS",
    "DebugResourceInspector": "FLAGS_DEBUG_RESOURCE_INSPECTOR",
    "DebugMaterialType": "FLAGS_DEBUG_MATERIAL_TYPE",
    "DebugInEmissive": "FLAGS_DEBUG_IN_EMISSIVE",
    "DebugRRMaterialType": "FLAGS_DEBUG_RR_MATERIAL_TYPE",
    "DebugAlbedoStructure": "FLAGS_DEBUG_ALBEDO_STRUCTURE",
    "DebugFloorResidual": "FLAGS_DEBUG_FLOOR_RESIDUAL",
    "DebugSpecularSplit": "FLAGS_DEBUG_SPECULAR_SPLIT",
    "DebugInTitleLinearDepth": "FLAGS_DEBUG_IN_TITLE_DEPTH",
    "DebugTitleLinearDepthDiff": "FLAGS_DEBUG_TITLE_DEPTH_DIFF",
    "DebugInResponsivityMask": "FLAGS_DEBUG_IN_RESPONSIVITY",
    "DebugInBiasMask": "FLAGS_DEBUG_IN_BIAS_MASK",
    "DebugDemodGain": "FLAGS_DEBUG_DEMOD_GAIN",
    "DebugHitDistGate": "FLAGS_DEBUG_HIT_DIST_GATE",
    "DebugDenoiserFraction": "FLAGS_DEBUG_DENOISER_FRACTION",
    "DebugFloorNoise": "FLAGS_DEBUG_FLOOR_NOISE",
    "DebugSkipUnmapped": "FLAGS_DEBUG_SKIP_UNMAPPED",
    "DebugSkipFloor": "FLAGS_DEBUG_SKIP_FLOOR",
    "DebugFloorExcess": "FLAGS_DEBUG_FLOOR_EXCESS",
    "DebugDemodRisk": "FLAGS_DEBUG_DEMOD_RISK",
}

COMP_FLAG_NAMES = {
    "RawSourceBlit": "FLAGS_RAW_SOURCE_BLIT",
    "ExtraDiffuse": "FLAGS_EXTRA_DIFFUSE",
    "ExtraSpecular": "FLAGS_EXTRA_SPECULAR",
    "DiffuseAlternate": "FLAGS_DIFFUSE_ALTERNATE",
    "DiffuseSignalDisabled": "FLAGS_DIFFUSE_SIGNAL_DISABLED",
    "SpecularSignalDisabled": "FLAGS_SPECULAR_SIGNAL_DISABLED",
    "ScaleSrc": "FLAGS_SCALE_SRC",
    "DiffuseSignalIndirect": "FLAGS_DIFFUSE_SIGNAL_INDIRECT",
    "SpecularSignalIndirect": "FLAGS_SPECULAR_SIGNAL_INDIRECT",
    "Debug": "FLAGS_DEBUG",
    "DebugModeMask": "FLAGS_DEBUG_MODE_MASK",
}

COMP_DEBUG_NAMES = {
    "DebugDetailConfidence": "FLAGS_DEBUG_DETAIL_CONFIDENCE",
    "DebugSkipSignal": "FLAGS_DEBUG_SKIP_SIGNAL",
    "DebugDenoiserOutput": "FLAGS_DEBUG_DENOISER_OUTPUT",
    "DebugDirectSpecular": "FLAGS_DEBUG_DIRECT_SPECULAR",
    "DebugDirectDiffuse": "FLAGS_DEBUG_DIRECT_DIFFUSE",
    "DebugIndirectDiffuse": "FLAGS_DEBUG_INDIRECT_DIFFUSE",
    "DebugDetailCorrection": "FLAGS_DEBUG_DETAIL_CORRECTION",
    "DebugDetailReference": "FLAGS_DEBUG_DETAIL_REFERENCE",
    "DebugReconstructedColor": "FLAGS_DEBUG_RECONSTRUCTED_COLOR",
    "DebugDetailSeed": "FLAGS_DEBUG_DETAIL_SEED",
    "DebugDetailAnchored": "FLAGS_DEBUG_DETAIL_ANCHORED",
    "DebugHandoverWeights": "FLAGS_DEBUG_HANDOVER_WEIGHTS",
    "DebugHandoverLimits": "FLAGS_DEBUG_HANDOVER_LIMITS",
    "DebugCompositionBeforeClamp": "FLAGS_DEBUG_COMPOSITION_BEFORE_CLAMP",
    "DebugCompositionFinal": "FLAGS_DEBUG_COMPOSITION_FINAL",
    "DebugDetailBoxAnchored": "FLAGS_DEBUG_DETAIL_BOX_ANCHORED",
    "DebugHandoverEligibility": "FLAGS_DEBUG_HANDOVER_ELIGIBILITY",
    "DebugChromaRecovery": "FLAGS_DEBUG_CHROMA_RECOVERY",
    "DebugLumaRecovery": "FLAGS_DEBUG_LUMA_RECOVERY",
    "DebugIndirectSpecular": "FLAGS_DEBUG_INDIRECT_SPECULAR",
    "DebugAlbedoTrust": "FLAGS_DEBUG_ALBEDO_TRUST",
}

MAX_DEBUG_MODE = 0x7F
COMP_FLAGS_RESOLVED_BY_BINDING = set()


def cpp_enum_values(text, enum_name):
    body = brace_body(text, "enum class " + enum_name)
    values = {}
    for raw in body.split("\n"):
        line = strip_line_comment(raw).strip().rstrip(",")
        if not line:
            continue
        m = re.match(r"^(\w+)\s*=\s*(.+)$", line)
        if not m:
            fail("enum %s: cannot parse entry: %s" % (enum_name, line))
            continue
        values[m.group(1)] = m.group(2).strip()
    return values


def hlsl_defines(text):
    values = {}
    for m in re.finditer(r"#define\s+(FLAGS_\w+)\s+\((.+?)\)\s*$", text, re.M):
        values[m.group(1)] = m.group(2).strip()
    return values


def evaluate(expr, known, where):
    expr = expr.strip()
    if expr in known:
        return known[expr]
    try:
        py = re.sub(r"(?<=\d)[uU]", "", expr)
        for name in set(re.findall(r"[A-Za-z_]\w*", py)):
            if name in known:
                py = re.sub(r"\b%s\b" % name, "(%d)" % known[name], py)
            elif name in ("int", "uint", "float"):
                py = re.sub(r"\b%s\b" % name, "", py)
        return int(eval(py, {"__builtins__": {}}, {}))
    except Exception as exc:
        fail("%s: cannot evaluate %r (%s)" % (where, expr, exc))
        return None


def check_flag_list(cpp_values, hlsl_values, name_map, label, is_mode, debug_bit=None,
                    resolved_by_binding=frozenset(), requires_debug=True):
    resolved = {name: None for name in cpp_values}
    if not is_mode:
        for _ in range(3):
            for name in cpp_values:
                if resolved.get(name) is not None:
                    continue
                known = {k: v for k, v in resolved.items() if v is not None}
                resolved[name] = evaluate(cpp_values[name], known, "C++ %s::%s" % (label, name))
        debug_bit = resolved.get("Debug")
        if debug_bit is None and requires_debug:
            fail("%s: no Debug entry" % label)

    mapped_cpp = set()
    for cpp_name, shader_name in name_map.items():
        if cpp_name not in cpp_values:
            fail("%s: name map lists %s but the C++ enum has no such entry" % (label, cpp_name))
            continue
        if shader_name not in hlsl_values:
            fail("%s: name map lists %s but the shader defines no %s" % (label, cpp_name, shader_name))
            continue
        mapped_cpp.add(cpp_name)
        cpp_value = resolved.get(cpp_name)
        if is_mode:
            full = evaluate(cpp_values[cpp_name], {"Debug": debug_bit},
                            "%s::%s" % (label, cpp_name))
            if full is None:
                continue
            if (full & ((1 << 17) - 1)) != debug_bit:
                fail("%s: %s is not composed as (mode << 17 | Debug)" % (label, cpp_name))
                continue
            mode = full >> 17
            if mode > MAX_DEBUG_MODE:
                fail("%s: mode number %d for %s exceeds the %d the mask can hold"
                     % (label, mode, cpp_name, MAX_DEBUG_MODE))
            cpp_value = full
            shader_value = evaluate(hlsl_values[shader_name], {"FLAGS_DEBUG": debug_bit},
                                    "HLSL %s" % shader_name)
        else:
            shader_value = evaluate(hlsl_values[shader_name], {}, "HLSL %s" % shader_name)
        if cpp_value is None or shader_value is None:
            continue
        if cpp_value != shader_value:
            fail("%s: %s is 0x%X in C++ but %s is 0x%X in the shader"
                 % (label, cpp_name, cpp_value, shader_name, shader_value))

    for cpp_name in cpp_values:
        if cpp_name in resolved_by_binding:
            continue
        if cpp_name not in mapped_cpp:
            fail("%s: C++ entry %s is not in the verifier's name map, so nothing ties it to the "
                 "shader. Add it to the map." % (label, cpp_name))

    if not is_mode and debug_bit is not None:
        for cpp_name, value in resolved.items():
            if value is not None and cpp_name not in ("Debug", "DebugModeMask") and value & (0xFF << 16):
                fail("%s: %s overlaps the debug mode field "
                     "(mode << 17, masks 0xFF << 16). A debug mode would set it by itself."
                     % (label, cpp_name))
    return resolved


def check_flags():
    pre = read(PRE_H)
    seed = cpp_enum_values(brace_body(read(DATA_H), "namespace FloorSeed"), "Flags")
    seed.pop("None", None)
    check_flag_list(seed, hlsl_defines(read(SEED_HLSL)), {
        "LinearDepth": "FLAGS_LINEAR_DEPTH", "NegativeViewDepth": "FLAGS_NEGATIVE_VIEW_DEPTH",
        "TitleLinearDepth": "FLAGS_TITLE_LINEAR_DEPTH"}, "FloorSeed flags", False, requires_debug=False)
    conv_cpp = cpp_enum_values(pre, "ConvFlags")
    comp_cpp = cpp_enum_values(pre, "CompFlags")
    conv_hlsl = hlsl_defines(read(CONV_HLSL))
    comp_hlsl = hlsl_defines(read(COMP_HLSL))

    def split(cpp_values, mode_names):
        plain = {k: v for k, v in cpp_values.items() if k not in mode_names and k != "None"}
        modes = {k: v for k, v in cpp_values.items() if k in mode_names}
        return plain, modes

    conv_plain, conv_modes = split(conv_cpp, CONV_DEBUG_NAMES)
    comp_plain, comp_modes = split(comp_cpp, COMP_DEBUG_NAMES)
    conv_resolved = check_flag_list(conv_plain, conv_hlsl, CONV_FLAG_NAMES, "ConvFlags", False)
    comp_resolved = check_flag_list(comp_plain, comp_hlsl, COMP_FLAG_NAMES, "CompFlags", False,
                                    resolved_by_binding=COMP_FLAGS_RESOLVED_BY_BINDING)
    check_flag_list(conv_modes, conv_hlsl, CONV_DEBUG_NAMES, "ConvFlags debug", True,
                    conv_resolved.get("Debug"))
    check_flag_list(comp_modes, comp_hlsl, COMP_DEBUG_NAMES, "CompFlags debug", True,
                    comp_resolved.get("Debug"))

    # Canonical SR depth flags
    depth_cpp = cpp_enum_values(brace_body(read(DATA_H), "namespace UpscalerDepth"), "Flags")
    depth_cpp.pop("None", None)
    check_flag_list(depth_cpp, hlsl_defines(read(UPSCALER_DEPTH_HLSL)), {
        "InfiniteFar": "FLAGS_INFINITE_FAR", "InvertedOutput": "FLAGS_INVERTED_OUTPUT"
    }, "UpscalerDepth flags", False, requires_debug=False)


# ---------------------------------------------------------------- resource order

TEXTURE_DECL = re.compile(
    r"^\s*(?:Texture2D|RWTexture2D)<[^>]+>\s+(\w+)\s*:\s*register\((\w)(\d+)\)\s*;")
RESOURCE_PTR = re.compile(r"^\s*ID3D12Resource\*\s+(\w+)\s*;\s*(?://.*)?$")


def cpp_resource_order(text, marker):
    body = brace_body(text, marker)
    body = brace_body(body, "struct Data")
    names = []
    for raw in body.split("\n"):
        line = strip_line_comment(raw).strip()
        if not line:
            continue
        m = RESOURCE_PTR.match(line)
        if not m:
            fail("%s: cannot parse C++ resource line: %s" % (marker, line))
            continue
        names.append(m.group(1))
    return names


def shader_resource_order(text, kind, where):
    names = []
    for raw in text.split("\n"):
        line = strip_line_comment(raw).strip()
        m = TEXTURE_DECL.match(raw)
        if not m:
            continue
        register, index = m.group(2), int(m.group(3))
        if register != kind:
            continue
        names.append((index, m.group(1)))
    names.sort()
    for expected, (index, name) in enumerate(names):
        if index != expected:
            fail("%s: %s registers are not contiguous: expected t%d, found t%d (%s)"
                 % (where, kind, expected, index, name))
    return [name for _, name in names]


def check_resources():
    data = read(DATA_H)
    for namespace, path in (("FloorSeed", SEED_HLSL), ("FloorFilter", FLOOR_HLSL),
                            ("Conversion", CONV_HLSL), ("Composition", COMP_HLSL),
                            ("TrustEvidence", TRUST_EVIDENCE_HLSL),
                            ("TrustPropagate", TRUST_PROPAGATE_HLSL),
                            ("UpscalerDepth", UPSCALER_DEPTH_HLSL)):
        body = brace_body(data, "namespace " + namespace)
        shader = read(path)
        for kind, union in (("t", "Input"), ("u", "Output")):
            actual = shader_resource_order(shader, kind, namespace)
            root_kind = "SRV" if kind == "t" else "UAV"
            if namespace == "Composition" and kind == "t":
                # Join only the MainRS string literals, not shader comments or
                # declarations. Preserve the 17-entry table and require that
                # only t9/t10 permit pre-execute descriptor finalization.
                signature_lines = []
                collecting = False
                for line in shader.splitlines():
                    if line.startswith("#define MainRS"):
                        collecting = True
                    if collecting:
                        signature_lines.extend(re.findall(r'"([^"\\]*)"', line))
                        if not line.rstrip().endswith("\\"):
                            break
                signature = "".join(signature_lines)
                history_table = (
                    r'DescriptorTable\(\s*SRV\(t0,\s*numDescriptors\s*=\s*9\),\s*'
                    r'SRV\(t9,\s*numDescriptors\s*=\s*2,\s*flags\s*=\s*'
                    r'DESCRIPTORS_VOLATILE\s*\|\s*DATA_STATIC_WHILE_SET_AT_EXECUTE\),\s*'
                    r'SRV\(t11,\s*numDescriptors\s*=\s*6\)\s*\)'
                )
                if (not re.search(history_table, signature) or
                        len(re.findall(r'\bSRV\(', signature)) != 3 or len(actual) != 17):
                    fail("Composition SRV root table must be contiguous static t0..8, "
                         "descriptor-volatile/data-static-while-set t9..10, static t11..16 (17 total)")
            else:
                root = re.search(r'DescriptorTable\(' + root_kind + r'\(' + kind +
                                 r'0,\s*numDescriptors\s*=\s*(\d+)\)', shader)
                if not root or int(root.group(1)) != len(actual):
                    fail("%s %s root table differs from shader resources" % (namespace, root_kind))
            if namespace == "Composition" and kind == "u":
                expected = cpp_resource_order(body, "union Output")
                assertion = "Composition::kOutputCount"
            elif namespace == "Conversion" and kind == "u":
                output = brace_body(brace_body(body, "union Output"), "struct Data")
                signals = brace_body(body, "struct SignalResources")
                fields = re.findall(r'ComPtr<ID3D12Resource>\s+(\w+)\s*;',
                                    output.replace("SignalResources Signals;", signals))
                names = {"IndirectSpecular": "OutIndirectSpecular", "DirectDiffuse": "OutDirectDiffuse",
                         "Motion": "OutMotion", "Normals": "OutNormals", "SpecAlbedo": "OutSpecAlbedo",
                         "DiffAlbedo": "OutDiffAlbedo", "SkipSignal": "OutSkipSignal",
                         "DetailReference": "OutDetailReference",
                         "DirectSpecular": "OutDirectSpecular", "IndirectDiffuse": "OutIndirectDiffuse"}
                expected = [names.get(field, "UNMAPPED:" + field) for field in fields]
                assertion = namespace + "::Output::kCount"
            else:
                expected = cpp_resource_order(body, "union " + union)
                assertion = namespace + "::" + union + "::kCount"
            if expected != actual:
                fail("%s %s order differs: C++ %s, HLSL %s" %
                     (namespace, root_kind, expected, actual))
            count_assert = re.search(r'static_assert\(' + re.escape(assertion) + r' == (\d+)', data)
            if not count_assert or int(count_assert.group(1)) != len(actual):
                fail("%s count assertion differs from shader bindings" % assertion)


# ---------------------------------------------------------------- debug mode names

def check_debug_mode_names():
    """Every debug mode must be reachable from the menu."""
    pre = read(PRE_H)
    feature = read(FEATURE_CPP)
    conv_cpp = cpp_enum_values(pre, "ConvFlags")
    comp_cpp = cpp_enum_values(pre, "CompFlags")

    modes_body = brace_body(feature, "enum class DebugModes : uint64_t")
    alias_target = {}
    for raw in modes_body.split(chr(10)):
        line = strip_line_comment(raw).strip().rstrip(",")
        m = re.match(
            r"^(\w+)\s*=\s*(?:\(uint64_t\)\s*)?FSRD(?:Conv|Comp)Flags::(\w+)"
            r"(?:\s*<<\s*\w+)?$", line)
        if m:
            alias_target[m.group(1)] = m.group(2)
            if m.group(2) not in conv_cpp and m.group(2) not in comp_cpp:
                fail("DebugModes::%s aliases %s, which no flag enum defines"
                     % (m.group(1), m.group(2)))

    table_aliases = set()
    for m in re.finditer(r'\{\s*"([^"]+)",\s*\(uint64_t\)\s*DebugModes::(\w+)\s*\}', feature):
        table_aliases.add(m.group(2))
        if m.group(2) not in alias_target and m.group(2) not in OTHER_MODES:
            fail("the debug-mode name table lists DebugModes::%s, which the enum does not define"
                 % m.group(2))

    for enum_values, mode_names, label in ((conv_cpp, CONV_DEBUG_NAMES, "ConvFlags"),
                                           (comp_cpp, COMP_DEBUG_NAMES, "CompFlags")):
        for cpp_name in mode_names:
            if cpp_name not in enum_values:
                continue
            reached = [alias for alias, target in alias_target.items() if target == cpp_name]
            if not reached:
                fail("%s::%s has no DebugModes alias, so the name table cannot reach it"
                     % (label, cpp_name))
                continue
            if not any(alias in table_aliases for alias in reached):
                fail("%s::%s is not reachable from the debug-mode name table, so a user cannot "
                     "select it" % (label, cpp_name))


# ---------------------------------------------------------------- albedo storage precision

FORMAT_CONST = re.compile(r"constexpr\s+DXGI_FORMAT\s+(\w+)\s*=\s*DXGI_FORMAT_(\w+)\s*;")
UNORM_CHANNELS = re.compile(r"^R(\d+)G(\d+)B(\d+)A(\d+)_UNORM$")
ALBEDO_LEVELS = re.compile(r"static const float s_AlbedoStoreLevels\s*=\s*([0-9.]+)f\s*;")


def check_albedo_storage():
    formats = {m.group(1): m.group(2) for m in FORMAT_CONST.finditer(read(PREPROCESSOR_CPP))}
    m = ALBEDO_LEVELS.search(read(CONV_HLSL))
    if not m:
        fail("FSRDInputConv.hlsl declares no s_AlbedoStoreLevels, so the albedo quantization "
             "this check exists for has been removed or renamed")
        return

    levels = float(m.group(1))
    for name in ("SpecAlbedo", "DiffAlbedo"):
        fmt = formats.get(name)
        if fmt is None:
            fail("FSRDFormats::%s is not a plain DXGI_FORMAT_* constant, so the format the "
                 "shader quantizes for cannot be read" % name)
            continue

        channels = UNORM_CHANNELS.match(fmt)
        if not channels:
            fail("FSRDFormats::%s is DXGI_FORMAT_%s but FSRDInputConv.hlsl quantizes it to %g "
                 "levels; a format without a level count needs the quantization revisited"
                 % (name, fmt, levels))
            continue

        widths = {int(bits) for bits in channels.groups()}
        if len(widths) != 1:
            fail("FSRDFormats::%s is DXGI_FORMAT_%s, whose channels have different widths, so "
                 "the shader's single level count cannot describe it" % (name, fmt))
            continue

        want = float((1 << widths.pop()) - 1)
        if levels != want:
            fail("FSRDInputConv.hlsl quantizes albedo to %g levels but FSRDFormats::%s is "
                 "DXGI_FORMAT_%s, which holds %g" % (levels, name, fmt, want))


def check_composition_variants():
    for name, variant in [('FSRDOutputCompLight', 1), ('FSRDOutputCompNoRecovery', 2),
                         ('FSRDOutputCompTileLight', 3), ('FSRDOutputCompTileAnchor', 4)]:
        path = os.path.join(PRE, name + '.hlsl')
        source = read(path)
        if f'#define FSRD_COMPOSITION_VARIANT {variant}' not in source:
            fail(f'{name}: variant define drifted')
        if '#include "FSRDOutputComp.hlsl"' not in source:
            fail(f'{name}: must wrap the canonical composition shader')
    if '#if FSRD_COMPOSITION_VARIANT' not in read(COMP_HLSL):
        fail('FSRDOutputComp: default must retain the complete generic shader')


def check_thread_groups():
    source = read(TRUST_EVIDENCE_HLSL)
    if '[numthreads(16, 16, 1)]' not in source:
        fail("FSRDAlbedoTrustEvidence: shader numthreads drifted from 16x16")
    cpp = read(DATA_H)
    if 'constexpr UINT kThreadGroupSize = 16;' not in cpp:
        fail("FSRDAlbedoTrustEvidence: the preprocessor must dispatch with TrustEvidence::kThreadGroupSize")

    depth_source = read(UPSCALER_DEPTH_HLSL)
    if '[numthreads(THREAD_GROUP_SIZE_X, THREAD_GROUP_SIZE_Y, 1)]' not in depth_source:
        fail("FSRDUpscalerDepth: numthreads must match ComputeState group constants")


if __name__ == "__main__":
    check_constants()
    check_flags()
    check_resources()
    check_debug_mode_names()
    check_albedo_storage()
    check_composition_variants()
    check_thread_groups()
    if errors:
        print("FSRD mirror check FAILED (%d):" % len(errors))
        for e in errors:
            print("  - " + e)
        sys.exit(1)
    print("FSRD mirrors verified: constants layout, flag words, resource order and counts")

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Sanitizer controls for actual plane, template and colour-selection decisions.

Serialized input and strict-profile admission are separately covered by the
in-kernel apple-display-colour suite. This host check extracts the decisions
and constants used by production; it is not hardware colour qualification.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "drivers/gpu/drm/apple"


def function(source, name):
    match = re.search(r"^static [^\n]+ " + name + r"\(", source, re.M)
    if not match:
        raise ValueError(f"missing {name}")
    begin = source.index("{", match.end())
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def program(plane, template, parser, dcp):
    header = (DRIVER / "parser.h").read_text()
    constants = "\n".join(re.findall(r"enum dcp_color\w* \{.*?\n\};", header, re.S))
    constants += re.search(r"struct dcp_color_mode \{.*?\n\};", header, re.S)[0]
    constants += re.search(r"struct color_mode \{.*?\n\};", parser, re.S)[0]
    constants += re.search(r"enum dcp_colorspace \{.*?\n\};", (DRIVER / "iomfb_plane.h").read_text(), re.S)[0]
    fourcc = (ROOT / "include/uapi/drm/drm_fourcc.h").read_text()
    for name in ("XRGB8888", "XBGR8888", "XRGB2101010", "XBGR2101010",
                 "ARGB8888", "ABGR8888", "ARGB2101010", "ABGR2101010", "NV12"):
        constants += "\n" + re.search(r"^#define DRM_FORMAT_" + name + r"\s[^\n]+", fourcc, re.M)[0]
    constants += "\n" + re.search(r"^#define fourcc_code[^\n]+\\\n[^\n]+", fourcc, re.M)[0]
    constants += "\n" + re.search(r"^#define DRM_MODE_CONNECTOR_eDP[^\n]+", (ROOT / "include/uapi/drm/drm_mode.h").read_text(), re.M)[0]
    defaults = re.search(r"^static int external_colorimetry[^\n]+", parser, re.M)[0]
    defaults += "\n" + re.search(r"^bool external_srgb[^\n]+", dcp, re.M)[0]
    plane_begin = plane.index("\tif (fmt->format == DRM_FORMAT_XRGB8888")
    plane_end = plane.index("\n\tnew_state->src_rect", plane_begin)
    plane_block = plane[plane_begin:plane_end]
    template_begin = template.index("\t\tif (dcp->connector_type == DRM_MODE_CONNECTOR_eDP &&")
    template_end = template.index("\n\t}\n\n\tif (!has_surface", template_begin)
    template_block = template[template_begin:template_end]
    parse_body = function(parser, "parse_color_modes")
    choose_begin = parse_body.rindex("\t\tif (cmode.eotf == DCP_EOTF_SDR_GAMMA)")
    choose_end = parse_body.index("\t\tfill_color_mode(&out->best, &cmode);", choose_begin)
    choose_block = parse_body[choose_begin:choose_end]
    snapshot = re.search(r"\tint preferred = READ_ONCE\(external_colorimetry\);\n\tbool prefer_colorimetry = .*?;", parse_body, re.S)[0]
    assert template.count("bool use_external_srgb = READ_ONCE(external_srgb);") == 1
    return r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
typedef uint32_t u32; typedef uint32_t __u32; typedef uint8_t u8; typedef int64_t s64;
#define READ_ONCE(x) (x)
''' + constants + "\n" + defaults + function(parser, "fill_color_mode") + function(parser, "external_colorimetry_rank") + r'''
static bool premult(u32 format) {
    struct { u32 format; } value = {format}, *fmt = &value;
    bool is_premultiplied = false;
''' + plane_block + r'''
    return is_premultiplied;
}
static unsigned surface(unsigned connector, bool yuv, unsigned source, bool use_external_srgb) {
    struct { unsigned connector_type; } dp = {connector}, *dcp = &dp;
    struct { struct { struct { unsigned colorspace; } base; } surf[1]; } req_value = {0}, *req = &req_value;
    struct { bool is_yuv; } format = {yuv};
    struct { typeof(format) *format; } fb = {&format};
    struct { typeof(fb) *fb; } state = {&fb}, *new_state = &state;
    (void)new_state; /* a guard-removal mutant still needs to compile */
    int l = 0; req->surf[0].base.colorspace = source;
''' + template_block + r'''
    return req->surf[0].base.colorspace;
}
static struct dcp_color_mode choose(const struct color_mode *cs, unsigned count, int preference, bool internal) {
    struct { struct dcp_color_mode sdr_rgb, sdr_444, sdr; } state = {
        .sdr_rgb.score = -1, .sdr_444.score = -1, .sdr.score = -1}, *out = &state;
    unsigned best_sdr_rank = 0;
    bool external_sdr = false; /* this is the ordinary, non-strict decision */
    external_colorimetry = preference;
''' + snapshot + r'''
    for (unsigned i=0; i<count; i++) { struct color_mode cmode = cs[i];
''' + choose_block + r'''
    }
    return out->sdr_rgb;
}
int main(void) {
    unsigned cases = 0;
    assert(!external_srgb);
    assert(external_colorimetry == -1);
    const u32 x[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010};
    const u32 other[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_ARGB2101010, DRM_FORMAT_ABGR2101010, DRM_FORMAT_NV12};
    assert(premult(DRM_FORMAT_XRGB2101010)); cases++;
    for (unsigned i=0;i<sizeof(x)/sizeof(*x);i++) { assert(premult(x[i])); cases++; }
    for (unsigned i=0;i<sizeof(other)/sizeof(*other);i++) { assert(!premult(other[i])); cases++; }
    const unsigned spaces[] = {DCP_COLORSPACE_BG_SRGB,DCP_COLORSPACE_NATIVE,DCP_COLORSPACE_BT601,DCP_COLORSPACE_BT709,DCP_COLORSPACE_BG_BT2020};
    for(unsigned e=0;e<2;e++)for(unsigned y=0;y<2;y++)for(unsigned on=0;on<2;on++)for(unsigned i=0;i<5;i++) {
        unsigned expected=spaces[i];
        if(e && spaces[i]==DCP_COLORSPACE_BG_SRGB) expected=DCP_COLORSPACE_NATIVE;
        else if(!e && !y && on && spaces[i]==DCP_COLORSPACE_NATIVE) expected=DCP_COLORSPACE_BG_SRGB;
        assert(surface(e?DRM_MODE_CONNECTOR_eDP:11,y,spaces[i],on)==expected); cases++;
    }
    struct color_mode cs[3] = {
        {.id=69,.score=440999,.depth=10,.colorimetry=DCP_COLORIMETRY_RGB,.dynamic_range=0,.eotf=0,.pixel_encoding=0},
        {.id=68,.score=447999,.depth=10,.colorimetry=DCP_COLORIMETRY_BT2020_RGB,.dynamic_range=0,.eotf=0,.pixel_encoding=0},
        {.id=67,.score=200000,.depth=10,.colorimetry=DCP_COLORIMETRY_SRGB,.dynamic_range=0,.eotf=0,.pixel_encoding=0},
    };
    assert(choose(cs,3,-1,false).id==68); cases++;
    assert(choose(cs,3,DCP_COLORIMETRY_SRGB,false).id==67); cases++;
    assert(choose(cs,2,DCP_COLORIMETRY_SRGB,false).id==69); cases++;
    assert(choose(cs,3,DCP_COLORIMETRY_SRGB,true).id==68); cases++;
    assert(choose(cs,3,INT_MAX,false).id==68); cases++;
    for(unsigned bad=0;bad<4;bad++)for(unsigned order=0;order<2;order++) {
        struct color_mode pair[2]; pair[order]=cs[0];pair[!order]=cs[2];pair[!order].score=500000;
        if(bad==0)pair[!order].depth=-1;
        if(bad==1)pair[!order].dynamic_range=-1;
        if(bad==2)pair[!order].depth=0;
        if(bad==3)pair[!order].dynamic_range=DCP_COLOR_YCBCR_RANGE_COUNT;
        struct dcp_color_mode result=choose(pair,2,DCP_COLORIMETRY_SRGB,false);
        assert(result.score==440999 && result.id==69); cases++;
    }
    printf("PASS: %u display colour decisions\n",cases);
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    plane = (DRIVER / "plane.c").read_text()
    template = (DRIVER / "iomfb_template.c").read_text()
    colour = (DRIVER / "parser.c").read_text()
    dcp = (DRIVER / "dcp.c").read_text()
    variants = {"real": (plane, template, colour, dcp)}
    if args.mutations:
        variants["missing-xrgb10"] = (plane.replace("\n\t    fmt->format == DRM_FORMAT_XRGB2101010 ||", "", 1), template, colour, dcp)
        variants["convert-yuv"] = (plane, template.replace("\n\t\t\t !new_state->fb->format->is_yuv &&", "", 1), colour, dcp)
        variants["convert-internal"] = (plane, template.replace("\n\t\t\t dcp->connector_type != DRM_MODE_CONNECTOR_eDP &&", "", 1), colour, dcp)
        bad = re.sub(r"struct dcp_color_mode candidate = out->sdr_rgb;.*?best_sdr_rank = rank;\n\t\t\t\t\t}",
                     "if (rank > best_sdr_rank) out->sdr_rgb.score = -1;\nfill_color_mode(&out->sdr_rgb, &cmode);\nbest_sdr_rank = rank;", colour, count=1, flags=re.S)
        assert bad != colour
        variants["unchecked-promotion"] = (plane, template, bad, dcp)
        variants["conversion-default-on"] = (plane, template, colour, dcp.replace("bool external_srgb;", "bool external_srgb = true;", 1))
        variants["preference-default-on"] = (plane, template, colour.replace("static int external_colorimetry = -1;", "static int external_colorimetry = 10;", 1), dcp)
    reasons = {
        "missing-xrgb10": "premult(DRM_FORMAT_XRGB2101010)",
        "convert-yuv": "surface(e?DRM_MODE_CONNECTOR_eDP:11,y,spaces[i],on)==expected",
        "convert-internal": "surface(e?DRM_MODE_CONNECTOR_eDP:11,y,spaces[i],on)==expected",
        "unchecked-promotion": "result.score==440999 && result.id==69",
        "conversion-default-on": "!external_srgb",
        "preference-default-on": "external_colorimetry == -1",
    }
    receipt = {}
    with tempfile.TemporaryDirectory(prefix="sf-display-colour-") as tmp:
        for name, sources in variants.items():
            source = Path(tmp) / (name + ".c")
            source.write_text(program(*sources))
            binary = source.with_suffix("")
            subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", str(source), "-o", str(binary)], check=True)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            if name == "real" and run.returncode:
                raise SystemExit(run.stderr)
            if name != "real" and (run.returncode == 0 or reasons[name] not in run.stderr or "Assertion" not in run.stderr):
                raise SystemExit(f"{name} did not fail its intended runtime control: {run.stderr}")
            receipt[name] = {"exit": run.returncode, "stdout": run.stdout.strip(), "expected_failure": name != "real"}
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()

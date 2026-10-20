#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Run the real UVC query bodies with hostile lengths and injected USB errors.

Only allocation, USB I/O, lookup and user-copy dependencies are simulated.
The source and UAPI constants come from this checkout (or --ref). No camera
is opened and no test intentionally crashes a running kernel.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CTRL = "drivers/media/usb/uvc/uvc_ctrl.c"
VIDEO = "drivers/media/usb/uvc/uvc_video.c"


def read(path, ref):
    if ref:
        return subprocess.check_output(["git", "show", f"{ref}:{path}"], cwd=ROOT, text=True)
    return (ROOT / path).read_text()


def function(source, name):
    match = re.search(r"^int " + name + r"\(", source, re.M)
    if not match:
        raise ValueError(f"missing function {name}")
    begin = source.index("{", match.end())
    # These two functions have balanced braces in comments/strings as well.
    depth, end = 1, begin + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


STUBS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define ERESTARTSYS 512
#define GFP_KERNEL 0
#define likely(x) (x)
#define UVC_ENTITY_TYPE(e) ((e)->type & 0x7fff)
#define UVC_CTRL_CONTROL_TIMEOUT 5000
#define UVC_VC_REQUEST_ERROR_CODE_CONTROL 2
#define uvc_dbg(...) ((void)0)
#define dev_warn_once(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
struct uvc_control { unsigned index; struct { u16 size; u32 flags; } info; };
struct uvc_entity { int type, id; unsigned ncontrols; u8 guid[16]; struct uvc_control *controls; };
struct uvc_device { int intfnum; };
struct uvc_video_chain { struct uvc_entity *entities; struct uvc_device *dev; int ctrl_mutex; };
struct uvc_xu_control_query { u8 unit, selector, query; u16 size; u8 *data; };
#define list_for_each_entry(it, head, member) \
    for ((it) = *(head); (it) != NULL; (it) = NULL)
static bool uvc_allow_privacy_override_param;
static bool privacy, alloc_fail, copy_fail, lock_fail;
static unsigned allocated, freed, usb_calls;
static int init_error, transfer_result, error_result;
static u8 error_value;
static bool uvc_ctrl_is_privacy_control(const u8 *guid, u8 selector)
{ (void)guid; (void)selector; return privacy; }
static int mutex_lock_interruptible(int *m)
{ if (lock_fail) return -1; assert(!*m); *m = 1; return 0; }
static void mutex_unlock(int *m) { assert(*m); *m = 0; }
static int uvc_ctrl_init_xu_ctrl(struct uvc_device *d, struct uvc_control *c)
{ (void)d; (void)c; return init_error; }
static void *kmalloc(size_t size, int flags)
{ (void)flags; allocated++; if (alloc_fail) return NULL;
  return size ? malloc(size) : (void *)(uintptr_t)16; }
static void kfree(void *p)
{ if (p && (uintptr_t)p != 16) { freed++; free(p); } }
static int copy_to_user(void *d, const void *s, size_t n)
{ if (copy_fail) return 1; memcpy(d, s, n); return 0; }
static int copy_from_user(void *d, const void *s, size_t n)
{ return copy_to_user(d, s, n); }
static int __uvc_query_ctrl(struct uvc_device *d, u8 query, u8 unit,
                          u8 intf, u8 cs, void *data, u16 size, int timeout)
{
    (void)d; (void)intf;
    assert(timeout == UVC_CTRL_CONTROL_TIMEOUT);
    usb_calls++;
    if (unit == 0) {
        assert(query == UVC_GET_CUR && cs == UVC_VC_REQUEST_ERROR_CODE_CONTROL && size == 1);
        if (error_result == 1) *(u8 *)data = error_value;
        return error_result;
    }
    assert(unit == 4 && cs == 1);
    if (transfer_result >= 0) {
        assert(transfer_result <= size);
        if (query != UVC_SET_CUR) memset(data, 0xa5, transfer_result);
    }
    return transfer_result;
}
'''

TESTS = r'''
static struct uvc_control control;
static struct uvc_entity entity;
static struct uvc_device device;
static struct uvc_video_chain chain;
static unsigned cases;
static void reset(unsigned size)
{
    control = (struct uvc_control){.index = 0, .info = {.size = size, .flags = 0x3f}};
    entity = (struct uvc_entity){.type = UVC_VC_EXTENSION_UNIT, .id = 4,
                               .ncontrols = 1, .controls = &control};
    chain = (struct uvc_video_chain){.entities = &entity, .dev = &device};
    privacy = alloc_fail = copy_fail = lock_fail = false;
    allocated = freed = usb_calls = 0;
    init_error = 0; transfer_result = size; error_result = 1; error_value = 4;
}
static int xu(u8 query, unsigned size)
{
    u8 data[32] = {};
    struct uvc_xu_control_query q = {.unit=4, .selector=1, .query=query,
                                    .size=size, .data=data};
    int ret = uvc_xu_ctrl_query(&chain, &q);
    assert(chain.ctrl_mutex == 0); /* all error paths unlock */
    return ret;
}
static void xu_cases(void)
{
    const u8 values[] = {UVC_GET_CUR, UVC_GET_MIN, UVC_GET_MAX,
                        UVC_GET_RES, UVC_GET_DEF, UVC_SET_CUR};
    for (unsigned i=0; i<sizeof(values); i++) {
        reset(0);
        assert(xu(values[i], 0) == -ENODATA);
        assert(!allocated && !usb_calls); cases++;
        reset(0);
        assert(xu(values[i], 1) == -ENODATA);
        assert(!allocated && !usb_calls); cases++;
        reset(4);
        assert(xu(values[i], 4) == 0);
        assert(allocated == 1 && freed == 1 && usb_calls == 1); cases++;
        reset(4);
        assert(xu(values[i], 3) == -ENOBUFS);
        assert(!allocated && !usb_calls); cases++;
        reset(4); control.info.flags = 0;
        assert(xu(values[i], 4) == -EBADRQC);
        assert(!allocated && !usb_calls); cases++;
    }
    for (unsigned ctrl_size=0; ctrl_size<=4; ctrl_size+=4) {
        reset(ctrl_size); transfer_result = 2;
        assert(xu(UVC_GET_LEN, 2) == 0 && usb_calls == 1 && freed == 1); cases++;
        reset(ctrl_size); transfer_result = 1;
        assert(xu(UVC_GET_INFO, 1) == 0 && usb_calls == 1 && freed == 1); cases++;
    }
    reset(4); alloc_fail = true;
    assert(xu(UVC_GET_CUR, 4) == -ENOMEM && !usb_calls); cases++;
    reset(4); copy_fail = true;
    assert(xu(UVC_SET_CUR, 4) == -EFAULT && !usb_calls && freed == 1); cases++;
    reset(4); copy_fail = true;
    assert(xu(UVC_GET_CUR, 4) == -EFAULT && usb_calls == 1 && freed == 1); cases++;
    reset(4); transfer_result = -EPIPE;
    assert(xu(UVC_GET_CUR, 4) == -ERANGE && usb_calls == 2 && freed == 1); cases++;
    reset(4); init_error = -EIO;
    assert(xu(UVC_GET_CUR, 4) == -ENOENT && !allocated); cases++;
    reset(4); privacy = true;
    assert(xu(UVC_GET_CUR, 4) == -EACCES && !allocated); cases++;
    reset(4); lock_fail = true;
    assert(xu(UVC_GET_CUR, 4) == -ERESTARTSYS && !allocated); cases++;
    reset(4); chain.entities = NULL;
    assert(xu(UVC_GET_CUR, 4) == -ENOENT && !allocated); cases++;
    reset(4); entity.ncontrols = 0;
    assert(xu(UVC_GET_CUR, 4) == -ENOENT && !allocated); cases++;
    reset(4);
    assert(xu(0xff, 4) == -EINVAL && !allocated); cases++;
}
static void transport_cases(void)
{
    const int responses[] = {-EPIPE, -ENODEV, -ETIMEDOUT, 0};
    for (unsigned i=0; i<sizeof(responses)/sizeof(*responses); i++) {
        reset(0); transfer_result = responses[i];
        int ret = uvc_query_ctrl(&device, UVC_GET_CUR, 4, 0, 1,
                                 (void *)(uintptr_t)16, 0);
        assert(ret == (responses[i] == 0 ? 0 : responses[i]));
        assert(usb_calls == 1); cases++;
    }
    reset(4); u8 data[4] = {1,2,3,4}; transfer_result = 2;
    assert(uvc_query_ctrl(&device, UVC_GET_CUR, 4, 0, 1, data, 4) == 0);
    assert(data[0] == 0xa5 && data[1] == 0xa5 && !data[2] && !data[3]); cases++;
    const int mapped[] = {-EPIPE,-EBUSY,-EACCES,-EREMOTE,-ERANGE,-EIO,-EIO,-EIO,-EINVAL,-EPIPE};
    for (unsigned i=0; i<sizeof(mapped)/sizeof(*mapped); i++) {
        reset(4); transfer_result = -EPIPE; error_value = i;
        u8 buf[4] = {42,3,4,5};
        assert(uvc_query_ctrl(&device, UVC_GET_CUR, 4, 0, 1, buf, 4) == mapped[i]);
        assert(buf[0] == 42 && usb_calls == 2); cases++;
    }
    reset(4); transfer_result = -EPIPE; error_result = -ETIMEDOUT;
    u8 buf[4] = {42,3,4,5};
    assert(uvc_query_ctrl(&device, UVC_GET_CUR, 4, 0, 1, buf, 4) == -ETIMEDOUT);
    assert(buf[0] == 42); cases++;
}
int main(void) { xu_cases(); transport_cases(); printf("PASS: %u UVC cases\n", cases); }
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ref", help="read production source and UAPI from a git revision")
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    ctrl = function(read(CTRL, args.ref), "uvc_xu_ctrl_query")
    video = function(read(VIDEO, args.ref), "uvc_query_ctrl")
    constants = "\n".join(line for path in ("include/uapi/linux/usb/video.h", "include/uapi/linux/uvcvideo.h")
                          for line in read(path, args.ref).splitlines()
                          if re.match(r"#define UVC_(?:GET_\w+|SET_CUR|VC_EXTENSION_UNIT|CTRL_FLAG_(?:GET_\w+|SET_CUR))\s", line))
    variants = {"real": (ctrl, video)}
    if args.mutations:
        guard = "\tif (size == 0) {\n\t\tret = -ENODATA;\n\t\tgoto done;\n\t}\n"
        transport = "\tif (size == 0)\n\t\treturn -EPIPE;\n"
        if guard not in ctrl or transport not in video:
            raise SystemExit("mutation anchors missing")
        variants["no-xu-guard"] = (ctrl.replace(guard, "", 1), video)
        variants["no-transport-guard"] = (ctrl, video.replace(transport, "", 1))
        variants["block-discovery"] = (ctrl.replace("size = ctrl->info.size;", "size = ctrl->info.size;\n" + guard, 1), video)
        variants["wrong-zero-errno"] = (ctrl.replace("ret = -ENODATA;", "ret = -EINVAL;", 1), video)
    receipt = {"source_ref": args.ref, "bodies_sha256": hashlib.sha256((ctrl+video).encode()).hexdigest(), "variants": {}}
    with tempfile.TemporaryDirectory(prefix="sf-uvc-tests-") as tmp:
        for name, (cbody, vbody) in variants.items():
            source = Path(tmp) / f"{name}.c"
            binary = source.with_suffix("")
            source.write_text(constants + "\n" + STUBS + vbody + "\n" + cbody + TESTS)
            subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", str(source), "-o", str(binary)], check=True)
            run = subprocess.run([str(binary)], text=True, capture_output=True)
            passed = run.returncode == 0
            if name == "real" and not passed:
                raise SystemExit(run.stdout + run.stderr)
            if name != "real" and (passed or not re.search(r"Assertion .* failed|AddressSanitizer|runtime error:", run.stderr)):
                raise SystemExit(f"mutation {name} did not fail its runtime control: {run.stderr}")
            receipt["variants"][name] = {"exit": run.returncode, "stdout": run.stdout.strip(), "expected_failure": name != "real"}
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the production fdinfo residency helpers with cached and imported backing."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "drivers/gpu/drm/drm_gem_shmem_helper.c"


def function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument('--ref', help='Read production functions at this git revision')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    original = (subprocess.check_output(['git', 'show', f'{args.ref}:drivers/gpu/drm/drm_gem_shmem_helper.c'], cwd=ROOT, text=True) if args.ref else SOURCE.read_text())
    functions = "\n".join(function(original, signature) for signature in [
        "size_t drm_gem_shmem_object_rss(",
        "enum drm_gem_object_status drm_gem_shmem_object_status(",
    ])
    reporter = function((ROOT / 'drivers/gpu/drm/drm_file.c').read_text(), 'void drm_show_memory_stats(')
    fixture = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define min_t(type,a,b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define PAGE_SIZE (1UL << PAGE_SHIFT)
struct address_space { unsigned long nrpages; };
struct file { struct address_space *f_mapping; };
struct drm_gem_object;
enum drm_gem_object_status { DRM_GEM_OBJECT_RESIDENT = 1, DRM_GEM_OBJECT_PURGEABLE = 2, DRM_GEM_OBJECT_ACTIVE = 4 };
struct drm_gem_object_funcs { size_t (*rss)(struct drm_gem_object *); enum drm_gem_object_status (*status)(struct drm_gem_object *); };
struct drm_gem_object { size_t size; void *import_attach; struct file *filp; const struct drm_gem_object_funcs *funcs; bool *resv; bool shared; };
struct drm_memory_stats { uint64_t shared, private, resident, purgeable, active; };
struct drm_printer { struct drm_memory_stats result; unsigned supported; };
struct fake_idr { size_t count; struct drm_gem_object *objects[3]; };
struct drm_file { int table_lock; struct fake_idr object_idr; };
#define idr_for_each_entry(idr,obj,id) for((id)=0;(size_t)(id)<(idr)->count && (((obj)=(idr)->objects[id]),1);(id)++)
static void spin_lock(int *lock) { assert(!*lock); *lock=1; }
static void spin_unlock(int *lock) { assert(*lock); *lock=0; }
static bool drm_gem_object_is_shared_for_memory_stats(struct drm_gem_object *obj) { return obj->shared; }
static bool dma_resv_test_signaled(bool *r,int usage) { assert(usage==1);return *r; }
static int dma_resv_usage_rw(bool write) { assert(write);return 1; }
static void drm_print_memory_stats(struct drm_printer *p,const struct drm_memory_stats *s,unsigned supported,const char *region) { assert(!strcmp(region,"memory"));p->result=*s;p->supported=supported; }
FUNCTIONS
REPORTER
int main(void) {
    struct address_space mapping = {0};
    struct file backing = { .f_mapping = &mapping };
    struct drm_gem_object obj = { .size = 5 * PAGE_SIZE };
    assert(drm_gem_shmem_object_rss(&obj) == 0);
    assert(drm_gem_shmem_object_status(&obj) == 0);
    obj.filp = &backing;
    assert(drm_gem_shmem_object_rss(&obj) == 0);
    mapping.nrpages = 2;
    assert(drm_gem_shmem_object_rss(&obj) == 2 * PAGE_SIZE);
    assert(drm_gem_shmem_object_status(&obj) == DRM_GEM_OBJECT_RESIDENT);
    /* A cached backing remains resident independently of a device page-array pin. */
    mapping.nrpages = 5;
    assert(drm_gem_shmem_object_rss(&obj) == obj.size);
    mapping.nrpages = 9;
    assert(drm_gem_shmem_object_rss(&obj) == obj.size);
    mapping.nrpages = ULONG_MAX;
    assert(drm_gem_shmem_object_rss(&obj) == obj.size);
    mapping.nrpages = (1UL << (sizeof(unsigned long) * CHAR_BIT - PAGE_SHIFT));
    assert(drm_gem_shmem_object_rss(&obj) == obj.size);
    mapping.nrpages = 0;
    assert(drm_gem_shmem_object_rss(&obj) == 0);
    assert(drm_gem_shmem_object_status(&obj) == 0);
    obj.filp = NULL;
    obj.import_attach = &mapping;
    assert(drm_gem_shmem_object_rss(&obj) == obj.size);
    assert(drm_gem_shmem_object_status(&obj) == DRM_GEM_OBJECT_RESIDENT);
    assert(obj.size == 5 * PAGE_SIZE && mapping.nrpages == 0);
    const struct drm_gem_object_funcs callbacks = { .rss=drm_gem_shmem_object_rss, .status=drm_gem_shmem_object_status };
    bool idle=true, busy=false;
    struct address_space local_mapping={2},empty_mapping={0};
    struct file local_backing={&local_mapping},empty_backing={&empty_mapping};
    struct drm_gem_object local={.size=5*PAGE_SIZE,.filp=&local_backing,.funcs=&callbacks,.resv=&idle};
    struct drm_gem_object imported={.size=3*PAGE_SIZE,.import_attach=&mapping,.funcs=&callbacks,.resv=&busy,.shared=true};
    struct drm_gem_object empty={.size=4*PAGE_SIZE,.filp=&empty_backing,.funcs=&callbacks,.resv=&idle};
    struct drm_file client={.object_idr={.count=3,.objects={&local,&imported,&empty}}};
    struct drm_printer printer={0};
    drm_show_memory_stats(&printer,&client);
    assert(!client.table_lock);
    assert(printer.result.private==9*PAGE_SIZE && printer.result.shared==3*PAGE_SIZE);
    assert(printer.result.resident==5*PAGE_SIZE && printer.result.active==3*PAGE_SIZE);
    assert(printer.result.purgeable==0);
    assert((printer.supported & (DRM_GEM_OBJECT_RESIDENT|DRM_GEM_OBJECT_ACTIVE)) == (DRM_GEM_OBJECT_RESIDENT|DRM_GEM_OBJECT_ACTIVE));
    assert(local_mapping.nrpages==2 && empty_mapping.nrpages==0);
    puts("PASS actual helper callbacks and DRM fdinfo walk: cached/imported/empty memory, no population");
}
'''.replace("FUNCTIONS", functions).replace("REPORTER", reporter)
    source = args.output / "fdinfo.c"
    source.write_text(fixture)
    results = []
    for shift in [12, 14]:
        binary = args.output / f"fdinfo-{shift}"
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Werror", "-g",
                        "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                        f"-DPAGE_SHIFT={shift}", str(source), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
        results.append({"page_shift": shift, "stdout": result.stdout, "stderr": result.stderr})
    record = {"production_sha256": hashlib.sha256((functions + reporter).encode()).hexdigest(),
              "results": results,
              "scope": "Actual C helper and DRM reporter bodies with typed file/cache/IDR/printer/reservation boundary stubs; not kernel ABI or hardware execution."}
    (args.output / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    main()

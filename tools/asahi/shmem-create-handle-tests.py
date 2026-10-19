#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Check shmem::Object::create_handle()'s device identity guard.

The method body is extracted from rust/kernel/drm/gem/shmem.rs and compiled on the
host against minimal stand-ins for the DRM bindings it touches. Cases:

- same-device: a file opened on the object's own device gets a handle, and
  drm_gem_handle_create() runs once;
- other-device: a file of another device of the same driver is refused with
  EINVAL, and drm_gem_handle_create() never runs.

--mutant no-guard removes the identity check, and the other-device case must
then fail.
"""
from pathlib import Path
import argparse, re, subprocess, sys, tempfile

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--ref', default='WORKTREE', help='git revision to read shmem.rs from')
parser.add_argument('--mutant', choices=['no-guard'])
args = parser.parse_args()

wt = Path(__file__).resolve().parents[2]
path = 'rust/kernel/drm/gem/shmem.rs'
source = ((wt / path).read_text() if args.ref == 'WORKTREE' else
          subprocess.check_output(['git', 'show', f'{args.ref}:{path}'], cwd=wt, text=True))

m = re.search(r'^    pub fn create_handle<F>\(.*?^    }\n', source, re.M | re.S)
if not m:
    sys.exit('create_handle() not found in ' + path)
method = m.group(0)
if args.mutant == 'no-guard':
    method, n = re.subn(r'\n        if file_dev != self\.dev\(\)\.as_raw\(\) \{\n            return Err\(EINVAL\);\n        }\n',
                        '\n', method)
    assert n == 1, 'guard not found for the mutant'

program = r'''
#![allow(dead_code, non_camel_case_types)]
use core::marker::PhantomData;
type Result<T = ()> = core::result::Result<T, i32>;
const EINVAL: i32 = -22;
fn to_result(r: i32) -> Result { if r < 0 { Err(r) } else { Ok(()) } }
mod bindings {
    pub struct drm_device { pub id: u32 }
    pub struct drm_minor { pub dev: *mut drm_device }
    pub struct drm_file { pub minor: *mut drm_minor }
    pub struct drm_gem_object { pub dev: *mut drm_device }
    pub static mut CREATES: u32 = 0;
    pub unsafe fn drm_gem_handle_create(_f: *mut drm_file, _o: *mut drm_gem_object, h: *mut u32) -> i32 {
        unsafe { CREATES += 1; *h = 7; }
        0
    }
}
mod drm {
    pub mod file { pub trait DriverFile { type Driver; } }
    pub struct File<F>(pub *mut crate::bindings::drm_file, pub core::marker::PhantomData<F>);
    impl<F> File<F> { pub fn as_raw(&self) -> *mut crate::bindings::drm_file { self.0 } }
    pub struct Device(pub *mut crate::bindings::drm_device);
    impl Device { pub fn as_raw(&self) -> *mut crate::bindings::drm_device { self.0 } }
}
trait DriverObject { type Driver; }
struct Drv;
struct Obj;
impl DriverObject for Obj { type Driver = Drv; }
struct Fil;
impl drm::file::DriverFile for Fil { type Driver = Drv; }
struct Object<T: DriverObject> { gem: bindings::drm_gem_object, dev: drm::Device, _t: PhantomData<T> }
impl<T: DriverObject> Object<T> {
    fn as_raw(&self) -> *mut bindings::drm_gem_object { &self.gem as *const _ as *mut _ }
    fn dev(&self) -> &drm::Device { &self.dev }
@@METHOD@@
}
fn creates() -> u32 { unsafe { bindings::CREATES } }
fn main() {
    let mut a = bindings::drm_device { id: 1 };
    let mut b = bindings::drm_device { id: 2 };
    let pa: *mut bindings::drm_device = &mut a;
    let pb: *mut bindings::drm_device = &mut b;
    let obj: Object<Obj> = Object { gem: bindings::drm_gem_object { dev: pa }, dev: drm::Device(pa), _t: PhantomData };
    let mut minor_a = bindings::drm_minor { dev: pa };
    let mut minor_b = bindings::drm_minor { dev: pb };
    let mut file_a = bindings::drm_file { minor: &mut minor_a };
    let mut file_b = bindings::drm_file { minor: &mut minor_b };
    let fa: drm::File<Fil> = drm::File(&mut file_a, PhantomData);
    let fb: drm::File<Fil> = drm::File(&mut file_b, PhantomData);
    let case = std::env::args().nth(1).unwrap();
    let ok = match case.as_str() {
        "same-device" => obj.create_handle(&fa) == Ok(7) && creates() == 1,
        "other-device" => obj.create_handle(&fb) == Err(EINVAL) && creates() == 0,
        _ => false,
    };
    println!("{}: {}", case, if ok { "PASS" } else { "FAIL" });
    std::process::exit(if ok { 0 } else { 1 });
}
'''.replace('@@METHOD@@', method)

with tempfile.TemporaryDirectory() as d:
    src, exe = Path(d) / 'h.rs', Path(d) / 'h'
    src.write_text(program)
    subprocess.run(['rustc', '--edition=2021', '-O', '-o', str(exe), str(src)], check=True)
    results = {c: subprocess.run([str(exe), c]).returncode for c in ('same-device', 'other-device')}
failed = [c for c, rc in results.items() if rc]
print('result:', 'FAIL ' + ','.join(failed) if failed else 'PASS')
sys.exit(1 if failed else 0)

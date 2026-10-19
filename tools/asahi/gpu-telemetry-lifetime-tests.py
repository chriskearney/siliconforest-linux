#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the actual telemetry owner and removal bodies against device teardown."""
import argparse
import json
import hashlib
import resource
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PATH = "drivers/gpu/drm/asahi/g17/telemetry.rs"


def item(text, start):
    a = text.index(start)
    b = text.index("{", a) + 1
    depth = 1
    while depth:
        depth += (text[b] == "{") - (text[b] == "}")
        b += 1
    return text[a:b].replace("pub(super)", "pub")


PREFIX = r'''
#![allow(dead_code, unused_imports, private_interfaces)]
use std::{ffi::{c_int,c_void},ops::Deref,sync::{Arc,Mutex,Weak}};
type Result<T> = std::result::Result<T,i32>;
extern crate self as kernel;
mod error { pub fn to_result(v:i32)->Result<(),i32>{if v<0{Err(v)}else{Ok(())}} }
struct Inner;
struct Device { owner: Arc<Inner> }
impl Device { fn as_raw(&self)->*mut Inner {Arc::as_ptr(&self.owner).cast_mut()} }
struct ARef<T>(T);
impl<T> Deref for ARef<T>{type Target=T;fn deref(&self)->&T{&self.0}}
impl From<&Device> for ARef<Device>{fn from(v:&Device)->Self{Self(Device{owner:v.owner.clone()})}}
struct Statistics;
static CURRENT:Mutex<Option<(Weak<Inner>,Weak<Statistics>)>>=Mutex::new(None);
static EVENTS:Mutex<Vec<&'static str>>=Mutex::new(Vec::new());
static FAIL:Mutex<bool>=Mutex::new(false);
unsafe extern "C" fn snapshot(_data:*const c_void,_out:*mut c_void)->c_int{0}
unsafe fn asahi_telemetry_register(_dev:*mut c_void,_data:*const c_void,_snapshot:unsafe extern "C" fn(*const c_void,*mut c_void)->c_int,handle:*mut *mut c_void)->c_int{
 if *FAIL.lock().unwrap(){return -12;}
 unsafe{*handle=std::ptr::dangling_mut();}
 EVENTS.lock().unwrap().push("registered");0
}
unsafe fn asahi_telemetry_unregister(_dev:*mut c_void,_handle:*mut c_void){
 let current=CURRENT.lock().unwrap();let (device,stats)=current.as_ref().unwrap();
 assert!(device.upgrade().is_some(),"attribute removal used a released platform device");
 assert!(stats.upgrade().is_some(),"callback data was dropped before reader drain");
 EVENTS.lock().unwrap().push("readers-drained");
}
'''
MAIN = r'''
fn main(){
 // Last open file releases a GPU after its platform binding has gone away.
 let dev=Device{owner:Arc::new(Inner)};
 let device=Arc::downgrade(&dev.owner);
 let stats=Arc::new(Statistics);let data=Arc::downgrade(&stats);
 *CURRENT.lock().unwrap()=Some((device.clone(),data.clone()));
 let registration=Telemetry::new(&dev,stats).unwrap();
 drop(dev);
 assert!(device.upgrade().is_some(),"GPU file must retain the platform-device reference");
 assert!(data.upgrade().is_some());
 drop(registration);
 assert_eq!(*EVENTS.lock().unwrap(),["registered","readers-drained"]);
 assert!(device.upgrade().is_none());assert!(data.upgrade().is_none());
 // Registration failure unwinds both owners, without removing a nonexistent file.
 *FAIL.lock().unwrap()=true;
 let dev=Device{owner:Arc::new(Inner)};let device=Arc::downgrade(&dev.owner);
 let stats=Arc::new(Statistics);let data=Arc::downgrade(&stats);
 assert!(Telemetry::new(&dev,stats).is_err());
 assert!(data.upgrade().is_none());assert_eq!(Arc::strong_count(&dev.owner),1);
 drop(dev);assert!(device.upgrade().is_none());
 assert_eq!(EVENTS.lock().unwrap().len(),2);
 println!("PASS actual owner/new/drop: post-unbind device, drained readers, failure unwind");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--ref', help='Read the tested source at this git revision')
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    source = (subprocess.check_output(['git', 'show', f'{a.ref}:{PATH}'], cwd=ROOT, text=True)
              if a.ref else (ROOT / PATH).read_text())
    bodies = '\n'.join(item(source, s) for s in [
        'pub(super) struct Telemetry', 'impl Telemetry', 'impl Drop for Telemetry'])
    # This is the real repr(C) exchange contract, not a hand-declared test layout.
    layout = '#[repr(C)]\n' + item(source, 'struct Export')
    code = PREFIX + layout + '\nconst _: () = {assert!(std::mem::size_of::<Export>()==96);assert!(std::mem::offset_of!(Export,samples)==32);};\n' + bodies + MAIN
    path = a.output / 'lifetime.rs'
    path.write_text(code)
    binary = a.output / 'lifetime'
    subprocess.run(['rustup','run','1.93.1','rustc','--edition=2021',str(path),'-C','panic=abort','-o',str(binary)], check=True)
    def no_core(): resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    run = subprocess.run([str(binary)], capture_output=True, text=True, preexec_fn=no_core)
    receipt={'ref':a.ref,'production_sha256':hashlib.sha256((layout+bodies).encode()).hexdigest(),'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr,
             'scope':'Actual production owner/constructor/destructor and repr(C) layout; typed reference-counting and sysfs drain boundary stubs, not kernel or GPU execution.'}
    (a.output/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
    if run.returncode: raise SystemExit(1)

if __name__=='__main__':main()

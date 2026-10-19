#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise production telemetry ownership, shutdown and removal against device teardown."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
PATH = "drivers/gpu/drm/asahi/g17/telemetry.rs"
GPU = "drivers/gpu/drm/asahi/g17/mod.rs"


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
use std::{ffi::{c_int,c_void},ops::Deref,sync::{Arc,Mutex,Weak,mpsc::{self,SyncSender,Receiver},atomic::{AtomicBool,AtomicPtr,AtomicUsize,Ordering}},collections::HashMap,time::Duration};
type Result<T> = std::result::Result<T,i32>;
extern crate self as kernel;
mod error { pub fn to_result(v:i32)->Result<(),i32>{if v<0{Err(v)}else{Ok(())}} }
struct Inner { deleted: AtomicBool }
struct Device { owner: Arc<Inner> }
impl Device { fn as_raw(&self)->*mut Inner {Arc::as_ptr(&self.owner).cast_mut()} }
struct ARef<T>(T);
impl<T> Deref for ARef<T>{type Target=T;fn deref(&self)->&T{&self.0}}
impl From<&Device> for ARef<Device>{fn from(v:&Device)->Self{Self(Device{owner:v.owner.clone()})}}
struct Noop;
impl Noop { fn invalidate(&self){} fn begin_shutdown(&self){} fn remove(&self){} fn shutdown(&self){} }
struct Statistics { load: Noop }
struct Stop(Mutex<()>);
impl Stop { fn lock(&self)->std::sync::MutexGuard<'_,()>{self.0.lock().unwrap()} }
struct Shared { statistics:Arc<Statistics>, stop:Stop, feed:Noop, preparations:Noop,
 poll:Noop,idle:Noop,grow:Noop,reclaim_work:Noop,work:Noop,feed_work:Noop }
impl Shared {
 fn stop_queues(&self){let _stopping=self.stop.lock();}
 fn disable_timer(&self,_:&Noop){} fn disable_work(&self,_:&Noop){}
 fn drain_reclaims(&self){} fn release_drm(&self){}
}
struct Gpu { shared:Shared, _telemetry:Option<Telemetry> }
struct Handle { device:Weak<Inner>, statistics:Weak<Statistics>, raw_dev:usize }
static CURRENT:Mutex<Option<(Weak<Inner>,Weak<Statistics>)>>=Mutex::new(None);
static ATTRS:Mutex<Option<HashMap<usize,usize>>>=Mutex::new(None);
static HANDLES:Mutex<Option<HashMap<usize,Handle>>>=Mutex::new(None);
static NEXT:AtomicUsize=AtomicUsize::new(1);
static REMOVALS:AtomicUsize=AtomicUsize::new(0);
static FAIL:AtomicBool=AtomicBool::new(false);
static BLOCK_REMOVE:Mutex<Option<(SyncSender<()>,Receiver<()>)>>=Mutex::new(None);
unsafe extern "C" fn snapshot(_data:*const c_void,_out:*mut c_void)->c_int{0}
unsafe fn asahi_telemetry_register(dev:*mut c_void,data:*const c_void,_snapshot:unsafe extern "C" fn(*const c_void,*mut c_void)->c_int,handle:*mut *mut c_void)->c_int{
 if FAIL.load(Ordering::SeqCst){return -12;}
 let current=CURRENT.lock().unwrap();let (device,statistics)=current.as_ref().unwrap();
 assert_eq!(data as usize,statistics.as_ptr() as usize);
 assert!(device.upgrade().is_some());assert!(statistics.upgrade().is_some());
 let mut attrs=ATTRS.lock().unwrap();let attrs=attrs.get_or_insert_with(HashMap::new);
 if attrs.contains_key(&(dev as usize)){return -17;}
 let id=NEXT.fetch_add(1,Ordering::SeqCst);attrs.insert(dev as usize,id);
 HANDLES.lock().unwrap().get_or_insert_with(HashMap::new).insert(id,Handle{device:device.clone(),statistics:statistics.clone(),raw_dev:dev as usize});
 unsafe{*handle=id as *mut c_void;} 0
}
unsafe fn asahi_telemetry_unregister(dev:*mut c_void,handle:*mut c_void){
 let state=HANDLES.lock().unwrap().as_mut().unwrap().remove(&(handle as usize)).expect("registration removed more than once");
 assert_eq!(state.raw_dev,dev as usize);
 let device=state.device.upgrade().expect("attribute removal used a released device");
 assert!(!device.deleted.load(Ordering::SeqCst),"late removal after device_del");
 assert!(state.statistics.upgrade().is_some(),"callback data dropped before drain");
 let block=BLOCK_REMOVE.lock().unwrap().take();
 if let Some((entered,release))=block{entered.send(()).unwrap();release.recv_timeout(Duration::from_secs(5)).unwrap();}
 let removed=ATTRS.lock().unwrap().as_mut().unwrap().remove(&(dev as usize));
 assert_eq!(removed,Some(handle as usize),"old GPU removed the replacement attribute");
 REMOVALS.fetch_add(1,Ordering::SeqCst);
}
fn stats(dev:&Device)->Arc<Statistics>{
 let s=Arc::new(Statistics{load:Noop});
 *CURRENT.lock().unwrap()=Some((Arc::downgrade(&dev.owner),Arc::downgrade(&s)));s
}
fn gpu(dev:&Device)->Arc<Gpu>{
 let statistics=stats(dev);
 let telemetry=Telemetry::new(dev,statistics.clone()).expect("rebind must recreate agx_stats");
 Arc::new(Gpu{shared:Shared{statistics,stop:Stop(Mutex::new(())),feed:Noop,preparations:Noop,poll:Noop,idle:Noop,grow:Noop,reclaim_work:Noop,work:Noop,feed_work:Noop},_telemetry:Some(telemetry)})
}
fn device()->Device{Device{owner:Arc::new(Inner{deleted:AtomicBool::new(false)})}}
fn has_attribute(dev:&Device)->bool{ATTRS.lock().unwrap().as_ref().unwrap().contains_key(&(dev.as_raw() as usize))}
'''
MAIN = r'''
fn main(){
 // Exclusive Drop still cleans up a successfully constructed standalone owner.
 let dev=device();let device_owner=Arc::downgrade(&dev.owner);
 let s=stats(&dev);let data=Arc::downgrade(&s);
 let registration=Telemetry::new(&dev,s).unwrap();drop(dev);
 assert!(device_owner.upgrade().is_some());assert!(data.upgrade().is_some());
 drop(registration);
 assert!(device_owner.upgrade().is_none());assert!(data.upgrade().is_none());
 assert_eq!(REMOVALS.load(Ordering::SeqCst),1);

 // Unbind removes the old attribute while files still retain the old GPU.
 // Its delayed destructor must not remove the replacement registration.
 let dev=device();let old=gpu(&dev);old.shutdown();assert!(!has_attribute(&dev));
 let replacement=gpu(&dev);assert!(has_attribute(&dev));
 drop(old);assert!(has_attribute(&dev));
 replacement.shutdown();assert!(!has_attribute(&dev));
 dev.owner.deleted.store(true,Ordering::SeqCst);
 let owner=Arc::downgrade(&dev.owner);drop(dev);drop(replacement);
 assert!(owner.upgrade().is_none());assert_eq!(REMOVALS.load(Ordering::SeqCst),3);

 // A second shutdown cannot return to device_del while the first is draining readers.
 let dev=device();let old=gpu(&dev);
 let (entered_tx,entered_rx)=mpsc::sync_channel(1);let (release_tx,release_rx)=mpsc::sync_channel(1);
 *BLOCK_REMOVE.lock().unwrap()=Some((entered_tx,release_rx));
 let first=old.clone();let worker=std::thread::spawn(move||first.shutdown());
 entered_rx.recv_timeout(Duration::from_secs(5)).unwrap();
 let (done_tx,done_rx)=mpsc::sync_channel(1);let second=old.clone();
 let other=std::thread::spawn(move||{second.shutdown();done_tx.send(()).unwrap();});
 assert!(matches!(done_rx.recv_timeout(Duration::from_millis(100)),Err(mpsc::RecvTimeoutError::Timeout)),"shutdown returned before the winning removal drained readers");
 release_tx.send(()).unwrap();worker.join().unwrap();other.join().unwrap();done_rx.recv().unwrap();
 assert!(!has_attribute(&dev));assert_eq!(REMOVALS.load(Ordering::SeqCst),4);
 let replacement=gpu(&dev);drop(old);assert!(has_attribute(&dev));
 replacement.shutdown();dev.owner.deleted.store(true,Ordering::SeqCst);drop(replacement);drop(dev);
 assert_eq!(REMOVALS.load(Ordering::SeqCst),5);

 // Failed construction releases both owners and never removes a nonexistent file.
 FAIL.store(true,Ordering::SeqCst);let dev=device();let s=stats(&dev);let data=Arc::downgrade(&s);
 assert!(Telemetry::new(&dev,s).is_err());assert!(data.upgrade().is_none());
 assert_eq!(Arc::strong_count(&dev.owner),1);drop(dev);
 assert_eq!(REMOVALS.load(Ordering::SeqCst),5);
 assert!(ATTRS.lock().unwrap().as_ref().unwrap().is_empty());
 assert!(HANDLES.lock().unwrap().as_ref().unwrap().is_empty());
 println!("PASS actual telemetry/Gpu shutdown bodies: owner lifetime, single removal, rebind before late Drop, device_del, concurrent drain, failure unwind");
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', required=True, type=Path)
    p.add_argument('--ref', help='Read tested production bodies at this git revision')
    p.add_argument('--mutation', choices=['skip-unbind-removal', 'no-shutdown-lock', 'no-handle-take'])
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    def read(path):
        return (subprocess.check_output(['git','show',f'{a.ref}:{path}'],cwd=ROOT,text=True)
                if a.ref else (ROOT/path).read_text())
    source, gpu_source = read(PATH), read(GPU)
    bodies = '\n'.join(item(source, s) for s in [
        'pub(super) struct Telemetry', 'impl Telemetry', 'impl Drop for Telemetry'])
    for implementation in ['unsafe impl Send for Telemetry', 'unsafe impl Sync for Telemetry']:
        if implementation in source:
            bodies += '\n' + item(source, implementation)
    shutdown = item(gpu_source, '    pub(crate) fn shutdown(&self)')
    gpu_drop = item(gpu_source, 'impl Drop for Gpu')
    if a.mutation == 'skip-unbind-removal':
        shutdown = shutdown.replace('telemetry.unregister();', 'let _ = telemetry;')
    elif a.mutation == 'no-shutdown-lock':
        shutdown = shutdown.replace('let _stopping = self.shared.stop.lock();', '')
    elif a.mutation == 'no-handle-take':
        bodies = bodies.replace('self.handle.swap(core::ptr::null_mut(), Ordering::AcqRel)',
                                'self.handle.load(Ordering::Acquire)')
    layout = '#[repr(C)]\n' + item(source, 'struct Export')
    tested = layout+bodies+shutdown+gpu_drop
    code = PREFIX + layout + '\nconst _: () = {assert!(std::mem::size_of::<Export>()==96);assert!(std::mem::offset_of!(Export,samples)==32);};\n' + bodies + '\nimpl Gpu {\n' + shutdown + '\n}\n' + gpu_drop + MAIN
    path = a.output/'lifetime.rs';path.write_text(code)
    binary = a.output/'lifetime'
    subprocess.run(['rustup','run','1.93.1','rustc','--edition=2021',str(path),'-C','panic=abort','-o',str(binary)],check=True)
    def no_core(): resource.setrlimit(resource.RLIMIT_CORE,(0,0))
    run = subprocess.run([str(binary)],capture_output=True,text=True,preexec_fn=no_core,timeout=20)
    receipt={'ref':a.ref,'mutation':a.mutation,'production_sha256':hashlib.sha256(tested.encode()).hexdigest(),'exit':run.returncode,'stdout':run.stdout,'stderr':run.stderr,
             'scope':'Actual production telemetry owner/new/unregister/drop, Gpu::shutdown/drop and repr(C) layout; typed reference-counting, Shared services and sysfs drain boundaries, not kernel or GPU execution.'}
    (a.output/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))
    if run.returncode: raise SystemExit(1)

if __name__=='__main__':main()

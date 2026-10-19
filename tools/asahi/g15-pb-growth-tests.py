#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Run the G15 PB growth answer's actual bodies against a firmware model.

The harness extracts Firmware::answer_growth(), Firmware::answer_request() and Pools::soft_max()
from drivers/gpu/drm/asahi-m3/g15/pools.rs, pb_max_blocks() from params.rs, the PB sizes and
tuning from hw.rs and the parameter's default from asahi_core.rs. It includes pools/growth.rs,
fw/buffer.rs and fw/channels.rs whole. The bodies run against a model of the firmware objects
that logs every block record, barrier, producer store, DEVCTL reply and log line in order. The
model's DEVCTL wait may queue the firmware's next request, as the real wait drains the rings.

What it shows:
- a poll pass answers only the requests queued when it starts, in order; one the reply's wait
  queues goes to the next pass. A request that cannot be processed does not end the pass;
- from the driver's initial size to its maximum, every block record is written once at its
  offset, each producer store follows its records and a barrier, each reply follows its producer
  store and a barrier, and every producer is in 1..=4095, ending at 4095;
- at the maximum two refusals are answered; further requests get no reply, count as bad and
  warn once until the ledger is made again, as init() does;
- the soft maximum is the parameter clamped to 64..=4095 and then to the ledger's
  initial..=max; growth answers above it are logged at info level, and the one that reaches it
  exactly is not;
- invalid requests (type, slot, stale counter, epoch, no live render) get no reply;
- Ledger::new() is Ledger::new() as of dba2a110de00 (the carried upstream constructor), capped at
  4095, over every growth and fixed input in 0..=4200 and at the u32 extremes. The driver's
  values are unchanged.

Not shown: kernel, firmware or GPU execution, timing, or what the firmware does with a reply, a
refusal or an unanswered request. Recovery after an unanswered request is not modelled.

With --mutations, each mutation below must fail its intended test with the expected message, and
the model-only control (a driver maximum of the whole 4096-block heap) must pass, held by the
ledger's cap. This control bypasses pools.rs static assertions; it is not a supported kernel tuning.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

ROOT = Path(__file__).resolve().parents[2]
G15 = "drivers/gpu/drm/asahi-m3/g15"
POOLS = f"{G15}/pools.rs"
GROWTH = f"{G15}/pools/growth.rs"
BUFFER = f"{G15}/fw/buffer.rs"
CHANNELS = f"{G15}/fw/channels.rs"
INITDATA = f"{G15}/fw/initdata.rs"
PARAMS = f"{G15}/params.rs"
HW = f"{G15}/hw.rs"
CORE = "drivers/gpu/drm/asahi-m3/asahi_core.rs"
OLD_REF = "dba2a110de00743eeecd41edf23e54ede4491d5e"

# Ledger::new() at OLD_REF, the tip of the four PR363 carries (upstream 027bd7623f37), verbatim:
# the constructor before the cap below K.
OLD_LEDGER_NEW = '''    pub(crate) const fn new(grow: bool, blocks: u32, max_blocks: u32) -> Self {
        let (initial, max) = if grow {
            let nb = clamp(blocks, MIN_BLOCKS, MAX_BLOCKS);
            (nb, clamp(max_blocks, nb, MAX_BLOCKS))
        } else {
            (MAX_BLOCKS, MAX_BLOCKS)
        };
        Self {
            initial,
            max,
            posted: initial,
            next_request: 0,
            refusal_logged: false,
            refusals: 0,
        }
    }'''

# The two statements of Pools::init() that start a new ledger.
LEDGER_RESET = ['self.ledger = Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);',
                'self.exhausted_logged = false;']

MUTATIONS = {
    # Answer until the queue is empty, including requests queued by the replies' waits.
    'drain-until-empty': (POOLS, 'for _ in 0..queued {', 'while !self.events.grow.is_empty() {'),
    # Leave the last request queued before the pass for later.
    'skip-last-queued': (POOLS, 'for _ in 0..queued {', 'for _ in 0..queued.saturating_sub(1) {'),
    # End the pass at a request that cannot be processed.
    'return-after-request-error': (POOLS, '                self.pools.stats.bad += 1;\n                dev_err!(',
                                   '                self.pools.stats.bad += 1;\n                return Ok(());\n'
                                   '                dev_err!('),
    # Answer three refusals at the maximum.
    'three-refusals': (GROWTH, 'pub(crate) const MAX_REFUSALS: u32 = 2;', 'pub(crate) const MAX_REFUSALS: u32 = 3;'),
    # Answer refusals for ever.
    'never-exhausted': (GROWTH, 'self.refusals >= MAX_REFUSALS', 'false && self.refusals >= MAX_REFUSALS'),
    # Warn for every unanswered request.
    'warn-every-time': (POOLS, 'if !self.pools.exhausted_logged {', 'if true {'),
    # Store the producer without the barrier after the block records.
    'no-record-barrier': (POOLS, '            // Records before their producer.\n            crate::mem::sync();\n',
                          '            // Records before their producer.\n'),
    # Reply without the barrier after the producer store.
    'no-reply-barrier': (POOLS, '            // Producer before the reply.\n            crate::mem::sync();\n',
                         '            // Producer before the reply.\n'),
    # Log the answer that reaches the soft maximum exactly.
    'soft-boundary-inclusive': (POOLS, 'let past_soft = n != 0 && ledger.posted > soft;',
                                'let past_soft = n != 0 && ledger.posted >= soft;'),
    # Give the firmware the parameter without the ledger's bounds.
    'soft-max-unclamped': (POOLS, 'super::params::pb_max_blocks().clamp(self.ledger.initial, self.ledger.max)',
                           'super::params::pb_max_blocks()'),
    # Ledger::new() without the cap below K.
    'uncapped-ledger': (GROWTH, 'let nb = clamp(blocks, MIN_BLOCKS, MAX_POSTED);\n'
                                '            (nb, clamp(max_blocks, nb, MAX_POSTED))\n'
                                '        } else {\n            (MAX_POSTED, MAX_POSTED)',
                        'let nb = clamp(blocks, MIN_BLOCKS, MAX_BLOCKS);\n'
                        '            (nb, clamp(max_blocks, nb, MAX_BLOCKS))\n'
                        '        } else {\n            (MAX_BLOCKS, MAX_BLOCKS)'),
}

# The test each mutation must fail, and a piece of its failure message.
INTENDED = {
    'drain-until-empty': ('queued_snapshot_keeps_order', 'unbounded answering in one poll pass'),
    'skip-last-queued': ('queued_snapshot_keeps_order', 'replies in the first pass'),
    'return-after-request-error': ('unprocessed_request_does_not_end_the_pass', 'requests taken in the pass'),
    'three-refusals': ('default_growth_to_exhaustion', 'want Some(Log("warn"'),
    'never-exhausted': ('default_growth_to_exhaustion', 'growth did not end'),
    'warn-every-time': ('default_growth_to_exhaustion', 'later request after exhaustion'),
    'no-record-barrier': ('default_growth_to_exhaustion', 'got Some(Producer(1034)), want Some(Sync)'),
    'no-reply-barrier': ('default_growth_to_exhaustion',
                         'got Some(Reply([7, 1, 0, 5, 0, 256, 3, 1, 0, 0, 0, 0, 0, 0])), want Some(Sync)'),
    'soft-boundary-inclusive': ('soft_max_boundaries', 'got Some(Log("info", "")), want Some(Log("dbg", ""))'),
    'soft-max-unclamped': ('soft_max_bounds', 'soft_max, param 0'),
    'uncapped-ledger': ('constructor_is_the_old_one_capped', 'new(true, '),
}
assert set(INTENDED) == set(MUTATIONS)

# Changes that must pass: the ledger's cap holds a driver maximum of the whole heap.
CONTROLS = {
    'tuning-whole-heap': (HW, 'pub(crate) const PB_MAX_BLOCKS: u32 = super::sizes::PB_BLOCKS - 1;',
                          'pub(crate) const PB_MAX_BLOCKS: u32 = super::sizes::PB_BLOCKS;'),
}


def item(text, start):
    a = text.index(start)
    b = text.index("{", a) + 1
    depth = 1
    while depth:
        depth += (text[b] == "{") - (text[b] == "}")
        b += 1
    return text[a:b]


def consts(text, names):
    out = []
    for name in names:
        m = re.search(rf'pub\(crate\) const {name}: (\w+) = ([^;]+);', text)
        if not m:
            raise SystemExit(f'{name} not found')
        out.append(f'pub const {name}: {m.group(1)} = {m.group(2)};')
    return '\n'.join(out)


PREFIX = r'''
#![allow(dead_code, unused_imports, unused_macros, unused_variables, unreachable_code, non_upper_case_globals)]
extern crate self as kernel;
use std::cell::{Cell, RefCell};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(i32);
impl Error {
    pub fn to_errno(&self) -> i32 {
        self.0
    }
}
pub type Result<T = ()> = core::result::Result<T, Error>;
pub const EINVAL: Error = Error(-22);
pub const ENODEV: Error = Error(-19);
pub const EIO: Error = Error(-5);
pub mod prelude {
    pub use crate::{static_assert, Error, Result, EINVAL, EIO, ENODEV};
}
#[macro_export]
macro_rules! static_assert {
    ($e:expr) => {
        const _: () = assert!($e);
    };
}

/// One firmware-visible access, barrier or log line, in program order.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Ev {
    Record(usize, u64),
    Sync,
    Producer(u32),
    Reply([u32; 14]),
    Log(&'static str, String),
}
thread_local! {
    pub static LOG: RefCell<Vec<Ev>> = RefCell::new(Vec::new());
    pub static PARAM: Cell<u32> = Cell::new(PARAM_DEFAULT);
}
pub fn ev(e: Ev) {
    LOG.with(|l| l.borrow_mut().push(e));
}
pub fn take() -> Vec<Ev> {
    LOG.with(|l| std::mem::take(&mut *l.borrow_mut()))
}
macro_rules! dev_log {
    ($lvl:expr, $dev:expr, $($arg:tt)*) => {{
        let _ = $dev;
        crate::ev(crate::Ev::Log($lvl, format!($($arg)*)));
    }};
}
macro_rules! dev_err { ($($t:tt)*) => { dev_log!("err", $($t)*) }; }
macro_rules! dev_warn { ($($t:tt)*) => { dev_log!("warn", $($t)*) }; }
macro_rules! dev_info { ($($t:tt)*) => { dev_log!("info", $($t)*) }; }
macro_rules! dev_dbg { ($($t:tt)*) => { dev_log!("dbg", $($t)*) }; }

pub mod mem {
    pub fn sync() {
        crate::ev(crate::Ev::Sync);
    }
}

/// A boot parameter: read-only after load.
pub mod module_parameters {
    pub struct Param;
    impl Param {
        pub fn value(&self) -> &'static u32 {
            Box::leak(Box::new(crate::PARAM.with(|p| p.get())))
        }
    }
    pub static g15_pb_max_blocks: Param = Param;
}
'''

MODEL = r'''
        pub struct Ctl {
            pub epoch: u64,
        }
        impl Ctl {
            pub fn r32(&self, offset: usize) -> Result<u32> {
                if offset == control::HALT_COUNT {
                    Ok(self.epoch as u32)
                } else if offset == control::HALT_COUNT + 4 {
                    Ok((self.epoch >> 32) as u32)
                } else {
                    panic!("unexpected control read {offset:#x}")
                }
            }
        }
        pub struct Init {
            pub control: Option<std::sync::Arc<Ctl>>,
        }
        pub struct Vm {
            pub slot: u32,
        }
        pub struct Job {
            pub vm: Vm,
        }
        pub struct Flight {
            pub job: Job,
        }
        pub struct Engine {
            pub flights: Vec<Flight>,
        }
        pub struct Receipts {
            pub renders: u32,
        }
        /// A firmware object: the block list, block_ctl or the heap.
        pub struct FwObject {
            pub gva: Option<u64>,
            pub size: usize,
            /// Writes that fail from now on.
            pub failures: Cell<u32>,
        }
        impl FwObject {
            pub fn w64(&self, offset: usize, value: u64) -> Result {
                assert!(offset % 8 == 0 && offset + 8 <= self.size, "record write at {offset:#x}");
                if self.failures.get() != 0 {
                    self.failures.set(self.failures.get() - 1);
                    return Err(EIO);
                }
                crate::ev(crate::Ev::Record(offset, value));
                Ok(())
            }
            pub fn w32(&self, offset: usize, value: u32) -> Result {
                assert_eq!(offset, block_ctl::PRODUCER);
                assert!(offset + 4 <= self.size);
                crate::ev(crate::Ev::Producer(value));
                Ok(())
            }
        }
        fn gva(object: &FwObject) -> Result<u64> {
            object.gva.ok_or(EINVAL)
        }
        #[derive(Default)]
        pub struct PbStats {
            pub requests: u64,
            pub grown: u64,
            pub refused: u64,
            pub bad: u64,
        }
        pub struct Live {
            pub block_ctl: FwObject,
        }
        pub struct Pools {
            pub ledger: Ledger,
            pub exhausted_logged: bool,
            pub stats: PbStats,
            pub pb_heap: FwObject,
            pub pb_blocks: FwObject,
            pub live: Live,
        }
        pub struct Queue(pub std::collections::VecDeque<[u32; 14]>);
        impl Queue {
            pub fn len(&self) -> usize {
                self.0.len()
            }
            pub fn is_empty(&self) -> bool {
                self.0.is_empty()
            }
            pub fn remove(&mut self, i: usize) -> core::result::Result<[u32; 14], ()> {
                assert_eq!(i, 0);
                self.0.pop_front().ok_or(())
            }
        }
        pub struct Events {
            pub grow: Queue,
        }
        /// The firmware side: its requests, and whether the next one arrives during a reply.
        pub struct FwModel {
            pub ctx: u32,
            pub epoch: u64,
            pub next_counter: u32,
            pub ask_after_reply: bool,
            pub reply_fails: bool,
            pub replies_this_pass: u32,
        }
        impl FwModel {
            pub fn request(&mut self) -> [u32; 14] {
                let mut q = [0u32; 14];
                q[buffer::grow_request::TYPE] = 7;
                q[buffer::grow_request::CONTEXT] = self.ctx;
                q[buffer::grow_request::SLOT] = 0;
                q[buffer::grow_request::COUNTER] = self.next_counter;
                q[buffer::grow_request::POSITION] = 0x100 + self.next_counter;
                q[buffer::grow_request::EPOCH] = self.epoch as u32;
                q[buffer::grow_request::EPOCH + 1] = (self.epoch >> 32) as u32;
                self.next_counter += 1;
                q
            }
        }
        pub struct Firmware {
            pub events: Events,
            pub pools: Pools,
            pub init: Init,
            pub engine: Engine,
            pub renders: Vec<u32>,
            pub fw: FwModel,
        }
        pub const HEAP: u64 = super::hw::client::PB_HEAP;
        impl Firmware {
            pub fn new() -> Self {
                Firmware {
                    events: Events { grow: Queue(Default::default()) },
                    pools: Pools {
                        ledger: Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS),
                        exhausted_logged: false,
                        stats: PbStats::default(),
                        pb_heap: FwObject { gva: Some(HEAP), size: 0, failures: Cell::new(0) },
                        pb_blocks: FwObject {
                            gva: Some(super::hw::client::PB_BLOCK_LIST),
                            size: buffer::BLOCK_LIST_SIZE,
                            failures: Cell::new(0),
                        },
                        live: Live {
                            block_ctl: FwObject { gva: None, size: block_ctl::SIZE, failures: Cell::new(0) },
                        },
                    },
                    init: Init { control: Some(std::sync::Arc::new(Ctl { epoch: 0x1_0000_0003 })) },
                    engine: Engine { flights: vec![Flight { job: Job { vm: Vm { slot: 5 } } }] },
                    renders: vec![1],
                    fw: FwModel {
                        ctx: 5,
                        epoch: 0x1_0000_0003,
                        next_counter: 0,
                        ask_after_reply: false,
                        reply_fails: false,
                        replies_this_pass: 0,
                    },
                }
            }
            pub fn dev(&self) -> &() {
                &()
            }
            pub fn receipts(&self, i: usize) -> Receipts {
                Receipts { renders: self.renders[i] }
            }
            pub fn devctl(&mut self, reply: &DevCtl, wait: bool) -> Result {
                assert!(wait);
                crate::ev(crate::Ev::Reply(reply.words));
                self.fw.replies_this_pass += 1;
                assert!(self.fw.replies_this_pass <= 64, "unbounded answering in one poll pass");
                // The reply's wait drains the rings: the firmware's next request may queue now.
                if self.fw.ask_after_reply {
                    let q = self.fw.request();
                    self.events.grow.0.push_back(q);
                }
                if self.fw.reply_fails {
                    Err(EIO)
                } else {
                    Ok(())
                }
            }
            pub fn queue(&mut self) {
                let q = self.fw.request();
                self.events.grow.0.push_back(q);
            }
            /// One poll pass.
            pub fn pass(&mut self) {
                self.fw.replies_this_pass = 0;
                assert_eq!(self.answer_growth(), Ok(()));
            }
            /// What Pools::init() does to the ledger.
            pub fn reset_ledger(&mut self) {
                let p = &mut self.pools;
                p.RESET
            }
        }
'''

TESTS = r'''
#[cfg(test)]
mod pb_growth_tests {
    use crate::g15::firmware::{Firmware, HEAP};
    use crate::g15::fw::buffer::{self, grow_reply, BLOCK_LIST_RECORDS, GROW_STEP};
    use crate::g15::fw::channels::devctl;
    use crate::g15::hw::tuning;
    use crate::g15::pools::growth::Ledger;
    use crate::g15::{params, OldLedger};
    use crate::{take, Ev, PARAM, PARAM_DEFAULT};

    const CAP: u32 = BLOCK_LIST_RECORDS - 1;
    /// Refusals answered at the maximum, as the commit states them.
    const REFUSALS: u32 = 2;

    fn clamp(v: u32, lo: u32, hi: u32) -> u32 {
        v.max(lo).min(hi)
    }

    fn soft(param: u32, initial: u32, max: u32) -> u32 {
        clamp(clamp(param, 64, tuning::PB_MAX_BLOCKS), initial, max)
    }

    /// Event logs compared with a short message at the first difference.
    fn same(got: &[Ev], want: &[Ev]) {
        if let Some(i) = (0..got.len().max(want.len())).find(|&i| got.get(i) != want.get(i)) {
            panic!("event {i} of {}/{}: got {:?}, want {:?}", got.len(), want.len(), got.get(i), want.get(i));
        }
    }

    fn level(e: &Ev) -> Option<&'static str> {
        match e {
            Ev::Log(l, _) => Some(l),
            _ => None,
        }
    }

    /// The accesses, replies and log levels a ledger at `posted` gives for the firmware's
    /// requests from counter 0 on, if it keeps asking after every reply: growth to the maximum,
    /// MAX_REFUSALS refusals, then one unanswered request.
    fn expected(initial: u32, max: u32, soft_max: u32) -> (Vec<Ev>, u32, u32) {
        let mut out = Vec::new();
        let (mut posted, mut refusals, mut logged_refusal) = (initial, 0, false);
        let mut counter = 0;
        loop {
            if refusals >= REFUSALS {
                out.push(Ev::Log("warn", String::new()));
                break;
            }
            let n = GROW_STEP.min(max - posted);
            for b in posted..posted + n {
                out.push(Ev::Record(8 * b as usize, buffer::block_record(HEAP, b) as u64));
            }
            if n != 0 {
                out.push(Ev::Sync);
                out.push(Ev::Producer(posted + n));
                out.push(Ev::Sync);
            }
            let mut r = [0u32; 14];
            r[grow_reply::OPCODE] = devctl::GROW_REPLY;
            r[grow_reply::GREW] = (n != 0) as u32;
            r[grow_reply::CONTEXT] = 5;
            r[grow_reply::COUNTER] = counter;
            r[grow_reply::POSITION] = 0x100 + counter;
            r[grow_reply::EPOCH] = 3;
            r[grow_reply::EPOCH + 1] = 1;
            out.push(Ev::Reply(r));
            posted += n;
            let first_refusal = n == 0 && !logged_refusal;
            if n == 0 {
                refusals += 1;
                logged_refusal = true;
            }
            let info = counter < 2 || (n != 0 && posted > soft_max) || first_refusal;
            out.push(Ev::Log(if info { "info" } else { "dbg" }, String::new()));
            counter += 1;
        }
        (out, posted, counter + 1)
    }

    fn levels_only(evs: Vec<Ev>) -> Vec<Ev> {
        evs.into_iter()
            .map(|e| match e {
                Ev::Log(l, _) => Ev::Log(l, String::new()),
                e => e,
            })
            .collect()
    }

    /// Growth to the maximum with the firmware asking after every reply, one pass at a time.
    fn run_to_exhaustion(param: u32) {
        PARAM.with(|p| p.set(param));
        let mut f = Firmware::new();
        let (initial, max) = (f.pools.ledger.initial, f.pools.ledger.max);
        assert_eq!(f.pools.soft_max(), soft(param, initial, max));
        f.fw.ask_after_reply = true;
        f.queue();
        take();
        let mut passes = 0;
        while !f.events.grow.is_empty() {
            let queued = f.events.grow.len();
            let before = f.pools.stats.requests;
            f.pass();
            passes += 1;
            // Exactly the requests queued before the pass were taken.
            assert_eq!(f.pools.stats.requests - before, queued as u64);
            assert!(passes <= 1000, "growth did not end");
        }
        let log = take();
        let (want, posted, requests) = expected(initial, max, soft(param, initial, max));
        same(&levels_only(log.clone()), &want);
        assert_eq!(passes, requests);
        assert_eq!(f.pools.ledger.posted, posted);
        assert_eq!(posted, max);
        // Every producer store: increasing, in 1..=4095, the last one the maximum.
        let producers: Vec<u32> = log
            .iter()
            .filter_map(|e| if let Ev::Producer(p) = e { Some(*p) } else { None })
            .collect();
        assert!(producers.windows(2).all(|w| w[0] < w[1]));
        assert!(producers.iter().all(|p| (1..=CAP).contains(p)));
        assert_eq!(*producers.last().unwrap(), max);
        // Every block above the initial ones was written once, below the list's end.
        let records: Vec<usize> = log
            .iter()
            .filter_map(|e| if let Ev::Record(o, _) = e { Some(*o) } else { None })
            .collect();
        assert_eq!(records.len() as u32, max - initial);
        assert!(records.iter().all(|o| o + 8 <= buffer::BLOCK_LIST_SIZE));
        let s = &f.pools.stats;
        assert_eq!((s.grown, s.refused, s.bad), ((max - initial) as u64, REFUSALS as u64, 1));
        let warn: Vec<&String> = log
            .iter()
            .filter_map(|e| if let Ev::Log("warn", m) = e { Some(m) } else { None })
            .collect();
        assert_eq!(warn.len(), 1);
        assert!(warn[0].contains(&format!("left unanswered: {REFUSALS} refusals at the maximum ({max} blocks)")));
        // A later request: no reply, counted, no second warning.
        f.queue();
        f.pass();
        let log = take();
        assert!(log.is_empty(), "later request after exhaustion: {log:?}");
        assert_eq!(f.pools.stats.bad, 2);
        // A new ledger, as Pools::init() makes, answers again.
        f.reset_ledger();
        f.queue();
        f.pass();
        let log = take();
        assert_eq!(log.iter().filter(|e| matches!(e, Ev::Producer(_))).count(), 1);
        assert!(log.contains(&Ev::Producer(initial + GROW_STEP.min(max - initial))));
    }

    #[test]
    fn driver_values() {
        let l = Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);
        assert!(tuning::PB_GROW);
        assert_eq!((l.initial, l.max, l.posted), (1024, 4095, 1024));
        assert_eq!(PARAM_DEFAULT, 3072);
        PARAM.with(|p| p.set(PARAM_DEFAULT));
        assert_eq!(params::pb_max_blocks(), 3072);
        assert_eq!(Firmware::new().pools.soft_max(), 3072);
    }

    #[test]
    fn soft_max_bounds() {
        let f = Firmware::new();
        for param in [0, 1, 63, 64, 65, 1023, 1024, 1025, 3071, 3072, 3073, 4094, 4095, 4096, 8192, u32::MAX] {
            PARAM.with(|p| p.set(param));
            assert_eq!(params::pb_max_blocks(), clamp(param, 64, tuning::PB_MAX_BLOCKS), "pb_max_blocks, param {param}");
            // Below the initial 1024 the firmware still gets 1024.
            assert_eq!(f.pools.soft_max(), clamp(param, 1024, CAP), "soft_max, param {param}");
        }
    }

    #[test]
    fn default_growth_to_exhaustion() {
        run_to_exhaustion(PARAM_DEFAULT);
    }

    #[test]
    fn soft_max_boundaries() {
        // 3074 is reached exactly (1024 + 205 * 10): that answer is not past it.
        for param in [0, 1024, 3072, 3074, 3075, 4094, 4095, u32::MAX] {
            run_to_exhaustion(param);
        }
    }

    #[test]
    fn queued_snapshot_keeps_order() {
        let mut f = Firmware::new();
        f.fw.ask_after_reply = true;
        f.queue();
        f.queue();
        take();
        f.pass();
        let log = take();
        let replies: Vec<u32> = log
            .iter()
            .filter_map(|e| if let Ev::Reply(r) = e { Some(r[grow_reply::COUNTER]) } else { None })
            .collect();
        assert_eq!(replies, vec![0, 1], "replies in the first pass");
        // Compare both requests' complete record/barrier/producer/reply/log sequences,
        // not just their reply order. Each request finishes before the next one's records.
        let (want, _, _) = expected(f.pools.ledger.initial, f.pools.ledger.max, f.pools.soft_max());
        let end = want.iter().position(|e| matches!(e, Ev::Reply(r) if r[grow_reply::COUNTER] == 1))
            .expect("second expected reply") + 2; // Include its log too.
        same(&levels_only(log), &want[..end]);
        let queued: Vec<u32> = f.events.grow.0.iter().map(|q| q[buffer::grow_request::COUNTER]).collect();
        assert_eq!(queued, vec![2, 3]);
        assert_eq!(f.pools.stats.requests, 2);
        f.pass();
        assert_eq!(f.pools.stats.requests, 4);
        assert_eq!(f.events.grow.len(), 2);
    }

    #[test]
    fn empty_queue_does_nothing() {
        let mut f = Firmware::new();
        take();
        f.pass();
        assert!(take().is_empty());
        assert_eq!(f.pools.stats.requests, 0);
    }

    #[test]
    fn unprocessed_request_does_not_end_the_pass() {
        let mut f = Firmware::new();
        f.queue();
        f.queue();
        f.pools.pb_blocks.failures.set(1);
        take();
        f.pass();
        let log = take();
        // The second request was answered in the same pass, from the same posted count.
        assert_eq!(f.pools.stats.requests, 2, "requests taken in the pass");
        assert_eq!(f.pools.stats.bad, 1);
        assert!(log.iter().any(|e| matches!(e, Ev::Log("err", m) if m.contains("not answered: -5"))));
        assert!(log.contains(&Ev::Producer(1024 + GROW_STEP)));
        assert!(f.events.grow.is_empty());
    }

    #[test]
    fn failed_reply_is_logged_and_counted_as_grown() {
        let mut f = Firmware::new();
        f.fw.reply_fails = true;
        f.queue();
        f.queue();
        f.queue();
        take();
        f.pass();
        let log = take();
        assert_eq!(f.pools.ledger.posted, 1024 + 3 * GROW_STEP);
        assert_eq!((f.pools.stats.grown, f.pools.stats.bad), (3 * GROW_STEP as u64, 0));
        // Each failed reply is logged at info level.
        let info = log.iter().filter(|e| matches!(e, Ev::Log("info", m) if m.contains("DEVCTL 7: -5"))).count();
        assert_eq!(info, 3);
    }

    #[test]
    fn invalid_requests_get_no_reply() {
        let mut f = Firmware::new();
        let good = f.fw.request();
        let mut cases = Vec::new();
        let mut q = good;
        q[buffer::grow_request::TYPE] = 6;
        cases.push(q);
        let mut q = good;
        q[buffer::grow_request::SLOT] = 1;
        cases.push(q);
        let mut q = good;
        q[buffer::grow_request::EPOCH] = 2;
        cases.push(q);
        let mut q = good;
        q[buffer::grow_request::CONTEXT] = 6;
        cases.push(q);
        for q in cases {
            f.events.grow.0.push_back(q);
        }
        take();
        f.pass();
        let log = take();
        assert!(log.iter().all(|e| matches!(e, Ev::Log("warn", _))), "{log:?}");
        assert_eq!(log.len(), 4);
        assert_eq!(f.pools.stats.bad, 4);
        assert_eq!(f.pools.ledger, Ledger::new(tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS));
        // No render published: not live.
        f.renders[0] = 0;
        f.events.grow.0.push_back(good);
        f.pass();
        assert!(!take().iter().any(|e| matches!(e, Ev::Reply(_))));
        // A stale counter after an answer.
        f.renders[0] = 1;
        f.events.grow.0.push_back(good);
        f.pass();
        assert!(take().iter().any(|e| matches!(e, Ev::Reply(_))));
        f.events.grow.0.push_back(good);
        f.pass();
        assert!(!take().iter().any(|e| matches!(e, Ev::Reply(_))));
        assert_eq!(f.pools.stats.bad, 6);
    }

    #[test]
    fn constructor_is_the_old_one_capped() {
        let mut values: Vec<u32> = (0..=4200).collect();
        values.extend([8191, 8192, u32::MAX - 1, u32::MAX]);
        let mut changed = 0u64;
        let mut cases = 0u64;
        for grow in [true, false] {
            for &blocks in &values {
                for &max_blocks in &values {
                    let old = OldLedger::new(grow, blocks, max_blocks);
                    let new = Ledger::new(grow, blocks, max_blocks);
                    cases += 1;
                    assert_eq!(
                        (new.initial, new.max, new.posted),
                        (old.initial.min(CAP), old.max.min(CAP), old.posted.min(CAP)),
                        "new({grow}, {blocks}, {max_blocks})"
                    );
                    assert!((1..=CAP).contains(&(new.posted % BLOCK_LIST_RECORDS)));
                    if old.max <= CAP {
                        assert_eq!((new.initial, new.max), (old.initial, old.max));
                    } else {
                        changed += 1;
                    }
                }
            }
        }
        // Only inputs the old constructor took to the whole 4096-block heap changed.
        assert!(changed > 0 && changed < cases);
        // The driver's values are below the cap, so its ledger is the old one.
        let d = (tuning::PB_GROW, tuning::PB_BLOCKS, tuning::PB_MAX_BLOCKS);
        let (old, new) = (OldLedger::new(d.0, d.1, d.2), Ledger::new(d.0, d.1, d.2));
        if d.2 <= CAP {
            assert_eq!((old.initial, old.max, old.posted), (new.initial, new.max, new.posted));
        }
        println!("constructor cases {cases}, changed {changed}");
    }
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument('--output', type=Path, help='Work directory (default: a temporary one)')
    p.add_argument('--ref', help='Read the tested bodies at this git revision')
    p.add_argument('--mutation', choices=sorted(MUTATIONS) + sorted(CONTROLS))
    p.add_argument('--mutations', action='store_true',
                   help='Also require every mutation to fail and every control to pass')
    a = p.parse_args()
    import tempfile
    out = (a.output or Path(tempfile.mkdtemp(prefix='g15-pb-growth-'))).resolve()
    out.mkdir(parents=True, exist_ok=True)

    def read(path, ref=None):
        ref = ref or a.ref
        return (subprocess.check_output(['git', 'show', f'{ref}:{path}'], cwd=ROOT, text=True,
                                        stderr=subprocess.DEVNULL)
                if ref else (ROOT / path).read_text())

    # The embedded reference is the commit's own text, when the commit is here.
    try:
        old_git = item(read(GROWTH, OLD_REF), '    pub(crate) const fn new(')
    except (subprocess.CalledProcessError, ValueError):
        old_git = None
    if old_git is not None and old_git != OLD_LEDGER_NEW:
        raise SystemExit('embedded Ledger::new() differs from ' + OLD_REF)

    def run(mutation):
        paths = (POOLS, GROWTH, BUFFER, CHANNELS, INITDATA, PARAMS, HW, CORE)
        src = {path: read(path) for path in paths}
        if mutation:
            path, old, new = {**MUTATIONS, **CONTROLS}[mutation]
            if src[path].count(old) != 1:
                raise SystemExit(f'mutation {mutation}: anchor not found once')
            src[path] = src[path].replace(old, new)
        pools, hw = src[POOLS], src[HW]
        bodies = [item(pools, s) for s in ('    pub(crate) fn answer_growth(&mut self) -> Result {',
                                           '    fn answer_request(&mut self, request: &[u32; 14]) -> Result {')]
        soft_max = item(pools, '    pub(crate) fn soft_max(&self) -> u32 {')
        init = item(pools, '    fn init(&mut self) -> Result {')
        for line in LEDGER_RESET:
            if init.count(line) != 1:
                raise SystemExit('Pools::init() does not hold: ' + line)
        reset = '\n'.join('p.' + line[len('self.'):] for line in LEDGER_RESET)
        pb_max_blocks = item(src[PARAMS], 'pub(crate) fn pb_max_blocks() -> u32 {')
        m = re.search(r'g15_pb_max_blocks: u32 \{\s*default: (\d+),', src[CORE])
        if not m:
            raise SystemExit('g15_pb_max_blocks default not found')
        param_default = int(m.group(1))
        sizes = consts(item(hw, 'pub(crate) mod sizes {'), ['PB_BLOCKS'])
        tuning = consts(item(hw, 'pub(crate) mod tuning {'), ['PB_GROW', 'PB_BLOCKS', 'PB_MAX_BLOCKS'])
        client = consts(item(hw, 'pub(crate) mod client {'), ['PB_HEAP', 'PB_BLOCK_LIST'])
        control = consts(item(src[INITDATA], 'pub(crate) mod control {'), ['HALT_COUNT'])
        growth = src[GROWTH]
        clamp_fn = item(growth, 'const fn clamp(v: u32, lo: u32, hi: u32) -> u32 {')
        tested = ''.join(bodies) + soft_max + pb_max_blocks + growth + src[BUFFER] + sizes + tuning
        dir = out / (mutation or 'actual')
        dir.mkdir(exist_ok=True)
        for path in (GROWTH, BUFFER, CHANNELS):
            (dir / Path(path).name).write_text(src[path])
        code = (PREFIX + f'pub const PARAM_DEFAULT: u32 = {param_default};\n' +
                'pub mod g15 {\n'
                '    pub mod fw {\n'
                f'        #[path = "{dir / "channels.rs"}"] pub mod channels;\n'
                f'        #[path = "{dir / "buffer.rs"}"] pub mod buffer;\n'
                '        pub mod initdata { pub mod control {\n' + control + '\n} }\n'
                '    }\n'
                f'    pub mod pools {{ #[path = "{dir / "growth.rs"}"] pub mod growth; }}\n'
                '    pub mod hw {\n'
                '        pub mod sizes {\n' + sizes + '\n}\n'
                '        pub mod tuning {\n' + tuning + '\n}\n'
                '        pub mod client {\n' + client + '\n}\n'
                '    }\n'
                '    pub mod params {\n        use crate::module_parameters;\n' + pb_max_blocks + '\n    }\n'
                '    /// Ledger::new() at ' + OLD_REF[:12] + '.\n'
                '    pub struct OldLedger { pub initial: u32, pub max: u32, pub posted: u32, pub next_request: u32,\n'
                '                           pub refusal_logged: bool, pub refusals: u32 }\n'
                '    use crate::g15::fw::buffer::{MAX_BLOCKS, MIN_BLOCKS};\n'
                '    impl OldLedger {\n' + OLD_LEDGER_NEW + '\n    }\n' + clamp_fn + '\n'
                '    pub mod firmware {\n'
                '        use crate::prelude::*;\n'
                '        use std::cell::Cell;\n'
                '        use super::fw::buffer::{self, block_ctl};\n'
                '        use super::fw::channels::DevCtl;\n'
                '        use super::fw::initdata::control;\n'
                '        use super::hw::{client, tuning};\n'
                '        use super::pools::growth::{self, Ledger};\n' +
                MODEL.replace('p.RESET', reset) +
                '        impl Pools {\n' + soft_max + '\n        }\n'
                '        impl Firmware {\n' + '\n'.join(bodies) + '\n        }\n'
                '    }\n'
                '}\n' + TESTS)
        path = dir / 'growth-answer.rs'
        path.write_text(code)
        binary = dir / 'growth-answer'
        build = subprocess.run(['rustup', 'run', '1.93.1', 'rustc', '--edition=2021', '--test', '-O',
                                '-C', 'overflow-checks=on', str(path), '-o', str(binary)],
                               capture_output=True, text=True)
        if build.returncode != 0:
            raise SystemExit(f'{mutation or "actual"}: build failed\n{build.stderr[-3000:]}')

        def no_core():
            resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        r = subprocess.run([str(binary), '--show-output', 'pb_growth_tests::'], capture_output=True,
                           text=True, preexec_fn=no_core, timeout=600)
        summary = [line for line in r.stdout.splitlines() if line.startswith('test result:')]
        failed = sorted(line.split()[1].removeprefix('pb_growth_tests::') for line in r.stdout.splitlines()
                        if line.startswith('test ') and line.endswith('FAILED'))
        # Each test's captured output: the panic message of a failure, the counts of a success.
        output = {m.group(1).removeprefix('pb_growth_tests::'): m.group(2).strip()
                  for m in re.finditer(r'---- (\S+) stdout ----\n(.*?)(?=\n---- |\n(?:failures|successes):\n|\Z)',
                                       r.stdout, re.S)}
        source_hashes = {path: hashlib.sha256(src[path].encode()).hexdigest() for path in paths}
        source_inputs = json.dumps(source_hashes, sort_keys=True, separators=(',', ':')).encode()
        return {'mutation': mutation, 'tested_sha256': hashlib.sha256(tested.encode()).hexdigest(),
                'source_sha256': source_hashes,
                'source_inputs_sha256': hashlib.sha256(source_inputs).hexdigest(),
                'exit': r.returncode, 'summary': summary, 'failed': failed, 'output': output,
                'stderr': r.stderr[-2000:]}

    result = run(a.mutation)
    receipt = {'ref': a.ref, 'old_ref': OLD_REF, 'old_reference_checked_against_git': old_git is not None,
               **result,
               'scope': 'Actual answer_growth/answer_request/soft_max/pb_max_blocks, pools/growth.rs, '
                        'fw/buffer.rs and fw/channels.rs over a model of the firmware objects with an '
                        'ordered access log, and Ledger::new against its body at '
                        f'{OLD_REF[:12]}. Not kernel, firmware or GPU execution, timing, or recovery.'}
    if a.mutations and not a.mutation:
        receipt['mutations'] = {}
        for m in sorted(MUTATIONS):
            res = run(m)
            test, message = INTENDED[m]
            got = res['output'].get(test, '')
            receipt['mutations'][m] = {'exit': res['exit'], 'failed': res['failed'], 'intended_test': test,
                                       'intended_failure': test in res['failed'] and message in got,
                                       'message': '\n'.join(l for l in got.splitlines()
                                                            if not l.startswith('note:'))[-600:]}
        receipt['controls'] = {}
        for m in sorted(CONTROLS):
            res = run(m)
            receipt['controls'][m] = {'exit': res['exit'], 'failed': res['failed'], 'summary': res['summary']}
    (out / 'result.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps({k: v for k, v in receipt.items() if k not in ('output', 'stderr')}, indent=2))
    # The actual bodies and the controls must pass; a single requested mutation must fail.
    expect_fail = a.mutation in MUTATIONS
    if (receipt['exit'] != 0) != expect_fail:
        raise SystemExit(1)
    if a.mutations and any(v['exit'] == 0 or not v['intended_failure']
                           for v in receipt.get('mutations', {}).values()):
        raise SystemExit('a mutation survived or failed elsewhere than intended')
    if a.mutations and any(v['exit'] != 0 for v in receipt.get('controls', {}).values()):
        raise SystemExit('a control failed')


if __name__ == '__main__':
    main()

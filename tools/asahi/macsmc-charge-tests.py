#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise real macsmc charging callbacks with SMC faults and racing writers.

No battery or firmware is accessed. Only the SMC transport and mutex primitive
are replaced. Callback bodies, flags and the public behaviour enum are extracted
from this checkout, including for each deliberate source mutation.
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
DRIVER = "drivers/power/supply/macsmc-power.c"


def function(source, name):
    start = source.index("static int " + name + "(")
    begin = source.index("{", start)
    end, depth = begin + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


STUBS = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define fallthrough __attribute__((fallthrough))
enum key { KEY_CH0I, KEY_CHTE, KEY_CH0C, KEY_CHLS, KEY_CHWA, KEY_COUNT };
#define SMC_KEY(k) KEY_##k
struct mutex { pthread_mutex_t real; };
static _Thread_local int role;
static struct mutex *lock_mutex(struct mutex *m)
{ assert(!pthread_mutex_lock(&m->real)); return m; }
static void unlock_mutex(struct mutex **m)
{ assert(!pthread_mutex_unlock(&(*m)->real)); }
#define guard(kind) struct mutex *held_lock __attribute__((cleanup(unlock_mutex), unused)) = lock_mutex
struct apple_smc { u32 keys[KEY_COUNT]; unsigned calls, fault, writes; };
struct macsmc_power { struct apple_smc *smc; struct mutex charge_lock;
    bool has_ch0i, has_chte, has_ch0c, has_chls, has_chwa; };
enum power_supply_property { POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,
    POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
    POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD };
union power_supply_propval { int intval; };
struct power_supply { struct macsmc_power *data; };
static struct macsmc_power *power_supply_get_drvdata(struct power_supply *p)
{ return p->data; }
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static bool paused, second_started, second_done, race, fail_chls_read;
static int io(struct apple_smc *s)
{ s->calls++; return s->fault == s->calls ? -EIO : 0; }
static int apple_smc_read_u16(struct apple_smc *s, enum key k, u16 *v)
{
    int rc = io(s); if (rc) return rc;
    if (fail_chls_read && k == KEY_CHLS) return -EIO;
    *v = s->keys[k];
    if (race && role == 1 && k == KEY_CHLS) {
        assert(!pthread_mutex_lock(&control)); paused = true;
        assert(!pthread_cond_broadcast(&condition));
        while (!second_started) assert(!pthread_cond_wait(&condition, &control));
        struct timespec limit; assert(!clock_gettime(CLOCK_REALTIME, &limit));
        limit.tv_nsec += 100000000;
        if (limit.tv_nsec >= 1000000000) { limit.tv_sec++; limit.tv_nsec -= 1000000000; }
        while (!second_done) {
            int wait = pthread_cond_timedwait(&condition, &control, &limit);
            if (wait == ETIMEDOUT) break;
            assert(!wait);
        }
        assert(!pthread_mutex_unlock(&control));
    }
    return 0;
}
static int apple_smc_read_u8(struct apple_smc *s, enum key k, u8 *v)
{ int rc=io(s); if (!rc) *v=s->keys[k]; return rc; }
static int apple_smc_read(struct apple_smc *s, enum key k, void *v, int n)
{ int rc=io(s); if (rc) return rc; memset(v, 0, n); *(u8 *)v=s->keys[k]; return n; }
static int apple_smc_write_u16(struct apple_smc *s, enum key k, u16 v)
{ int rc=io(s); if (!rc) {s->keys[k]=v; s->writes++;} return rc; }
static int apple_smc_write_u8(struct apple_smc *s, enum key k, u8 v)
{ return apple_smc_write_u16(s,k,v); }
static int apple_smc_write_u32(struct apple_smc *s, enum key k, u32 v)
{ return apple_smc_write_u16(s,k,v); }
static int apple_smc_write_flag(struct apple_smc *s, enum key k, bool v)
{ return apple_smc_write_u16(s,k,v); }
'''

TESTS = r'''
static struct apple_smc smc;
static struct macsmc_power power = {.smc=&smc,
    .charge_lock={.real=PTHREAD_MUTEX_INITIALIZER}};
static struct power_supply supply = {.data=&power};
static unsigned cases;
static void reset(void)
{ memset(&smc,0,sizeof(smc)); power.has_ch0i=power.has_chte=power.has_ch0c=false;
  power.has_chls=power.has_chwa=false; race=fail_chls_read=false; }
static int set(enum power_supply_property prop, int value)
{ union power_supply_propval v={.intval=value};
  int rc=macsmc_battery_set_property(&supply,prop,&v);
  if (!race) { assert(!pthread_mutex_trylock(&power.charge_lock.real));
  assert(!pthread_mutex_unlock(&power.charge_lock.real)); } return rc; }
static int mode(int value) { return set(POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,value); }
static int limit(int value) { return set(POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,value); }
static int get(void)
{ int rc=macsmc_battery_get_charge_behaviour(&power);
  if (!race) { assert(!pthread_mutex_trylock(&power.charge_lock.real));
  assert(!pthread_mutex_unlock(&power.charge_lock.real)); } return rc; }
static void values(void)
{
  for (unsigned bits=0;bits<32;bits++) {
    reset();power.has_ch0i=bits&1;power.has_ch0c=bits&2;power.has_chte=bits&4;
    power.has_chls=bits&8;power.has_chwa=bits&16;
    int mask=macsmc_battery_charge_behaviours(&power);
    assert(!!(mask&BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO))==!!(bits&15));
    assert(!!(mask&BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE))==!!(bits&8));
    assert(!!(mask&BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE))==!!(bits&1));
    assert(!!(mask&BIT(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE))==!!(bits&6));cases++;
  }
  for (int force=0;force<2;force++) for (int old=10;old<100;old++) {
    reset();power.has_chls=true;smc.keys[KEY_CHLS]=old|(force?CHLS_FORCE_DISCHARGE:0);
    assert(get()==(force?POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE:POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO));
    assert(!mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO));assert(smc.keys[KEY_CHLS]==(unsigned)old);
    assert(!mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE));
    assert(smc.keys[KEY_CHLS]==(unsigned)(old|CHLS_FORCE_DISCHARGE));cases++;
  }
  const int limits[]={-1,0,9,10,79,80,99,100,101,2147483647};
  for (unsigned i=0;i<sizeof(limits)/sizeof(*limits);i++) for (int force=0;force<2;force++) {
    reset();power.has_chls=true;smc.keys[KEY_CHLS]=80|(force?CHLS_FORCE_DISCHARGE:0);
    assert(!limit(limits[i]));
    unsigned expected=limits[i]<10?10:limits[i]<100?(unsigned)limits[i]:0;
    assert(smc.keys[KEY_CHLS]==(expected|(force?CHLS_FORCE_DISCHARGE:0)));cases++;
  }
  reset();assert(mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE)==-EOPNOTSUPP);
  assert(!smc.calls); assert(mode(999)==-EINVAL); assert(!smc.calls);cases+=2;
  reset();power.has_chwa=true;assert(!limit(80));assert(smc.keys[KEY_CHWA]==1);
  assert(!limit(100));assert(!smc.keys[KEY_CHWA]);
  assert(mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE)==-EOPNOTSUPP);cases++;
  reset();power.has_ch0i=power.has_ch0c=power.has_chls=true;
  smc.keys[KEY_CH0I]=1;smc.keys[KEY_CH0C]=1;smc.keys[KEY_CHLS]=CHLS_FORCE_DISCHARGE|80;
  assert(get()==POWER_SUPPLY_CHARGE_BEHAVIOUR_FORCE_DISCHARGE);
  smc.keys[KEY_CH0I]=0;assert(get()==POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE);
  smc.keys[KEY_CH0C]=0;assert(get()==POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE);cases++;
  reset();power.has_chte=power.has_ch0c=true;smc.keys[KEY_CHTE]=0;smc.keys[KEY_CH0C]=1;
  assert(get()==POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO);
  assert(!mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_INHIBIT_CHARGE));
  assert(smc.keys[KEY_CHTE]==1);cases++;
  assert(!set(POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,20));cases++;
  printf("charging values: %u cases\n",cases);
}
static void faults(void)
{
  reset();power.has_chls=power.has_ch0i=power.has_chte=true;
  smc.keys[KEY_CH0I]=smc.keys[KEY_CHTE]=1;smc.keys[KEY_CHLS]=80;fail_chls_read=true;
  assert(mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE)==-EIO);
  assert(!smc.writes && smc.keys[KEY_CH0I]==1 && smc.keys[KEY_CHTE]==1);cases++;
  for (unsigned at=1;at<=4;at++) {
    reset();power.has_chls=power.has_ch0i=power.has_chte=true;
    smc.keys[KEY_CHLS]=CHLS_FORCE_DISCHARGE|80;smc.fault=at;
    assert(mode(POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO)==-EIO);
    assert(smc.calls==at);cases++;
  }
  for (unsigned at=1;at<=2;at++) {
    reset();power.has_chls=true;smc.keys[KEY_CHLS]=80;smc.fault=at;
    assert(limit(70)==-EIO);assert(smc.keys[KEY_CHLS]==80);
    assert(smc.calls==at);cases++;
  }
  for (unsigned at=1;at<=3;at++) {
    reset();power.has_chls=power.has_ch0i=power.has_chte=true;smc.fault=at;
    assert(get()==-EIO);assert(!smc.writes);cases++;
  }
  puts("SMC faults: every mode operation, both limit operations, all getter reads; locks released");
}
static void *threshold_writer(void *unused)
{ (void)unused;role=1;assert(!limit(70));return NULL; }
static void *behaviour_writer(void *unused)
{
  (void)unused;role=2;assert(!pthread_mutex_lock(&control));
  while (!paused) assert(!pthread_cond_wait(&condition,&control));
  second_started=true;assert(!pthread_cond_broadcast(&condition));
  assert(!pthread_mutex_unlock(&control));
  union power_supply_propval v={.intval=POWER_SUPPLY_CHARGE_BEHAVIOUR_AUTO_DISCHARGE};
  assert(!macsmc_battery_set_property(&supply,POWER_SUPPLY_PROP_CHARGE_BEHAVIOUR,&v));
  assert(!pthread_mutex_lock(&control));second_done=true;
  assert(!pthread_cond_broadcast(&condition));assert(!pthread_mutex_unlock(&control));return NULL;
}
static void interleaving(void)
{
  reset();power.has_chls=true;smc.keys[KEY_CHLS]=80;race=true;
  paused=second_started=second_done=false;pthread_t a,b;
  assert(!pthread_create(&a,NULL,threshold_writer,NULL));
  assert(!pthread_create(&b,NULL,behaviour_writer,NULL));
  assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
  assert(smc.keys[KEY_CHLS]==(CHLS_FORCE_DISCHARGE|70));cases++;
  puts("racing threshold/mode writes: both settings survive the forced interleaving");
}
int main(void) { values();faults();interleaving();printf("PASS %u cases\n",cases); }
'''


def fixture(source):
    header = (ROOT / "include/linux/power_supply.h").read_text()
    enum = re.search(r"enum power_supply_charge_behaviour \{.*?\n\};", header, re.S).group()
    names = ("CH0R_NOAC_CH0I", "CH0X_CH0C", "CHLS_MIN_END_THRESHOLD",
             "CHLS_FORCE_DISCHARGE", "CHWA_PROP_WRITE_THRESHOLD")
    defines = "\n".join(re.search(r"^#define " + n + r"\s+.*$", source, re.M).group() for n in names)
    methods = "\n".join(function(source, n) for n in (
        "macsmc_battery_charge_behaviours", "macsmc_battery_get_charge_behaviour",
        "macsmc_battery_set_charge_behaviour",
        "macsmc_battery_set_property"))
    return STUBS + enum + "\n" + defines + "\n" + methods + TESTS


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--mutations", action="store_true")
    ap.add_argument("--output", type=Path)
    args = ap.parse_args()
    source = (ROOT / DRIVER).read_text()
    read_block = '\t\t/* Read the shared limit before changing any charging inhibitors. */\n\t\tif (power->has_chls) {\n\t\t\tret = apple_smc_read_u16(power->smc, SMC_KEY(CHLS), &vu16);\n\t\t\tif (ret)\n\t\t\t\treturn ret;\n\t\t}\n\n'
    mutations = {
        "late-chls-read": (read_block, "", "late"),
        "chls-only-without-auto": ("power->has_ch0c || power->has_chte || power->has_chls", "power->has_ch0c || power->has_chte", "one"),
        "no-writer-lock": ("guard(mutex)(&power->charge_lock);", "/* writer not serialized */", "last"),
        "always-force-limit": ("kval &= CHLS_FORCE_DISCHARGE;", "kval = CHLS_FORCE_DISCHARGE;", "one"),
        "clear-force-on-limit": ("kval &= CHLS_FORCE_DISCHARGE;", "kval = 0;", "one"),
        "mode-overwrites-limit": ("vu16 |= CHLS_FORCE_DISCHARGE;", "vu16 = CHLS_FORCE_DISCHARGE;", "one"),
        "auto-keeps-force": ("vu16 &= ~CHLS_FORCE_DISCHARGE;", "vu16 |= CHLS_FORCE_DISCHARGE;", "one"),
        "unsupported-mode-accepted": ("if (!power->has_chls)", "if (false)", "one"),
    }
    results = {}
    with tempfile.TemporaryDirectory(prefix="macsmc-charge-tests-") as temporary:
        work = Path(temporary)
        for name in ["real"] + (list(mutations) if args.mutations else []):
            candidate = source
            if name != "real":
                old, new, how = mutations[name]
                if old not in candidate:
                    raise ValueError("mutation anchor missing: " + name)
                if how == "last":
                    pos = candidate.rindex(old)
                    candidate = candidate[:pos] + candidate[pos:].replace(old, new, 1)
                else:
                    candidate = candidate.replace(old, new, 1)
                    if how == "late":
                        at = candidate.index("\t\t/* Set or clear force-discharge")
                        candidate = candidate[:at] + read_block + candidate[at:]
            cfile, exe = work / (name + ".c"), work / name
            cfile.write_text(fixture(candidate))
            build = subprocess.run([os.environ.get("CC", "clang"), "-std=gnu11", "-Wall", "-Wextra",
                                    "-Werror", "-pthread", "-fsanitize=address,undefined", "-g",
                                    str(cfile), "-o", str(exe)], capture_output=True, text=True)
            if build.returncode:
                raise RuntimeError(name + " did not compile:\n" + build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=15)
            passed = run.returncode == 0
            if name == "real" and not passed:
                raise RuntimeError(run.stdout + run.stderr)
            if name != "real" and (passed or "Assertion" not in run.stderr and "assertion" not in run.stderr):
                raise RuntimeError(name + " was not rejected at a runtime assertion:\n" + run.stderr)
            results[name] = {"returncode": run.returncode, "stdout": run.stdout,
                             "stderr": run.stderr.replace(str(work), "<temporary>")}
            print(name + (": PASS" if name == "real" else ": runtime mutation rejected"))
    receipt = {"driver_sha256": hashlib.sha256(source.encode()).hexdigest(), "results": results,
               "scope": "Actual callbacks with simulated SMC transport; no battery hardware claim."}
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")


if __name__ == "__main__":
    main()

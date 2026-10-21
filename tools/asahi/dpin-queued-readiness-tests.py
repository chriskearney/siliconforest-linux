#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise the actual provider-call loop, including its legacy path."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def extract(source, name):
    match = re.search(r"^(?:static )?(?:int|bool|void) " + name +
                      r"\([^;]*?\)\n\{", source, re.M | re.S)
    if not match:
        raise ValueError(name)
    end = source.index("{", match.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include <stdio.h>
typedef uint64_t u64;
#define APPLE_DP_CONNECT_TRIES 60
#define APPLE_DP_FIRMWARE_TRIES 240
#define APPLE_DP_CONNECT_WAIT_MS 500
struct apple_dpin_policy { bool queued_readiness; };
struct apple_dpin_state { bool alive, paused; };
struct apple_dpin_tokens { u64 requested, admitted, inflight, handed; };
struct apple_cio { struct apple_dpin_policy *dp; void *dev; };
struct apple_dpin_ctx {
    struct apple_cio *acio; int lock; unsigned idx;
    struct apple_dpin_state state; struct apple_dpin_tokens tokens;
};
struct apple_dpin_binding { u64 generation; };
struct apple_dpin_provider_ops { void *get, *held, *call, *put, *ctx; };
#define apple_dpin_provider_get NULL
#define apple_dpin_provider_held NULL
#define apple_dpin_provider_invoke NULL
#define apple_dpin_provider_put NULL
#define scoped_guard(...) for (int once = 1; once; once = 0)
#define guard(x) no_lock
#define dev_info(...) ((void)0)
static void no_lock(int *lock) { (void)lock; }
static int calls, sleeps, provider_result, succeed_after;
static bool apple_dpin_admission_blocked(const struct apple_dpin_state *s,
        const struct apple_dpin_policy *p, bool admitted)
{ (void)p; (void)admitted; return s->paused; }
static int apple_dpin_provider_call(bool active, const struct apple_dpin_provider_ops *ops)
{ (void)active; (void)ops; ++calls; return succeed_after && calls >= succeed_after ? 0 : provider_result; }
static void msleep(unsigned delay)
{ assert(delay == 500); assert(++sleeps <= 2); }
/* PRODUCTION */
static void call_case(bool queued, bool active, int error, int succeed,
                      int expected, int expected_calls, int expected_sleeps)
{
    struct apple_dpin_policy p = { .queued_readiness = queued };
    struct apple_cio acio = { .dp = &p };
    struct apple_dpin_ctx c = { .acio = &acio, .state.alive = true,
        .tokens = { .requested = 4, .admitted = 4, .inflight = 4 } };
    struct apple_dpin_binding binding = { .generation = 4 };
    calls = sleeps = 0; provider_result = error; succeed_after = succeed;
    assert(apple_dpin_connect(&c, &binding, active) == expected);
    assert(calls == expected_calls && sleeps == expected_sleeps);
    if (active && expected) assert(c.tokens.admitted == 0);
    if (active && !expected) assert(c.tokens.admitted == 4);
}
int main(void)
{
    call_case(true, true, -ENODEV, 0, -ENODEV, 1, 0);
    call_case(true, true, -EAGAIN, 0, -EAGAIN, 1, 0);
    call_case(true, true, -EBUSY, 0, -EBUSY, 1, 0);
    call_case(true, true, 0, 0, 0, 1, 0);
    call_case(true, false, -ENODEV, 0, -ENODEV, 1, 0);
    call_case(false, true, -ENODEV, 3, 0, 3, 2);
    call_case(false, true, -EAGAIN, 3, 0, 3, 2);
    struct apple_dpin_policy queued = { true }, legacy = { false };
    for (int e = 0; e < 2; ++e) {
        unsigned tries = 0, limit = e ? 240 : 60;
        for (unsigned i = 1; i <= limit; ++i) {
            int r = apple_dpin_queued_readiness(&queued, e ? -EAGAIN : -ENODEV, &tries);
            assert(tries == i && r == (i < limit ? -EAGAIN : -ETIMEDOUT));
        }
        assert(apple_dpin_queued_readiness(&queued, 0, &tries) == 0 && tries == 0);
    }
    unsigned tries = 240;
    assert(apple_dpin_queued_readiness(&queued, -EAGAIN, &tries) == -ETIMEDOUT);
    assert(tries == 240);
    assert(apple_dpin_queued_readiness(&legacy, -ENODEV, &tries) == -ENODEV);
    assert(tries == 240);
    puts("provider yields without sleeping; legacy retries, token revocation and bounds pass");
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    driver = (ROOT / 'drivers/thunderbolt/apple.c').read_text()
    state = (ROOT / 'drivers/thunderbolt/apple-dpin-state.c').read_text()
    bodies = [extract(state, name) for name in (
        'apple_dpin_readiness_retry', 'apple_dpin_queued_readiness',
        'apple_dpin_token_access', 'apple_dpin_token_admit', 'apple_dpin_token_revoke')]
    bodies.append(extract(driver, 'apple_dpin_connect'))
    production = '\n'.join(bodies)
    variants = [('real', production)]
    if args.mutations:
        variants += [
            ('blocking-loop', production.replace(
                'if (active && acio->dp->queued_readiness)', 'if (false)')),
            ('all-platforms-yield', production.replace(
                'if (active && acio->dp->queued_readiness)', 'if (active)')),
            ('missing-token-revocation', production.replace(
                'apple_dpin_token_revoke(&c->tokens, binding->generation);', '(void)0;')),
            ('unbounded-retry', production.replace(
                'return apple_dpin_readiness_retry(true, result, *tries) ? -EAGAIN : -ETIMEDOUT;',
                'return -EAGAIN;')),
        ]
    with tempfile.TemporaryDirectory(prefix='dpin-readiness-') as tmp:
        out = args.out or Path(tmp)
        out.mkdir(parents=True, exist_ok=True)
        results = []
        for name, body in variants:
            if name != 'real':
                assert body != production, name
            source = out / (name + '.c')
            source.write_text(FIXTURE.replace('/* PRODUCTION */', body))
            binary = out / name
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Werror',
                            str(source), '-o', str(binary)], check=True)
            def no_core():
                resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
            run = subprocess.run([str(binary)], text=True, capture_output=True,
                                 preexec_fn=no_core)
            assert (run.returncode == 0) == (name == 'real'), (name, run.stderr)
            results.append({'name': name, 'exit': run.returncode, 'stdout': run.stdout,
                            'stderr': run.stderr})
        (out / 'receipt.json').write_text(json.dumps({
            'source_sha256': hashlib.sha256(production.encode()).hexdigest(),
            'results': results}, indent=2) + '\n')
        print(json.dumps(results))


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Execute T6030 DCP callbacks and PHY clock functions on modeled registers."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import resource
import subprocess
ROOT = Path(__file__).resolve().parents[2]
FUNCTIONS = {
    'include/linux/soc/apple/dp-tunnel.h': ['apple_dp_tunnel_t6030_pclk'],
    'drivers/gpu/drm/apple/dcp-fabric-session.h': ['dcp_fabric_callback_valid'],
    'drivers/phy/apple/atc.c': [
        'apple_atc_tunnel_is_t6030', 'atc_t8122_tunnel_apb', 'atc_t8122_tunnel_stop',
        'atc_t8122_tunnel_pclk', 'atc_t8122_tunnel_start',
        'apple_atc_dp_tunnel_rate_source'],
    'drivers/gpu/drm/apple/dcp-fabric.c': [
        'dcp_route_clock_rate', 'dcp_tunnel_set_rate'],
}
CASES = ['rate-matrix', 'dual-start', 'reverse-start', 'stop-second', 'same-rate',
         'stale-owner', 'same-source', 'retune-one', 'busy-mode', 'busy-pll',
         'invalid-source', 'invalid-rate', 'wrong-generation', 'failed-cleanup', 'failed-final-stop']
CASES += ['fail-start-' + str(i) for i in range(4)]

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--mutations', action='store_true')
    parser.add_argument('--callback-ref')
    parser.add_argument('--pclk-function', type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('dp_controls', ROOT / 'tools/asahi/dp-release-tests.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    bodies = {name: module.function((ROOT / path).read_text(), name)
              for path, names in FUNCTIONS.items() for name in names}
    if args.callback_ref:
        original = subprocess.check_output(
            ['git', 'show', args.callback_ref + ':drivers/gpu/drm/apple/dcp-fabric.c'],
            cwd=ROOT, text=True)
        bodies['dcp_tunnel_set_rate'] = module.function(original, 'dcp_tunnel_set_rate')
    if args.pclk_function:
        bodies['atc_t8122_tunnel_pclk'] = args.pclk_function.read_text()
    lines = iter((ROOT / 'drivers/phy/apple/atc.c').read_text().splitlines(keepends=True))
    defines = []
    for line in lines:
        if line.startswith('#define'):
            definition = line
            while line.rstrip().endswith('\\'):
                line = next(lines)
                definition += line
            defines.append(definition)
    code = (ROOT / 'tools/asahi/t6030-clock/fixture.c').read_text().replace(
        '/* REGISTER_DEFINES */', ''.join(defines) + re.search(
            r'enum atcphy_generation \{.*?\n\};',
            (ROOT / 'drivers/phy/apple/atc.c').read_text(), re.S).group()).replace(
        '/* PRODUCTION_FUNCTIONS */', '\n'.join(bodies.values()))
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.out / 'production.c'
    source.write_text(code)
    binary = args.out / 'production'
    subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-unused-parameter', str(source), '-o', str(binary)], check=True)
    def no_core():
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    results = []
    for case in CASES:
        run = subprocess.run([str(binary), case], capture_output=True,
                             text=True, preexec_fn=no_core)
        results.append({'case': case, 'exit': run.returncode,
                        'stdout': run.stdout, 'stderr': run.stderr})
    receipt = {'production_bodies': {name: hashlib.sha256(body.encode()).hexdigest()
                                    for name, body in bodies.items()},
               'translation_sha256': hashlib.sha256(code.encode()).hexdigest(),
               'hardware_access': False, 'original_callback_ref': args.callback_ref,
               'pclk_function_override': str(args.pclk_function) if args.pclk_function else None,
               'results': results}
    (args.out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(json.dumps(results, indent=2))
    if any(row['exit'] for row in results):
        raise SystemExit(1)
    if args.mutations:
        mutations = {
            'no-source-owner': (
                'if ((atcphy->tunnel_users & BIT(dpin)) &&\n\t    atcphy->tunnel_source[dpin] != source)',
                'if (false)'),
            'duplicate-source': (
                'if ((atcphy->tunnel_users & BIT(1 - dpin)) &&\n\t    atcphy->tunnel_source[1 - dpin] == source)',
                'if (false)'),
            'stop-shared-pll': ('if (!(atcphy->tunnel_users & ~BIT(dpin)))', 'if (true)'),
            'lose-failed-start-owner': (
                'if (!ret || (atcphy->tunnel_clock_on && !atcphy->tunnel_rate))', 'if (!ret)'),
            'wrong-clock-selector': (
                'u32 select = pclk ? DPTX_PCLK2_SELECT : DPTX_PCLK1_SELECT;',
                'u32 select = DPTX_PCLK1_SELECT;'),
            'drop-prerequisite': ('if (!pclk && (atcphy->tunnel_users & BIT(1)))', 'if (false)'),
        }
        evidence = []
        for name, (before, after) in mutations.items():
            if code.count(before) != 1:
                raise ValueError('Mutation no longer matches exactly once: ' + name)
            mutant = code.replace(before, after)
            source = args.out / (name + '.c')
            binary = args.out / name
            source.write_text(mutant)
            subprocess.run(['cc', '-std=gnu11', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-parameter', str(source), '-o', str(binary)], check=True)
            failures = []
            for case in CASES:
                result = subprocess.run([str(binary), case], capture_output=True,
                                        text=True, preexec_fn=no_core)
                if result.returncode:
                    failures.append({'case': case, 'exit': result.returncode,
                                     'stderr': result.stderr})
            if not failures:
                raise AssertionError('Mutation escaped the tests: ' + name)
            evidence.append({'mutant': name, 'detected_by': failures,
                             'translation_sha256': hashlib.sha256(mutant.encode()).hexdigest()})
        (args.out / 'mutations.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print('Clock mutation controls: all ' + str(len(evidence)) + ' detected')
if __name__ == '__main__':
    main()

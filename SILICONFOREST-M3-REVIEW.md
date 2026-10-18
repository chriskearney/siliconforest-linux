# SiliconForest M3 integration review — 2026-10-18

**Approved for source integration as an off-by-default development variant.
This is not a qualified kernel release or an installation recommendation.**

## Sources and integration decision

| Author | Original PR | Reviewed head |
| --- | --- | --- |
| Eryk Wieliczko (`erykwieliczko`) | [Linux #358](https://github.com/aurora-silicon/linux/pull/358), 35 commits | `79d6375f4b03c70b71ac2fa3c27e5695a559516f` |
| Naeem Malik (`malik-na`) | [Linux #363](https://github.com/aurora-silicon/linux/pull/363), 12 commits stacked on #358 | `e30d702e3722aba6114c67ee86505cea0ce76117` |

The maintained parent is `5d570a78a4ab29ac383ec8988be2356dfc6c8a75`, Linux
7.1.12. The PR prerequisite is `0daed2db47875fc705b4e7aea10b4d33e3514b06`.
The earlier review snapshot is `8a29680940cde36759e2061d3c8a8e7e9df70fa9`.
Original commits, author identities, licenses and import refs are retained.
[SILICONFOREST-M3-SOURCES.json](SILICONFOREST-M3-SOURCES.json) records the source
and destination paths for future comparisons.

A direct merge would change 946 files, including unrelated prerequisite work and
large deletions of existing GPU implementations. The prerequisite has 1,414
commits absent by identity; that includes merges and is not a count of new fixes.
The integration therefore retains existing GPU/display/PHY/PMP sources and board
DTS files and adds separate implementations selected by `CONFIG_SILICONFOREST_M3`.
That option defaults to **N** and requires EXPERT, Apple arm64 and USB4 disabled.

The alternative GPU and display directories include the shared code their PRs
actually use, rather than attempting an unverified rewrite against the older
backend interfaces. PHY, PMP/power-domain, Type-C controller and mux dependencies
are selected with the same option. Their duplication is a maintenance cost:
compare both implementations for future shared fixes, and remove this temporary
split only after a reviewed common implementation can preserve existing support.
The larger prerequisite line has not been exhaustively audited.

## Adversarial findings and fixes

1. **High: RAM admission accepted holes and allocator-owned pages.**
   `region_intersects(...) == REGION_INTERSECTS` was treated as proof that an
   entire range was safe reserved RAM. `kernel/resource.c` explicitly permits
   that result for RAM mixed with undefined holes. The new check requires a
   nonempty aligned non-overflowing range and every page to be valid RAM with
   `PageReserved`. Mixed or allocator-owned ranges cannot fall through to a WC
   mapping. These page flags still need confirmation on actual target handoffs.

2. **High: a second adopted-UAT mapping path bypassed that rejection.**
   The earlier fix covered generation-specific mappings but not
   `mmu::Uat::map_physical`. Both now call one tested C classifier through
   `g15::memory_mode`: WB only for complete reserved RAM, WC only for aligned
   ranges outside System RAM, otherwise reject. End alignment is checked too.

3. **High: CDM validation returned success when its budget expired.**
   Cyclic linked streams, more than 64 links, and streams beyond 8,192 records
   previously broke out and returned success. They now fail with EINVAL;
   success requires an explicit TERMINATE within the budget. A terminator in
   the last permitted record remains valid. This gate is an encoder safety
   net: it does not establish immutable command buffers or shader sandboxing.

4. **High: the measured M3 Max profile admitted an unmeasured board.**
   J514C was accepted with J516C's 3456x2234 panel profile, and the GPU accepted
   a generic T6031 machine identity. Both GPU and panel admission now require
   J516C. The early display changeset rejects another board before mutation.
   This does not qualify every J516C bin or every machine's power calibration.

5. **High: using the maintained UAPI would silently change command layouts.**
   The imported Mesa ABI uses feature bit 8 for PER_COMMAND_INPUT_SYNCS; the
   maintained ABI uses 1024. Render/compute sizes are 384/136 versus 448/200,
   with shifted trailing fields. Simply compiling against the maintained
   structs would not be a valid port. The exact imported header is kept as
   `include/uapi/drm/asahi_m3_drm.h`; Rust binding generation selects it only
   for the alternative profile. The normal installed header is unchanged.
   Host checks exercise the actual selector and important layouts in both modes.

6. **Medium: missing board data was discovered after firmware started.**
   Board images, hashes and static layout checks now run in probe admission,
   before the runtime-PM hold and firmware start. Verified bytes are carried
   into initdata construction without requesting the file again. The hardware
   timestamp is still sampled when initdata is built. Missing or wrong J516C
   data is rejected before GPU firmware is touched; no substitute blob is shipped.

7. **Medium: the incoming DRM allocation API permits uninitialized driver data.**
   The maintained kernel has removed `new_uninit`/`init_data`. The alternative
   driver now initializes AsahiData first and publishes the backend through
   SetOnce, with access returning ENODEV until publication. No changes to the
   shared DRM allocator are needed. VM creation/destruction and queue destruction
   also reject nonzero reserved UAPI padding before changing objects.

8. **Integration: newer Type-C transactions are a real prerequisite.**
   The PHY uses `safe_after_role`; the controller uses begin/apply/end/abort
   transactions. Removing those calls merely to compile would lose their
   ordering contract. A matched Type-C mux/controller variant is included.
   The maintained Thunderbolt switch implementation, including its provider
   callback lock, captured module owner and shutdown checks, is retained in
   that variant instead of regressing to the prerequisite's older lifetime rules.
   USB4 remains excluded because display tunnel ownership has a separate ABI.

The maintained IOMMU tuple parser already includes Eryk's consume-every-owner
fix and additionally checks tuple bounds. It is retained. Two of Naeem's DT
patches were already patch-equivalent to baseline changes; existing board files
are not overwritten. New development DTBs live under `apple/m3-g15/`.

## Validation performed

- **18 C host cases** against the production range helpers/classifier: valid
  reserved RAM, non-RAM carveouts, holes, invalid PFNs, allocator ownership,
  alignment, empty ranges and overflow.
- **23 Rust host tests**, importing production gate, recovery, idle, firmware
  buffer/channel and pool-growth code. The initial review reproduced failures
  before its RAM and CDM fixes. Five Rust tests cover CDM gate boundaries.
- **2 UAPI profile checks** compile the actual binding-header selection in both
  modes and assert feature bits, sizes and important field offsets.
- LLVM 22.1.8 / Rust 1.93.1 arm64 Kbuild checks of the enabled alternative GPU,
  display, PMP, PHY, power-domain, mux, Type-C and SIO targets; composite driver
  objects linked. Development J514S/J516C DTBs and normal Apple DTBs compiled.
- The maintained configuration was processed with olddefconfig in both the
  original baseline and integrated tree. The only difference is
  `# CONFIG_SILICONFOREST_M3 is not set`. Existing GPU/display sources and normal
  board DTS files remain byte-identical to that baseline.
- CI runs the host suites on PRs and maintained-branch pushes.

Host services are mocked. Driver-object compilation is not a complete Image,
modules/modpost, package, boot, suspend, GPU workload or hardware qualification.
The build emits unused-code warnings from imported generic version expansions;
the local libclang version probe also prints a duplicate 22.1.8 line. Neither is
being presented as hardware evidence. No Mac was rebooted and no kernel installed.

Run host checks from this kernel tree:

```sh
RUSTUP_TOOLCHAIN=1.93.1 python3 tools/testing/selftests/drivers/gpu/asahi-g15/run.py
```

For source development, copy your maintained configuration to a separate build
output directory, then merge `arch/arm64/configs/siliconforest-m3.config` and run
olddefconfig with the pinned arm64 toolchain. The profile deliberately switches
GPU/display implementations and disables USB4. Preserve its generated .config.
The normal SiliconForest build helper continues using the maintained config.
The development DTBs are:

- `arch/arm64/boot/dts/apple/m3-g15/t6030-j514s.dtb`
- `arch/arm64/boot/dts/apple/m3-g15/t6031-j516c.dtb`

Do not flatten those paths into a common DTB directory or install them over the
normal board DTBs. Neither compilation nor selecting this profile supplies the
required boot handoff, board data or userspace.

## Requirements before a public M3 release

- Pin and qualify matching m1n1 handoffs. Eryk's related
  [m1n1 #34](https://github.com/aurora-silicon/m1n1/pull/34), merge
  `97820568035b8d7035c68e4126f203d86c76abe4`, concerns J514S external display/SIO.
  A matching T6031 handoff source is still needed. The personal m1n1 pin has
  not changed in this integration.
- Review and build matching Mesa:
  [Eryk #23](https://github.com/aurora-silicon/mesa/pull/23),
  `cf8ec613be202d8bfc5440e61e9aa6c60700fad7`, and
  [Naeem #25](https://github.com/aurora-silicon/mesa/pull/25),
  `aab698d2ef34e2aef38ba4d2bfcfa3abc5ded4c5`. They are identified, not imported.
- Obtain a reproducible, appropriately licensed source-derived J516C board-data
  path and verify its machine calibration. The source expects
  `asahi/t6031-j516c-board.bin`, digest
  `4e03f29fa447dcebfd4d1c225a647bfa31b363458f2833823a5ca8a10edff3fc`.
  No generator or file accompanies this source; other Max boards need profiles.
- Complete kernel/module linkage, package creation and target tests for each
  supported board, including fallback, display takeover, rendering, failure
  injection, hotplug, power management and attended suspend/resume.
- Resolve the USB4 ownership API port before enabling USB4 with this stack.
  Keep bring-up overrides (`g15_stream_gate=0`, unpinned firmware prefixes)
  outside release profiles. Retained sessions cannot safely unload or restart.

Source integration does not remove these release requirements. The archived
historical kernel repository is outside this workflow and remains untouched.

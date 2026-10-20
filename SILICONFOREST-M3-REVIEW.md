# SiliconForest M3 integration review — 2026-10-18

**Approved for source integration as an off-by-default development variant.
This is not a qualified kernel release or an installation recommendation.**

The sections below through “Requirements before a public M3 release” record the
original October 18 full-display-variant review. Later native-GPU releases reuse
the isolated GPU driver with the normal display stack. See the dated follow-up
at the end for the shared allocator changes; it does not qualify a new release.

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


## 2026-10-19: PR363 parameter-buffer follow-up

Reviewed public upstream head `027bd7623f370e264c8dd056c71a00e1df6bc8a6`.
Four carries, beginning with `97f81d57022c`, retain Naeem Malik's authorship
and supplied sign-offs. Their only adaptation is the `asahi/` to `asahi-m3/`
path mapping. Separate Chris-authored commits contain the review fixes and tests.
The frozen code snapshot is `709f0f08316ad528181027abe1cf00b3083b9bf0`, on
SiliconForest `0e1f466613761fecbe71ba8ca25f8b3598577062` (Linux 7.2.11).

These changes affect the shared G15 allocator on **T6030 and T6031**, including
the native-GPU profile. They seed all 22 already-allocated compute pools (22528
pages), bound the growth-answer loop to its starting queue, limit hard-maximum
refusals, reserve blocks beyond the firmware growth maximum, and map a 512 MiB
parameter buffer. Default initial/firmware-maximum/posted-maximum sizes are
1024/3072/4095 blocks. The 4096th mapped block is not posted: the block-list
producer must stay below its 4096-entry wrap point.

Review changes:

- `Ledger::new` now caps both growing and fixed configurations at
  `min(mapped blocks, block-list entries - 1)`. The upstream helper accepted
  4096, producing an empty-looking producer index of zero. Its current default
  caller already passes 1024/4095; this is defensive hardening, not a reproduced
  default boot fault. Compile-time bounds and constructor/growth tests cover it.
- Comments distinguish the physical heap, the firmware soft maximum and the
  posted maximum. They no longer promise that a refused request completes the
  scene or that the deadline path successfully recovers the GPU.
- The host harness extracts the actual growth answer, request handler, clamp,
  constants and constructor, and models ordered firmware-object accesses.
  Requests arriving during a reply wait must remain queued for the next poll.
  Receipts hash all eight source inputs as well as the extracted test bodies.

Host checks at the frozen code snapshot (Rust 1.93.1):

- G15 selftests: 55 Rust passed, 2 private-fixture cases ignored; 26 C/UAPI passed.
- PB-growth harness: 10 passed; all 11 deliberate regressions fail the intended
  assertions. Constructor comparison covers 35,364,050 inputs. The default trace
  reaches 4095, attempts two refusals, then leaves requests unanswered. The
  whole-heap tuning control is model-only: production static assertions reject it.
- Existing statistics-drain harness passes with all seven regressions detected.
- Independent constructor/default-trace and queue-extraction checks also pass;
  removing the constructor cap or restoring drain-until-empty makes them fail.

The normal GPU driver, both display drivers, DTs, UAPI, Rust core and build
scripts are byte-identical to the base. Native render/compute ABI sizes remain
384/136. Existing board admission, checked arithmetic, per-instance J516C
panel scope and the statistics-consumer full barrier are retained.

Build validation is recorded in the integration PR after the final native build.
Host models establish neither firmware behavior nor DMA ordering on hardware.

Limits: the 512 MiB heap is backed and DMA-mapped when allocated, an extra
256 MiB on T6030 too. `g15_pb_max_blocks` changes the firmware soft limit, not
that allocation. Refusal accounting bounds attempts; a failed reply transport
can consume the budget. Physical exhaustion can still reach a fail-stop state
until reboot. No T6030/T6031 workload, boot, thermal or power qualification is
claimed for these additions. Naeem's J516C workload report is upstream evidence,
not a SiliconForest hardware test.

This source carry changes no installed Mac, signed release or catalog. Public
`v2026.10.19.2` remains at `0e1f46661376` until a separately qualified release.

## 2026-10-19: physical parameter-buffer backing on demand

This follow-up supersedes the eager physical-allocation policy in the previous
section. It applies to the native G15 implementation shared by T6030 and T6031.
It is source development; it does not qualify a new installed kernel.

The logical PB address range remains 512 MiB. Its fixed layout, metadata, block
addresses and 4095-block posting ceiling are unchanged. Physical backing starts
at the ledger's initial size (1024 blocks, or 128 MiB, with the current tuning),
then grows in chunks of at most 4 MiB. The firmware soft-limit parameter still
controls the firmware's limit; it is not an allocation-size control. This uses
the M1/M2 discipline of allocating backing before publishing usable blocks,
while preserving G15's own live-growth protocol.

The scheduler permits overlapping flights only within one GPU VM. Before any
prefix is published, that VM catches up with all retained PB backing, including
for compute-only work and recovery prefixes. Growth uses the requesting flight's
retained VM directly, so closing a userspace VM handle cannot hide an active
address space from the growth path. The complete logical aperture remains
reserved against userspace binds even where no physical backing exists yet.

New backing must be allocated, DMA-mapped, checked for complete aligned coverage,
and installed into the executing VM before block records or the producer are
published. Ownership and mapping progress must agree before a success reply.
GPU-only PB leaves do not require the firmware cache-flush path on teardown;
that depends on the actual leaf permissions, not merely their cached attribute.
Normal mapping invalidation and lifetime rules still apply.

Backing is retained at its peak until reboot. Recovery resets the posted-block
ledger and reuses owned backing; it does not discard potentially reachable
pages. Failed preparation leaves the producer and success reply untouched, and
failed transport after publication retains the backing. The existing hard-limit
refusal/deadline policy is preserved. Allocation failure and exhaustion can still
lead to fail-stop; successful recovery and uninterrupted rendering under memory
pressure are not promised. Backing-page allocation uses the existing shmem
MAYFAIL policy, but synchronous allocation/reclaim latency is not bounded here.

The normal M1/M2 driver, board admission, UAPI, display, DTs, Mesa and m1n1 are
outside this change. Boot, sustained growth, multiple clients, memory pressure,
recovery, suspend and power behavior require hardware qualification before a
release containing this allocator is offered by the public installer.

The host checks import the actual geometry and ledger modules (61 Rust tests,
with two private-fixture tests ignored, plus 26 C/UAPI cases). The extended PB
harness executes source-extracted allocation, mapping and grant methods against
mocked GEM/MMU and firmware objects: 24 cases pass, and 22 mutations fail at
their intended checks. It checks startup allocation size, mapping before posting,
allocation/map/append failures, accurate per-VM progress, VM switching, stale
requests, reply failure and reuse after a ledger reset. A positive tuning control
also passes. Test progress loops are bounded, so a deliberately broken driver
cannot keep the test running indefinitely. The existing statistics and board
record controls remain green.

These are host control-flow tests, not GPU execution or kernel MMU execution.
Closed-job VM compatibility rests on a separate review of the actual job guard,
VM cleanup and mapping admission paths; the model does not execute a close
lifecycle. Ledger-reset reuse is not a full recovery test. Native build and layout
results are recorded separately in the control repository's source-validation
report. Hardware qualification remains outstanding.

## Camera controls and display power/colour (2026-10-19)

The reviewed carries are Omacom [PR153](https://github.com/omacom/linux-aurora/pull/153)
at `31f6cbb232df` and [PR157](https://github.com/omacom/linux-aurora/pull/157)
at `9d3aeb24cb86`, plus an adaptation of Aurora Silicon
[PR365](https://github.com/aurora-silicon/linux/pull/365) at `9cd46b2bac71`.
The source pins, authors and limitations are in `SILICONFOREST-M3-SOURCES.json`.
The integration and validation ledger is [SiliconForest PR16](https://github.com/iconidentify/siliconforest-linux/pull/16).

Zero-length UVC extension-unit value queries return `ENODATA` before allocating
or issuing a USB request. Length/info discovery remains available. The lower-level
stall path does not borrow a byte from a zero-length buffer. Actual-body host
checks cover 60 cases with Clang/GCC ASan/UBSan and four deliberate regressions.

XRGB2101010 uses the existing black-background premultiplication workaround.
New colour controls preserve the existing policy by default:

- `appledrm.external_colorimetry=-1` uses firmware scores. Valid colourimetry
  preferences (for example, `10` for sRGB) affect ordinary external SDR RGB
  catalogues; unsupported values retain the old policy. Changes take effect when
  modes are parsed again, normally on replug or reboot. Internal displays and
  strict firmware admission keep their existing rules.
- `appledrm.external_srgb=N` keeps source tagging as before. Opting in tags native
  RGB surfaces as sRGB in the common template flush, excluding YUV. Keep it off
  when the compositor already performs gamut mapping. Native 14.7 and H17P use
  separate presentation paths; this switch does not add source conversion there.

The writable policies are read once per mode-list/frame. A preferred colour is
validated in temporary storage before replacing the accepted RGB candidate or
ranking state. Missing depth/range in a preferred entry cannot erase a valid
fallback. Serialized-parser KUnit cases cover both input orders and strict
profile admission. Host controls exercise 63 production decisions and six
runtime mutations; restoring unchecked promotion also fails the serialized test.

The J613 power adaptation extends the existing `panel_blank`/`panel_off`/lid
state. A completed black frame and power-off share `present_lock`; panel power
state changes only after firmware acknowledgment. Wake establishes the internal
panel selection before powering it on when needed, and invalidates its colour
matrix. Type-C routing retains its lid and CRTC ownership requirements. The
KUnit power suite runs real present, power, callback and mailbox code against a
fake firmware link, including concurrent presentation and refusal/failure paths.
Test cleanup joins the presenter before fixture memory is released.

The combined ordinary display KUnit run is 360 tests: 359 pass and one existing
skip. Compiler, debug-kernel, mutation and independent-review results are recorded
in PR16 and the control repository's source-validation ledger. These are source
and simulated-device checks. This integration makes no local camera, external
10-bit/colourimeter/ICC or physical J613 sleep/wake qualification claim, and does
not update a signed installer release.

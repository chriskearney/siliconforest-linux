# SiliconForest M3 G15 stack review — 2026-10-18

**Status: experimental source import with local fixes. Not approved for promotion
to `siliconforest/next`, installation, or a binary release.**

## Source snapshots

| Author | Original PR | Head commit | Scope |
| --- | --- | --- | --- |
| Eryk Wieliczko (`erykwieliczko`) | [Linux #358](https://github.com/aurora-silicon/linux/pull/358) | `79d6375f4b03c70b71ac2fa3c27e5695a559516f` | 35 commits: J514S M3 Pro power, displays and G15 GPU |
| Naeem Malik (`malik-na`) | [Linux #363](https://github.com/aurora-silicon/linux/pull/363) | `e30d702e3722aba6114c67ee86505cea0ce76117` | 12 commits stacked on #358: T6031/J516C M3 Max |

The source on `siliconforest/m3-g15-review-20261018` preserves those original
commits and authors, followed by the SiliconForest review fixes. The exact PR
base before Eryk's 35 commits is `0daed2db47875fc705b4e7aea10b4d33e3514b06`.
No upstream branch or frozen personal repository was modified.

## Integration gap

SiliconForest's current baseline is
`5d570a78a4ab29ac383ec8988be2356dfc6c8a75` (Linux 7.1.12).
The incoming prerequisite base has **1,414 commits absent by commit identity**
from that baseline; this count includes merges and does not imply that every
patch is functionally new. Comparing the original Naeem head's complete tree to
the baseline changes 946 files, with 56,253 insertions and 180,923 deletions.

The PRs are not a self-contained 47-commit backport. Their base changes the shared
GPU architecture (`asahi.rs` versus `asahi_core.rs` and a different `AsahiData`),
removes much of the older M3 implementation, and changes the DRM UAPI. Even the
prerequisite GPU directory alone differs in 148 files, with 38,497 deletions.
Those prerequisites have not received a complete review here. A direct merge
would bring unrelated work and replace existing behavior, including older M3
paths. The experimental branch is a separate candidate line, not a descendant of
the current `siliconforest/next` tip.

Two of the device-tree patches in Naeem's PR are already patch-equivalent to
changes in the current baseline. Reapplying the entire stack blindly would also
obscure that overlap.

## Findings and local changes

### RAM admission accepted holes and did not establish reservation

`g15/glue.c:asahi_g15_ram_within()` used
`region_intersects(...) == REGION_INTERSECTS` as proof that an entire range was
System RAM. `kernel/resource.c` explicitly documents that this result can also
include undefined memory holes. It also does not establish that pages are kept
out of the allocator. The result authorizes a cacheable mapping of shared UAT
memory, which the host and GPU subsequently modify.

Local fix `79dc30d33316`: validate page alignment and overflow, then check every
page with `pfn_valid`, `page_is_ram`, and `PageReserved`. Reject a RAM overlap
that fails this check instead of falling back to a write-combined mapping.
This deliberately fails closed; actual reservation flags and cache coherency
still require qualification on the target Mac.

### Command-stream validation returned success at its limits

`g15/gate.rs:cdm()` broke out and returned success after exhausting either the
8,192-record limit or 64-link limit, even without seeing a terminator. A cyclic
link stream therefore passed this gate; a sufficiently long stream left its
tail unchecked despite the default gate being enabled.

Local fix `5d96426c00bf`: return success only on the terminator, and return
`EINVAL` when either budget is exceeded. A terminator in the final allowed
record remains valid. This may reject large streams accepted by the prototype;
it does not establish complete command-stream or shader security.

### A J516C board profile was admitted on other M3 Max boards

The GPU matched the generic `apple,t6031` machine identity while loading a
SHA-pinned J516C board-data image. The display profile listed J514C (14-inch)
alongside J516C (16-inch), but used the J516C 3456×2234 panel and framebuffer
stride for both. Sharing a SoC does not establish compatibility of these board
records or panel dimensions.

Local fix `f4472309fd15`: admit the measured GPU profile only on `apple,j516c`,
restrict the DCP profile to J516C, and reject other T6031 boards before the early
display handoff changes their live device tree. Update Kconfig descriptions.
This is not a qualification of all J516C units or of other M3 Max variants.

## Dependencies and unresolved blockers

- **Board data:** `asahi/t6031-j516c-board.bin` is absent. The driver pins one
  digest (`4e03f29fa447dcebfd4d1c225a647bfa31b363458f2833823a5ca8a10edff3fc`)
  for records described by the author as machine-specific. There is no included
  source-only generator for another Mac. Do not distribute another unit's data
  or advertise this as generally installable M3 Max support.
- **Boot handoff:** the current SiliconForest m1n1 pin does not contain the
  `apple,t6031-handoff` or `asahi,t6031-pmp` producer expected by this kernel.
  The matching T6031 handoff source and its exact commit must be identified and
  reviewed before enabling this path. Eryk's related
  [m1n1 #34](https://github.com/aurora-silicon/m1n1/pull/34), merged as
  `97820568035b8d7035c68e4126f203d86c76abe4`, supplies the newer J514S handoffs;
  it has not been imported into SiliconForest's m1n1 pin in this review.
- **Userspace:** Eryk's [Mesa #23](https://github.com/aurora-silicon/mesa/pull/23)
  at `cf8ec613be202d8bfc5440e61e9aa6c60700fad7` and Naeem's
  [Mesa #25](https://github.com/aurora-silicon/mesa/pull/25) at
  `aab698d2ef34e2aef38ba4d2bfcfa3abc5ded4c5` are the paired userspace work.
  The incoming UAPI differs from the current kernel. Stock Mesa compatibility
  must not be assumed. These Mesa patches are identified, not imported here.
- **Remaining M3 Max limits reported by the author:** render register values
  still come from G15S; presentations were paced at 60 fps on a 120 Hz mode;
  EDID, backlight control, and external displays are not established.
- **Firmware overrides:** the driver still offers root-controlled bring-up
  overrides for unpinned firmware and disabled stream gates. Keep their defaults
  when qualifying a release; an override is not a supported firmware profile.
- **Retention:** the driver retains firmware resources and pins the module
  after starting firmware, including some failed starts. Missing board data and
  other late failures need a preflight/cleanup review before production use;
  unloading the module is not a recovery strategy.

## Validation performed

The review inspected firmware identity admission, memory-region validation and
mapping, board/panel selection, stream admission limits, retained-session
behavior, and prerequisite/API differences. It is a focused static review, not a
complete audit of the new GPU implementation or its prerequisite base.

Run the included host checks with:

```sh
RUSTUP_TOOLCHAIN=1.93.1 python3 tools/testing/selftests/drivers/gpu/asahi-g15/run.py
```

The harness compiles the actual production C admission helper with mocked kernel
page/resource services, and compiles the actual Rust stream gate and upstream
pure buffer/growth/recovery/idle code with host shims. It does not duplicate the
driver's admission or gate implementation.

- Before the local fixes: five memory-range cases and two stream-gate tests
  failed, including RAM holes, allocator-owned pages, cyclic links, and an
  unterminated stream at the record budget.
- After the fixes: **9 C cases and 23 Rust tests pass**. The 23 include 18 tests
  already present in the imported pure logic and 5 added gate tests.
- `git diff --check` passes for the local changes.

No full kernel build, arm64 module build, DTB build, Mac boot, or GPU workload was
run for this import. Host mocks do not prove live reservation flags, DMA/cache
coherency, concurrency, hardware compatibility, or firmware behavior. The original
authors' workload results are upstream reports, not SiliconForest test results.

## Before promoting to the maintained baseline

Choose and review the prerequisite framework migration or a targeted backport;
preserve or explicitly retire the existing supported M3 paths. Obtain a
source-derived board-data solution, pin matching m1n1 and Mesa, then validate the
complete configuration and driver on the qualified board. Keep the experimental
source line separate until those dependencies and integration checks are complete.

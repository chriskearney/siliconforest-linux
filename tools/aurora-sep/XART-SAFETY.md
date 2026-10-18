# Shared xART gigalocker safety gate

## Current default

`xart_writes=1` is the driver default. The APFS extent-ownership checks,
record validation and write-ordering checks remain mandatory; unsupported
layouts fail closed. `xart_writes=0 provision_keybag=0` explicitly selects
read-only operation. `provision_keybag=1` is the default: create the Linux
identity if missing and recover an existing identity without replacing it.
The historical evidence below records the earlier opt-in deployments and
does not establish qualification on every supported Mac or APFS layout.

## Other OSes' records: `xart_guard`

The gigalocker is shared by every OS on the Mac, and nothing in a session
record says which OS wrote it. With `xart_guard=1` the driver lists every
session record the enclave creates while Linux runs, in
`/var/lib/aurora-sep-xart-owned.bin`, before the record is written. It then
refuses to rewrite or delete any session record that was already in the
gigalocker and is not on that list. `xart_guard=2` also refuses writes to the
two shared root records. A refused write fails the enclave's request the way
a read-only gigalocker does: Touch ID may then be unavailable, but no other
OS's record changes.

The default, `xart_guard=-1`, takes the platform's level:

| Platform | Default |
| --- | --- |
| M1 (T8103), M1 Pro/Max (T6000/T6001) | 1 |
| M2 (T8112), M2 Pro/Max (T6020/T6021) | 1 |
| MacBook Neo (T8140) | 0 |

The Neo stays at 0 because it already holds Linux records from before the
list existed, and a guarded boot would treat those as another OS's.

Two cases need a decision on an M1 or M2 Mac:

- On a cold-boot M1-family Mac, creating the identity keybag also rewrites a
  record from before Linux. The guard refuses that, so Linux
  cannot create its keybag there unless `options apple_sep xart_guard=0` is
  set. Set it only once the rewrite has been accepted for that Mac.
- Records Linux created while the guard was off are not on the list, so a
  guarded boot refuses them too. A Mac that already used Touch ID with the
  guard off needs `xart_guard=0` to keep using those records.

## Earlier deployment policy

The XART `.gl` file occupies APFS-managed storage. Writes require the explicit
`xart_writes=1` opt-in and an APFS-resolved extent with a single owner, no
snapshots or clones, and valid existing records. `xart_start_sector` is only
an assertion against that resolved extent.

The write path is limited to controlled J414s hardware tests. Restoring an
older raw image does not guarantee recovery of SEP anti-replay state. Keep a
recoverable partition image before testing writes.

## Supported storage layout

The APFS locator accepts only a complete checkpoint, single-node object maps
and file-system tree, and one unencrypted 6 MiB `.gl` extent. Unsupported
layouts fail closed.

Extent validation rejects missing, overlapping, shared, cloned, snapshotted or
mismatched references. Before enabling writes, the kernel checks volume flags,
pending revert fields, inode flags and extent ownership on its block handle.
Malformed or duplicate xART records prevent writes.

The record update order is to allocate a fresh 0x9000-byte slot, write it,
synchronize the disk cache, then clear 0x1000 bytes at the former slot.
Records are validated and duplicate selection is handled when the file opens.
Extent ownership checks alone do not validate record-update recovery or SEP
anti-replay recovery.

## Verification before writes

1. Verify that extent locking keeps the physical mapping stable for the
   entire write, including across APFS copy-on-write, snapshots, encryption
   and remapping.
2. Verify record ordering, duplicate selection, failed-write recovery,
   barriers/cache synchronization and revision behavior in controlled
   non-production tests.
3. Run controlled power-cycle tests with our builds. Check APFS checksums,
   the `.gl` mapping and records before and after each test, with a recoverable
   partition image available.

## J414s read-only hardware checks

Our read-only build verified checkpoint xid 136, one unencrypted 6 MiB `.gl`
extent at APFS blocks 1236–2771, and 11 live records with valid payload CRCs.
The volume flags were `0x1`, with zero snapshots and pending revert fields,
and no inode clone flag. The sole physical extent-reference record had kind
`APFS_KIND_NEW`, owner 16 and reference count 1. Mailbox and RNG operation were
verified; xART writes and keybag provisioning were not exercised.

Four synthetic tests passed for the extent-ownership checks. The supported
layout and ownership checks cover physical mapping and sharing only; they do
not enable general APFS writes.

APFS field definitions: [Apple File System Reference](https://developer.apple.com/support/apple-file-system/Apple-File-System-Reference.pdf).

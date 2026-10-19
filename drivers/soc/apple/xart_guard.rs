// SPDX-License-Identifier: GPL-2.0-only OR MIT
// Copyright 2026 Ryan Murray

//! Keeps the enclave's xART writes to the records this Linux install created.
//!
//! The gigalocker is shared by every OS installed on the Mac. Each OS keeps
//! its anti-replay state in session records keyed by a random session id.
//! Nothing in a record says which OS wrote it, so the driver cannot tell
//! another OS's record from its own by looking at it.
//!
//! With `xart_guard` set, the driver keeps a list of the session records the
//! enclave created while Linux was running, in a host file. It then refuses
//! to rewrite or delete any session record that was in the gigalocker when the
//! driver opened it and is not on that list. With `xart_guard=2` it also
//! refuses writes to the two shared root records. A refused write fails the
//! enclave's request, the same way a read-only gigalocker does: Touch ID may
//! then be unavailable, but no other OS's record changes.
//!
//! Each platform profile has a level for the parameter's default, -1: 1 on
//! the M1 and M2 Macs, whose enclave Linux shares with a macOS install, and 0
//! on the MacBook Neo, which already holds Linux records from before the list
//! existed and would otherwise lose write access to them. SiliconForest does
//! not apply those levels (see [`SILICONFOREST_DEFAULT`]): there the guard is
//! off unless `xart_guard=1` or `2` is set.
//!
//! The list entry is made durable before the enclave's write reaches the
//! gigalocker. A crash between the two leaves an entry for a record that was
//! never written, which is harmless. It can never leave a Linux record that
//! the next boot treats as another OS's: the list's directory entry is
//! flushed before anything is adopted, and the list never grows past what the
//! next boot reads back. A full list refuses new records instead.

use crate::shim::StoreFile;
use crate::xart_store::{crc32_ieee, Key};
use kernel::prelude::*;

/// Host file listing the session records Linux created.
pub(crate) const OWNED_PATH: &CStr = c"/var/lib/aurora-sep-xart-owned.bin";

const ENTRY_SIZE: usize = 32;
const ENTRY_MAGIC: &[u8; 4] = b"AXGO";
/// Bounds the file, in entries, whatever they hold: the driver reads no more
/// than this at probe, and appends no further. The gigalocker has at most a
/// few hundred slots, so a list this long is damaged or has run for a very
/// long time; new records are then refused.
const MAX_ENTRIES: usize = 4096;
/// The byte offset the list ends at when full.
const MAX_END: u64 = (MAX_ENTRIES * ENTRY_SIZE) as u64;
/// Refusals are logged individually up to this count, then only counted.
const LOGGED_REFUSALS: usize = 8;

/// What the guard does with one enclave write or delete.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Verdict {
    /// The record is Linux's own, or a shared root at level 1.
    Allow,
    /// A session record that did not exist when the gigalocker was opened:
    /// list it as Linux's before writing it.
    Adopt,
    /// A record that predates this boot and that Linux did not create.
    Refuse,
}

/// Decides one request. `level` is the `xart_guard` parameter, `existed`
/// whether the record was in the gigalocker when the driver opened it, and
/// `owned` whether it is on Linux's list.
pub(crate) fn decide(level: u8, key: &Key, existed: bool, owned: bool) -> Verdict {
    if key.kind <= 2 {
        return if level >= 2 { Verdict::Refuse } else { Verdict::Allow };
    }
    if owned {
        Verdict::Allow
    } else if existed {
        Verdict::Refuse
    } else {
        Verdict::Adopt
    }
}

/// SiliconForest: the level `xart_guard=-1` selects on every platform, in
/// place of the profile's. SiliconForest updates existing installs, whose
/// Linux records predate the list: a guarded boot refuses them like another
/// OS's, and Touch ID stops working there. Nothing migrates those records
/// yet, and on a cold-boot M1-family Mac creating the identity keybag needs a
/// write the guard refuses. So the guard stays opt-in. `None` would apply the
/// profile levels, which are kept, with their tests, as the policy upstream.
pub(crate) const SILICONFOREST_DEFAULT: Option<u8> = Some(0);

/// The level `xart_guard=-1` selects on a platform whose profile says
/// `platform`.
pub(crate) fn default_level(platform: u8) -> u8 {
    SILICONFOREST_DEFAULT.unwrap_or(platform)
}

/// The guard level for `xart_guard=param` on a platform whose profile
/// defaults to `platform`, or `None` for no guard. A negative value takes the
/// platform's level; a level above 2 is treated as 2, the strictest.
pub(crate) fn level(param: i8, platform: u8) -> Option<u8> {
    let level = u8::try_from(param).unwrap_or(platform);
    match level {
        0 => None,
        level => Some(level.min(2)),
    }
}

/// Encodes one list entry.
pub(crate) fn encode(key: &Key) -> [u8; ENTRY_SIZE] {
    let mut entry = [0u8; ENTRY_SIZE];
    entry[..4].copy_from_slice(ENTRY_MAGIC);
    entry[4] = key.kind;
    entry[8..24].copy_from_slice(&key.uuid);
    let crc = crc32_ieee(&entry[..24]);
    entry[24..28].copy_from_slice(&crc.to_le_bytes());
    entry
}

/// Decodes one list entry; `None` for a torn or foreign one.
pub(crate) fn decode(entry: &[u8]) -> Option<Key> {
    if entry.len() != ENTRY_SIZE || &entry[..4] != ENTRY_MAGIC || entry[5..8] != [0; 3] {
        return None;
    }
    let crc = u32::from_le_bytes([entry[24], entry[25], entry[26], entry[27]]);
    if crc != crc32_ieee(&entry[..24]) || !(3..=4).contains(&entry[4]) {
        return None;
    }
    let mut uuid = [0u8; 16];
    uuid.copy_from_slice(&entry[8..24]);
    Some(Key::new(entry[4], uuid))
}

/// The file that holds Linux's list: the host file, or memory in the tests.
pub(crate) trait ListFile {
    fn size(&self) -> Result<u64>;
    fn read_exact(&self, off: u64, buf: &mut [u8]) -> Result<()>;
    fn write_all(&self, off: u64, buf: &[u8]) -> Result<()>;
    fn sync(&self) -> Result<()>;
    /// Makes the file's directory entry durable.
    fn sync_parent(&self) -> Result<()>;
}

impl ListFile for StoreFile {
    fn size(&self) -> Result<u64> {
        StoreFile::size(self)
    }

    fn read_exact(&self, off: u64, buf: &mut [u8]) -> Result<()> {
        StoreFile::read_exact(self, off, buf)
    }

    fn write_all(&self, off: u64, buf: &[u8]) -> Result<()> {
        StoreFile::write_all(self, off, buf)
    }

    fn sync(&self) -> Result<()> {
        StoreFile::sync(self)
    }

    fn sync_parent(&self) -> Result<()> {
        StoreFile::sync_parent(self)
    }
}

pub(crate) struct Guard<F: ListFile = StoreFile> {
    level: u8,
    /// `None` when the list could not be opened: nothing can be adopted, so
    /// every session write that is not already listed is refused.
    file: Option<F>,
    /// Byte offset of the next entry.
    end: u64,
    owned: KVec<Key>,
    existed: KVec<Key>,
    refused: usize,
}

impl Guard {
    /// Opens Linux's list in the host file and records which keys `existing`
    /// holds now. Never fails: a list that cannot be read protects every
    /// existing record.
    pub(crate) fn open(level: u8, existing: KVec<Key>) -> Guard {
        Guard::with_file(
            level,
            existing,
            StoreFile::open_host(OWNED_PATH).map(|(file, _created)| file),
        )
    }
}

impl<F: ListFile> Guard<F> {
    /// As [`Guard::open`], with the list in `file`.
    pub(crate) fn with_file(level: u8, existing: KVec<Key>, file: Result<F>) -> Guard<F> {
        let mut guard = Guard {
            level,
            file: None,
            end: 0,
            owned: KVec::new(),
            existed: existing,
            refused: 0,
        };
        // A list created this boot (or by a crashed one) is only safe to
        // adopt into once its name is durable: fsync() of the file alone
        // does not guarantee that. Without it, nothing is adopted.
        let file = file.and_then(|file| file.sync_parent().map(|()| file));
        match file {
            Ok(file) => {
                if let Err(e) = guard.load(&file) {
                    pr_warn!(
                        "xART guard: the list of Linux's records is unreadable ({:?}); every existing session record is protected and none can be adopted\n",
                        e
                    );
                } else {
                    guard.file = Some(file);
                }
            }
            Err(e) => pr_warn!(
                "xART guard: the list of Linux's records could not be opened ({:?}); every existing session record is protected and none can be adopted\n",
                e
            ),
        }
        let sessions = guard.existed.iter().filter(|k| k.kind > 2).count();
        let own = guard
            .existed
            .iter()
            .filter(|k| k.kind > 2 && guard.owned.contains(k))
            .count();
        let roots = if level >= 2 {
            ", and so are the shared roots"
        } else {
            ""
        };
        pr_info!(
            "xART guard level {}: {} session records exist, {} created by Linux; the other {} are protected{}\n",
            level,
            sessions,
            own,
            sessions - own,
            roots
        );
        guard
    }

    fn load(&mut self, file: &F) -> Result<()> {
        let size = file.size()?;
        let physical = size / ENTRY_SIZE as u64;
        // Only the first MAX_ENTRIES are read. A longer list, which this
        // driver no longer writes, keeps the records listed there and takes
        // no new ones (see adopt()).
        let whole = physical.min(MAX_ENTRIES as u64) as usize;
        if physical > MAX_ENTRIES as u64 {
            pr_warn!(
                "xART guard: the list of Linux's records holds {} entries; only the first {} are read and no record can be adopted\n",
                physical,
                MAX_ENTRIES
            );
        }
        let mut torn = 0usize;
        let mut entry = [0u8; ENTRY_SIZE];
        for i in 0..whole {
            file.read_exact((i * ENTRY_SIZE) as u64, &mut entry)?;
            match decode(&entry) {
                Some(key) if !self.owned.contains(&key) => self.owned.push(key, GFP_KERNEL)?,
                Some(_) => {}
                None => torn += 1,
            }
        }
        if torn != 0 {
            pr_warn!("xART guard: ignored {} unreadable list entries\n", torn);
        }
        // A torn tail entry is overwritten by the next adoption.
        self.end = (whole * ENTRY_SIZE) as u64;
        Ok(())
    }

    fn adopt(&mut self, key: &Key) -> Result<()> {
        let file = self.file.as_ref().ok_or(ENOENT)?;
        // Bound the file, not the keys it lists: duplicate and unreadable
        // entries take room too, and the next boot reads no further.
        if self.end >= MAX_END {
            return Err(ENOSPC);
        }
        // Room for the key first, so that an entry made durable is always
        // listed in memory as well.
        self.owned.reserve(1, GFP_KERNEL)?;
        file.write_all(self.end, &encode(key))?;
        file.sync()?;
        self.owned.push_within_capacity(*key).map_err(|_| ENOMEM)?;
        // Only now: a failure above leaves the next adoption to overwrite
        // this slot, which holds no record the enclave wrote.
        self.end += ENTRY_SIZE as u64;
        Ok(())
    }

    /// Checks one enclave write (`write`) or delete before it reaches the
    /// gigalocker. An error refuses the request.
    pub(crate) fn check(&mut self, key: &Key, write: bool) -> Result<()> {
        let existed = self.existed.contains(key);
        let owned = self.owned.contains(key);
        match decide(self.level, key, existed, owned) {
            Verdict::Allow => Ok(()),
            Verdict::Adopt if !write => Ok(()),
            Verdict::Adopt => self.adopt(key).inspect_err(|e| {
                pr_warn!(
                    "xART guard: could not list a new kind-{} record as Linux's ({:?}); refusing to write it\n",
                    key.kind,
                    e
                )
            }),
            Verdict::Refuse => {
                self.refused += 1;
                if self.refused <= LOGGED_REFUSALS {
                    let action = if write { "rewrite" } else { "delete" };
                    pr_warn!(
                        "xART guard: refused to {} a kind-{} record this Linux install did not create\n",
                        action,
                        key.kind
                    );
                }
                Err(EPERM)
            }
        }
    }
}

#[kernel::macros::kunit_tests(apple_sep_xart_guard)]
mod tests {
    use super::*;
    use core::cell::{Cell, RefCell};
    use kernel::alloc::VVec;

    fn session(byte: u8) -> Key {
        Key::new(4, [byte; 16])
    }

    #[test]
    fn another_os_record_is_refused() {
        assert_eq!(decide(1, &session(1), true, false), Verdict::Refuse);
        assert_eq!(decide(2, &Key::new(3, [1; 16]), true, false), Verdict::Refuse);
    }

    #[test]
    fn linux_records_stay_writable() {
        assert_eq!(decide(1, &session(2), true, true), Verdict::Allow);
        assert_eq!(decide(1, &session(2), false, true), Verdict::Allow);
    }

    #[test]
    fn a_new_record_is_adopted() {
        assert_eq!(decide(1, &session(3), false, false), Verdict::Adopt);
    }

    #[test]
    fn roots_follow_the_level() {
        assert_eq!(decide(1, &Key::root(1), true, false), Verdict::Allow);
        assert_eq!(decide(1, &Key::root(2), true, false), Verdict::Allow);
        assert_eq!(decide(2, &Key::root(1), true, false), Verdict::Refuse);
        assert_eq!(decide(2, &Key::root(2), false, false), Verdict::Refuse);
    }

    #[test]
    fn entries_round_trip() {
        let key = Key::new(3, [0x5a; 16]);
        assert_eq!(decode(&encode(&key)), Some(key));
    }

    #[test]
    fn damaged_entries_are_ignored() {
        let key = session(7);
        let mut entry = encode(&key);
        entry[9] ^= 1;
        assert_eq!(decode(&entry), None);
        let mut root = encode(&Key::root(1));
        let crc = crc32_ieee(&root[..24]);
        root[24..28].copy_from_slice(&crc.to_le_bytes());
        assert_eq!(decode(&root), None);
        assert_eq!(decode(&encode(&key)[..31]), None);
    }

    /// Failures a [`MemFile`] reports, switchable while a guard holds it.
    #[derive(Default)]
    struct Faults {
        /// The write fails and changes nothing.
        write: Cell<bool>,
        /// The write lands, but the flush after it fails.
        sync: Cell<bool>,
        /// The directory entry cannot be flushed.
        parent: Cell<bool>,
    }

    /// A list file in memory, kept across the guards of successive boots.
    struct MemFile<'a>(&'a RefCell<VVec<u8>>, Option<&'a Faults>);

    fn mem(bytes: &RefCell<VVec<u8>>) -> MemFile<'_> {
        MemFile(bytes, None)
    }

    fn faulty<'a>(bytes: &'a RefCell<VVec<u8>>, faults: &'a Faults) -> MemFile<'a> {
        MemFile(bytes, Some(faults))
    }

    impl MemFile<'_> {
        fn fails(&self, fault: impl Fn(&Faults) -> &Cell<bool>) -> bool {
            self.1.is_some_and(|faults| fault(faults).get())
        }
    }

    impl ListFile for MemFile<'_> {
        fn size(&self) -> Result<u64> {
            Ok(self.0.borrow().len() as u64)
        }

        fn read_exact(&self, off: u64, buf: &mut [u8]) -> Result<()> {
            let bytes = self.0.borrow();
            let off = usize::try_from(off).map_err(|_| EINVAL)?;
            let end = off.checked_add(buf.len()).ok_or(EINVAL)?;
            buf.copy_from_slice(bytes.get(off..end).ok_or(EIO)?);
            Ok(())
        }

        fn write_all(&self, off: u64, buf: &[u8]) -> Result<()> {
            if self.fails(|f| &f.write) {
                return Err(EIO);
            }
            let mut bytes = self.0.borrow_mut();
            let off = usize::try_from(off).map_err(|_| EINVAL)?;
            let end = off.checked_add(buf.len()).ok_or(EINVAL)?;
            while bytes.len() < end {
                bytes.push(0, GFP_KERNEL)?;
            }
            bytes[off..end].copy_from_slice(buf);
            Ok(())
        }

        fn sync(&self) -> Result<()> {
            if self.fails(|f| &f.sync) {
                return Err(EIO);
            }
            Ok(())
        }

        fn sync_parent(&self) -> Result<()> {
            if self.fails(|f| &f.parent) {
                return Err(EIO);
            }
            Ok(())
        }
    }

    /// A list file of `count` copies of `entry`.
    fn list_of(entry: &[u8; ENTRY_SIZE], count: usize) -> Result<RefCell<VVec<u8>>> {
        let mut bytes = VVec::new();
        for _ in 0..count {
            bytes.extend_from_slice(entry, GFP_KERNEL)?;
        }
        Ok(RefCell::new(bytes))
    }

    fn keys(list: &[Key]) -> Result<KVec<Key>> {
        let mut keys = KVec::new();
        for key in list {
            keys.push(*key, GFP_KERNEL)?;
        }
        Ok(keys)
    }

    fn listed(bytes: &RefCell<VVec<u8>>) -> KVec<Option<Key>> {
        let mut entries = KVec::new();
        for entry in bytes.borrow().chunks(ENTRY_SIZE) {
            if entries.push(decode(entry), GFP_KERNEL).is_err() {
                break;
            }
        }
        entries
    }

    #[test]
    fn a_guard_refuses_another_os_record() -> Result<()> {
        let bytes = RefCell::new(VVec::new());
        let foreign = session(1);
        let mut guard = Guard::with_file(1, keys(&[foreign])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&foreign, true), Err(EPERM));
        assert_eq!(guard.check(&foreign, false), Err(EPERM));
        // A refusal lists nothing.
        assert_eq!(bytes.borrow().len(), 0);
        Ok(())
    }

    #[test]
    fn a_guard_allows_a_listed_record() -> Result<()> {
        let own = session(2);
        let bytes = list_of(&encode(&own), 1)?;
        let mut guard = Guard::with_file(1, keys(&[own])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&own, true), Ok(()));
        assert_eq!(guard.check(&own, false), Ok(()));
        // An entry that is already listed is not written again.
        assert_eq!(bytes.borrow().len(), ENTRY_SIZE);
        Ok(())
    }

    #[test]
    fn the_owned_list_survives_a_reboot() -> Result<()> {
        let bytes = RefCell::new(VVec::new());
        let foreign = session(1);
        let created = session(3);

        // First boot: the enclave creates a record, which is listed first.
        let mut guard = Guard::with_file(1, keys(&[foreign])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&created, true), Ok(()));
        assert_eq!(listed(&bytes).as_slice(), &[Some(created)]);
        drop(guard);

        // Next boot: both records predate the guard; only Linux's is writable.
        let mut guard = Guard::with_file(1, keys(&[foreign, created])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&created, true), Ok(()));
        assert_eq!(guard.check(&created, false), Ok(()));
        assert_eq!(guard.check(&foreign, true), Err(EPERM));
        assert_eq!(listed(&bytes).as_slice(), &[Some(created)]);
        Ok(())
    }

    #[test]
    fn a_torn_tail_entry_is_overwritten() -> Result<()> {
        let first = session(4);
        let second = session(5);
        let bytes = list_of(&encode(&first), 1)?;
        // A crash during the next append left part of an entry behind.
        bytes
            .borrow_mut()
            .extend_from_slice(&encode(&second)[..10], GFP_KERNEL)?;

        let mut guard = Guard::with_file(1, keys(&[first])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&first, true), Ok(()));
        assert_eq!(guard.check(&second, true), Ok(()));
        assert_eq!(listed(&bytes).as_slice(), &[Some(first), Some(second)]);
        Ok(())
    }

    #[test]
    fn without_the_list_nothing_is_adopted() -> Result<()> {
        let foreign = session(1);
        let mut guard = Guard::<MemFile<'_>>::with_file(1, keys(&[foreign])?, Err(ENOENT));
        assert_eq!(guard.check(&foreign, true), Err(EPERM));
        assert!(guard.check(&session(6), true).is_err());
        // Deleting a record that does not exist changes nothing.
        assert_eq!(guard.check(&session(6), false), Ok(()));
        assert_eq!(guard.check(&Key::root(1), true), Ok(()));
        Ok(())
    }

    #[test]
    fn a_full_list_of_duplicates_takes_no_more() -> Result<()> {
        let own = session(2);
        let bytes = list_of(&encode(&own), MAX_ENTRIES)?;
        let mut guard = Guard::with_file(1, keys(&[own])?, Ok(mem(&bytes)));
        // One key, but no room: the file is bounded, not its distinct keys.
        assert_eq!(guard.check(&own, true), Ok(()));
        assert_eq!(guard.check(&session(3), true), Err(ENOSPC));
        assert_eq!(bytes.borrow().len() as u64, MAX_END);
        drop(guard);

        // The next boot still reads the list: Linux's record stays writable.
        let mut guard = Guard::with_file(1, keys(&[own])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&own, true), Ok(()));
        assert_eq!(guard.check(&session(4), true), Err(ENOSPC));
        Ok(())
    }

    #[test]
    fn a_full_list_of_unreadable_entries_takes_no_more() -> Result<()> {
        let bytes = list_of(&[0u8; ENTRY_SIZE], MAX_ENTRIES)?;
        let mut guard = Guard::with_file(1, KVec::new(), Ok(mem(&bytes)));
        assert_eq!(guard.check(&session(3), true), Err(ENOSPC));
        assert_eq!(bytes.borrow().len() as u64, MAX_END);
        drop(guard);

        let mut guard = Guard::with_file(1, keys(&[session(1)])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&session(1), true), Err(EPERM));
        Ok(())
    }

    #[test]
    fn the_last_slot_is_used_and_read_back() -> Result<()> {
        let own = session(2);
        let last = session(3);
        let bytes = list_of(&encode(&own), MAX_ENTRIES - 1)?;
        let mut guard = Guard::with_file(1, keys(&[own])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&last, true), Ok(()));
        assert_eq!(bytes.borrow().len() as u64, MAX_END);
        assert_eq!(guard.check(&session(4), true), Err(ENOSPC));
        assert_eq!(bytes.borrow().len() as u64, MAX_END);
        drop(guard);

        let mut guard = Guard::with_file(1, keys(&[own, last])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&own, true), Ok(()));
        assert_eq!(guard.check(&last, true), Ok(()));
        assert_eq!(guard.check(&last, false), Ok(()));
        Ok(())
    }

    #[test]
    fn an_oversized_list_keeps_its_first_entries() -> Result<()> {
        let first = session(2);
        let beyond = session(3);
        // A list longer than this driver writes: a listed key in the first
        // entry, unreadable ones up to the bound, and a key past it.
        let bytes = list_of(&[0u8; ENTRY_SIZE], MAX_ENTRIES + 1)?;
        bytes.borrow_mut()[..ENTRY_SIZE].copy_from_slice(&encode(&first));
        bytes.borrow_mut()[MAX_ENTRIES * ENTRY_SIZE..].copy_from_slice(&encode(&beyond));
        let mut guard = Guard::with_file(1, keys(&[first, beyond])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&first, true), Ok(()));
        assert_eq!(guard.check(&beyond, true), Err(EPERM));
        assert_eq!(guard.check(&session(4), true), Err(ENOSPC));
        assert_eq!(bytes.borrow().len(), (MAX_ENTRIES + 1) * ENTRY_SIZE);
        Ok(())
    }

    #[test]
    fn a_failed_write_lists_nothing() -> Result<()> {
        let bytes = RefCell::new(VVec::new());
        let faults = Faults::default();
        let created = session(3);
        let mut guard = Guard::with_file(1, KVec::new(), Ok(faulty(&bytes, &faults)));
        faults.write.set(true);
        assert_eq!(guard.check(&created, true), Err(EIO));
        assert_eq!(bytes.borrow().len(), 0);
        faults.write.set(false);
        assert_eq!(guard.check(&created, true), Ok(()));
        assert_eq!(listed(&bytes).as_slice(), &[Some(created)]);
        Ok(())
    }

    #[test]
    fn a_failed_flush_is_retried_in_the_same_slot() -> Result<()> {
        let bytes = RefCell::new(VVec::new());
        let faults = Faults::default();
        let refused = session(3);
        let created = session(4);
        let mut guard = Guard::with_file(1, KVec::new(), Ok(faulty(&bytes, &faults)));
        faults.sync.set(true);
        assert_eq!(guard.check(&refused, true), Err(EIO));
        // Not listed in memory, so not writable either.
        assert_eq!(guard.check(&refused, true), Err(EIO));
        faults.sync.set(false);
        assert_eq!(guard.check(&created, true), Ok(()));
        assert_eq!(guard.check(&refused, true), Ok(()));
        // The unflushed entry was overwritten, not followed: no duplicate.
        assert_eq!(listed(&bytes).as_slice(), &[Some(created), Some(refused)]);
        drop(guard);

        let mut guard = Guard::with_file(1, keys(&[created, refused])?, Ok(mem(&bytes)));
        assert_eq!(guard.check(&created, true), Ok(()));
        assert_eq!(guard.check(&refused, true), Ok(()));
        Ok(())
    }

    #[test]
    fn without_a_durable_directory_entry_nothing_is_adopted() -> Result<()> {
        let own = session(2);
        let bytes = list_of(&encode(&own), 1)?;
        let faults = Faults::default();
        faults.parent.set(true);
        let mut guard = Guard::with_file(1, keys(&[own])?, Ok(faulty(&bytes, &faults)));
        // As without a list: every existing record is protected, and
        // nothing new is listed.
        assert_eq!(guard.check(&own, true), Err(EPERM));
        assert!(guard.check(&session(3), true).is_err());
        assert_eq!(bytes.borrow().len(), ENTRY_SIZE);
        Ok(())
    }

    #[test]
    fn the_default_follows_the_platform() {
        use crate::profile;

        // On where Linux shares the enclave with macOS.
        assert_eq!(level(-1, profile::T8103.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T6000.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T6020.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T8112.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T6020_SEPOS13.xart_guard), Some(1));
        // Off on the Neo, whose Linux records predate the list, and on the
        // M3 Air, which also holds such records and is not an M1 or M2.
        assert_eq!(level(-1, profile::T8140.xart_guard), None);
        assert_eq!(level(-1, profile::T8122.xart_guard), None);
        assert_eq!(level(i8::MIN, 1), Some(1));
    }

    #[test]
    fn siliconforest_leaves_the_guard_off_by_default() {
        use crate::profile;

        for profile in [
            &profile::T8103,
            &profile::T6000,
            &profile::T6020,
            &profile::T6020_SEPOS13,
            &profile::T8112,
            &profile::T8122,
            &profile::T8140,
        ] {
            assert_eq!(level(-1, default_level(profile.xart_guard)), None);
            // Opting in still works on every platform.
            assert_eq!(level(1, default_level(profile.xart_guard)), Some(1));
            assert_eq!(level(2, default_level(profile.xart_guard)), Some(2));
        }
    }

    #[test]
    fn an_explicit_level_overrides_the_platform() {
        assert_eq!(level(0, 1), None);
        assert_eq!(level(1, 0), Some(1));
        assert_eq!(level(2, 1), Some(2));
        assert_eq!(level(i8::MAX, 0), Some(2));
    }
}

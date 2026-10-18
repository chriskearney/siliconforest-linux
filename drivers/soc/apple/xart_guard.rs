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
//! The parameter's default, -1, takes the platform profile's level: 1 on the
//! M1 and M2 Macs, whose enclave Linux shares with a macOS install, and 0 on
//! the MacBook Neo, which already holds Linux records from before the list
//! existed and would otherwise lose write access to them.
//!
//! The list entry is made durable before the enclave's write reaches the
//! gigalocker. A crash between the two leaves an entry for a record that was
//! never written, which is harmless. It can never leave a Linux record that
//! the next boot treats as another OS's.

use crate::shim::StoreFile;
use crate::xart_store::{crc32_ieee, Key};
use kernel::prelude::*;

/// Host file listing the session records Linux created.
pub(crate) const OWNED_PATH: &CStr = c"/var/lib/aurora-sep-xart-owned.bin";

const ENTRY_SIZE: usize = 32;
const ENTRY_MAGIC: &[u8; 4] = b"AXGO";
/// Bounds the file the driver reads at probe. The gigalocker has at most a
/// few hundred slots, so a longer list is damaged.
const MAX_ENTRIES: usize = 4096;
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
        let whole = (size as usize / ENTRY_SIZE).min(MAX_ENTRIES + 1);
        if whole > MAX_ENTRIES {
            return Err(EFBIG);
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
        if self.owned.len() >= MAX_ENTRIES {
            return Err(ENOSPC);
        }
        file.write_all(self.end, &encode(key))?;
        file.sync()?;
        self.end += ENTRY_SIZE as u64;
        self.owned.push(*key, GFP_KERNEL)?;
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
    use core::cell::RefCell;

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

    /// A list file in memory, kept across the guards of successive boots.
    struct MemFile<'a>(&'a RefCell<KVec<u8>>);

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
            Ok(())
        }
    }

    fn keys(list: &[Key]) -> Result<KVec<Key>> {
        let mut keys = KVec::new();
        for key in list {
            keys.push(*key, GFP_KERNEL)?;
        }
        Ok(keys)
    }

    fn listed(bytes: &RefCell<KVec<u8>>) -> KVec<Option<Key>> {
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
        let bytes = RefCell::new(KVec::new());
        let foreign = session(1);
        let mut guard = Guard::with_file(1, keys(&[foreign])?, Ok(MemFile(&bytes)));
        assert_eq!(guard.check(&foreign, true), Err(EPERM));
        assert_eq!(guard.check(&foreign, false), Err(EPERM));
        // A refusal lists nothing.
        assert_eq!(bytes.borrow().len(), 0);
        Ok(())
    }

    #[test]
    fn a_guard_allows_a_listed_record() -> Result<()> {
        let own = session(2);
        let mut list = KVec::new();
        list.extend_from_slice(&encode(&own), GFP_KERNEL)?;
        let bytes = RefCell::new(list);
        let mut guard = Guard::with_file(1, keys(&[own])?, Ok(MemFile(&bytes)));
        assert_eq!(guard.check(&own, true), Ok(()));
        assert_eq!(guard.check(&own, false), Ok(()));
        // An entry that is already listed is not written again.
        assert_eq!(bytes.borrow().len(), ENTRY_SIZE);
        Ok(())
    }

    #[test]
    fn the_owned_list_survives_a_reboot() -> Result<()> {
        let bytes = RefCell::new(KVec::new());
        let foreign = session(1);
        let created = session(3);

        // First boot: the enclave creates a record, which is listed first.
        let mut guard = Guard::with_file(1, keys(&[foreign])?, Ok(MemFile(&bytes)));
        assert_eq!(guard.check(&created, true), Ok(()));
        assert_eq!(listed(&bytes).as_slice(), &[Some(created)]);
        drop(guard);

        // Next boot: both records predate the guard; only Linux's is writable.
        let mut guard = Guard::with_file(1, keys(&[foreign, created])?, Ok(MemFile(&bytes)));
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
        let mut list = KVec::new();
        list.extend_from_slice(&encode(&first), GFP_KERNEL)?;
        // A crash during the next append left part of an entry behind.
        list.extend_from_slice(&encode(&second)[..10], GFP_KERNEL)?;
        let bytes = RefCell::new(list);

        let mut guard = Guard::with_file(1, keys(&[first])?, Ok(MemFile(&bytes)));
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
    fn the_default_follows_the_platform() {
        use crate::profile;

        // On where Linux shares the enclave with macOS.
        assert_eq!(level(-1, profile::T8103.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T6000.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T6020.xart_guard), Some(1));
        assert_eq!(level(-1, profile::T8112.xart_guard), Some(1));
        // Off on the Neo, whose Linux records predate the list.
        assert_eq!(level(-1, profile::T8140.xart_guard), None);
        assert_eq!(level(i8::MIN, 1), Some(1));
    }

    #[test]
    fn an_explicit_level_overrides_the_platform() {
        assert_eq!(level(0, 1), None);
        assert_eq!(level(1, 0), Some(1));
        assert_eq!(level(2, 1), Some(2));
        assert_eq!(level(i8::MAX, 0), Some(2));
    }
}

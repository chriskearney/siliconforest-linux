// SPDX-License-Identifier: GPL-2.0 OR MIT

//! DRM subsystem abstractions.

pub mod device;
pub mod driver;
pub mod file;
pub mod gem;
// SiliconForest's GPU drivers use the downstream GPUVM abstraction in
// gpuvm.rs. Upstream's immediate-mode abstraction, which has no in-tree
// users in 7.2 and is gated by CONFIG_RUST_DRM_GPUVM, lives in gpuvm/ and
// is kept under its own name so both module files can coexist.
#[cfg(CONFIG_DRM_GPUVM = "y")]
#[path = "gpuvm.rs"]
pub mod gpuvm;
#[path = "gpuvm/mod.rs"]
pub mod gpuvm_immediate;
pub mod ioctl;
pub mod mm;
pub mod sched;
pub mod syncobj;

pub use self::device::Device;
pub use self::device::DeviceContext;
pub use self::device::Registered;
pub use self::device::Uninit;
pub use self::device::UnregisteredDevice;
pub use self::driver::Driver;
pub use self::driver::DriverInfo;
pub use self::driver::Registration;
pub use self::file::File;

pub(crate) mod private {
    pub trait Sealed {}
}

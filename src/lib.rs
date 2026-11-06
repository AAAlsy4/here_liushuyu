// TODO: Always allow dead code for now, as we want to release the crate while many dead code exists.
#![allow(dead_code)]
// Only Allow dead code in debug mode
// #![cfg_attr(debug_assertions, allow(dead_code))]
#![allow(incomplete_features)]
#![feature(generic_const_exprs)]
#![feature(macro_metavar_expr_concat)]
#![feature(likely_unlikely)]
#![feature(unsafe_cell_access)]

#[cfg(all(feature = "native-cli", target_arch = "wasm32"))]
compile_error!("feature 'native-cli' is not supported on wasm32 targets");

mod clock;
mod cpu;
mod fpu;
mod utils;

#[cfg(feature = "native-cli")]
pub mod gdb;

#[cfg(any(feature = "native-cli", target_arch = "wasm32"))]
pub mod rvdb;

pub mod board;
pub mod byte_io;
pub mod config;
pub mod device;
pub mod isa;
pub mod load;
pub mod ram;
#[cfg(not(target_arch = "wasm32"))]
pub mod task_spawner;

#[cfg(target_arch = "wasm32")]
pub mod wasm_api;

pub use config::ram_config;

use crate::device::virtio::virtio_device::VirtIODeviceEnum;
use std::{path::PathBuf, str::FromStr};

#[derive(Debug, Clone)]
pub struct DeviceConfig {
    pub dev_type: VirtIODeviceEnum,
    pub path: PathBuf,
}

impl FromStr for DeviceConfig {
    type Err = String;

    fn from_str(s: &str) -> Result<Self, Self::Err> {
        let (dev_type_name, path) = match s.split_once(':') {
            Some((dev_type, path)) => (dev_type, Some(path)),
            None => (s, None),
        };
        let dev_type = match dev_type_name {
            "virtio-block" => VirtIODeviceEnum::VirtIOBlock,
            "virtio-network" => VirtIODeviceEnum::VirtIONet,
            "virtio-rng" => VirtIODeviceEnum::VirtIORng,
            other => return Err(format!("Unknown device type: {other}")),
        };
        let path = match dev_type {
            VirtIODeviceEnum::VirtIORng => PathBuf::new(),
            _ => PathBuf::from(path.ok_or("Need input a device path.")?),
        };
        Ok(DeviceConfig { dev_type, path })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_virtio_rng_without_a_path() {
        let config = "virtio-rng".parse::<DeviceConfig>().unwrap();
        assert_eq!(config.dev_type, VirtIODeviceEnum::VirtIORng);
        assert!(config.path.as_os_str().is_empty());
    }

    #[test]
    fn block_device_still_requires_a_path() {
        assert!("virtio-block".parse::<DeviceConfig>().is_err());
        let config = "virtio-block:./disk.img".parse::<DeviceConfig>().unwrap();
        assert_eq!(config.dev_type, VirtIODeviceEnum::VirtIOBlock);
        assert_eq!(config.path, PathBuf::from("./disk.img"));
    }
}

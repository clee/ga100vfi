//! Scoped Linux GPU0 BAR0 lease. Compiled only with `linux-bar0` and never
//! selected by the simulator target binary.
use crate::{BAR0_MAP_LENGTH, FUSEADDR, FUSECTRL, Gpu0Bar, ROW_COUNT, valid_bdf};
use memmap2::{MmapMut, MmapOptions};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{Ordering, fence};
use thiserror::Error;
use uuid::Uuid;

#[derive(Debug, Error)]
pub enum LinuxBarError {
    #[error("invalid GPU0 BDF or identity")]
    Identity,
    #[error("GPU0 busy: driver_bound={driver_bound} PCI_COMMAND=0x{command:04x} enable={enable}")]
    Busy {
        driver_bound: bool,
        command: u16,
        enable: u32,
    },
    #[error("BAR0 is smaller than the scoped capture map")]
    ShortBar,
    #[error("GPU0 BAR0 offset is out of scope")]
    Offset,
    #[error("GPU0 sysfs I/O: {0}")]
    Io(#[from] std::io::Error),
    #[error("GPU0 boot identity changed")]
    BootChanged,
    #[error("GPU0 PCI state could not be restored")]
    Restore,
}

fn config_bytes(device: &Path, offset: u64, length: usize) -> Result<Vec<u8>, LinuxBarError> {
    let mut file = File::open(device.join("config"))?;
    file.seek(SeekFrom::Start(offset))?;
    let mut bytes = vec![0; length];
    file.read_exact(&mut bytes)?;
    Ok(bytes)
}

fn pci_id(device: &Path) -> Result<u32, LinuxBarError> {
    let bytes = config_bytes(device, 0, 4)?;
    Ok(u32::from_le_bytes(
        bytes.try_into().map_err(|_| LinuxBarError::Identity)?,
    ))
}

fn pci_command(device: &Path) -> Result<u16, LinuxBarError> {
    let bytes = config_bytes(device, 4, 2)?;
    Ok(u16::from_le_bytes(
        bytes.try_into().map_err(|_| LinuxBarError::Identity)?,
    ))
}

fn enable_count(device: &Path) -> Result<u32, LinuxBarError> {
    fs::read_to_string(device.join("enable"))?
        .trim()
        .parse()
        .map_err(|_| LinuxBarError::Identity)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PciState {
    pub pci_id: u32,
    pub command: u16,
    pub enable: u32,
    pub driver_bound: bool,
}

fn inspect_at(device: &Path, allowed_ids: &[u32]) -> Result<PciState, LinuxBarError> {
    let id = pci_id(device)?;
    if id & 0xffff != 0x10de || !allowed_ids.contains(&id) {
        return Err(LinuxBarError::Identity);
    }
    Ok(PciState {
        pci_id: id,
        command: pci_command(device)?,
        enable: enable_count(device)?,
        driver_bound: device.join("driver").exists(),
    })
}

pub fn inspect_scoped(
    bdf: &str,
    allowed_ids: &[u32],
    expected_boot: Uuid,
) -> Result<PciState, LinuxBarError> {
    if !valid_bdf(bdf) || allowed_ids.is_empty() || expected_boot.is_nil() {
        return Err(LinuxBarError::Identity);
    }
    if boot_id()? != expected_boot {
        return Err(LinuxBarError::BootChanged);
    }
    inspect_at(&Path::new("/sys/bus/pci/devices").join(bdf), allowed_ids)
}

fn clear_firmware_command_at(
    device: &Path,
    allowed_ids: &[u32],
) -> Result<PciState, LinuxBarError> {
    let before = inspect_at(device, allowed_ids)?;
    if before.driver_bound || before.enable != 0 || before.command != 0x0007 {
        return Err(LinuxBarError::Busy {
            driver_bound: before.driver_bound,
            command: before.command,
            enable: before.enable,
        });
    }
    write_pci_command_zero(device)?;
    let after = inspect_at(device, allowed_ids).map_err(|_| LinuxBarError::Restore)?;
    if after.driver_bound || after.enable != 0 || after.command != 0 {
        return Err(LinuxBarError::Restore);
    }
    Ok(after)
}

/// Called only after the target has fsynced a one-use boot-scoped intent.
/// It accepts exactly the observed firmware-owned command 0x0007.
pub fn clear_firmware_command(
    bdf: &str,
    allowed_ids: &[u32],
    expected_boot: Uuid,
) -> Result<PciState, LinuxBarError> {
    if !valid_bdf(bdf) || allowed_ids.is_empty() || expected_boot.is_nil() {
        return Err(LinuxBarError::Identity);
    }
    if boot_id()? != expected_boot {
        return Err(LinuxBarError::BootChanged);
    }
    let after =
        clear_firmware_command_at(&Path::new("/sys/bus/pci/devices").join(bdf), allowed_ids)?;
    if boot_id()? != expected_boot {
        return Err(LinuxBarError::BootChanged);
    }
    Ok(after)
}

fn write_pci_command_zero(device: &Path) -> Result<(), LinuxBarError> {
    let mut file = OpenOptions::new().write(true).open(device.join("config"))?;
    file.seek(SeekFrom::Start(4))?;
    file.write_all(&0_u16.to_le_bytes())?;
    Ok(())
}

fn boot_id() -> Result<Uuid, LinuxBarError> {
    Uuid::parse_str(fs::read_to_string("/proc/sys/kernel/random/boot_id")?.trim())
        .map_err(|_| LinuxBarError::Identity)
}

fn restore_device(
    device: &Path,
    expected_id: u32,
    expected_boot: Uuid,
) -> Result<(), LinuxBarError> {
    fs::write(device.join("enable"), b"0\n").map_err(|_| LinuxBarError::Restore)?;
    let command = pci_command(device).map_err(|_| LinuxBarError::Restore)?;
    if command == 2 || command == 6 {
        write_pci_command_zero(device).map_err(|_| LinuxBarError::Restore)?;
    } else if command != 0 {
        return Err(LinuxBarError::Restore);
    }
    if fs::read_to_string(device.join("enable"))
        .map_err(|_| LinuxBarError::Restore)?
        .trim()
        != "0"
        || pci_command(device).map_err(|_| LinuxBarError::Restore)? != 0
        || pci_id(device).map_err(|_| LinuxBarError::Restore)? != expected_id
    {
        return Err(LinuxBarError::Restore);
    }
    if boot_id()? != expected_boot {
        return Err(LinuxBarError::BootChanged);
    }
    Ok(())
}

/// This lease maps only `/sys/bus/pci/devices/<selected GPU0 BDF>/resource0`.
/// It never enumerates or accesses other PCI devices.
pub struct LinuxBar {
    device: PathBuf,
    expected_id: u32,
    expected_boot: Uuid,
    map: Option<MmapMut>,
    resource: Option<File>,
    active: bool,
}

impl LinuxBar {
    pub fn open(
        bdf: &str,
        allowed_ids: &[u32],
        expected_boot: Uuid,
    ) -> Result<Self, LinuxBarError> {
        if !valid_bdf(bdf) || allowed_ids.is_empty() || expected_boot.is_nil() {
            return Err(LinuxBarError::Identity);
        }
        if boot_id()? != expected_boot {
            return Err(LinuxBarError::BootChanged);
        }
        let device = Path::new("/sys/bus/pci/devices").join(bdf);
        let state = inspect_at(&device, allowed_ids)?;
        if state.driver_bound || state.command != 0 || state.enable != 0 {
            return Err(LinuxBarError::Busy {
                driver_bound: state.driver_bound,
                command: state.command,
                enable: state.enable,
            });
        }
        fs::write(device.join("enable"), b"1\n")?;
        let mapped = (|| -> Result<(File, MmapMut), LinuxBarError> {
            let resource = OpenOptions::new()
                .read(true)
                .write(true)
                .custom_flags(libc::O_SYNC)
                .open(device.join("resource0"))?;
            if resource.metadata()?.len() < BAR0_MAP_LENGTH as u64 {
                return Err(LinuxBarError::ShortBar);
            }
            let map = unsafe { MmapOptions::new().len(BAR0_MAP_LENGTH).map_mut(&resource)? };
            Ok((resource, map))
        })();
        let (resource, map) = match mapped {
            Ok(value) => value,
            Err(error) => {
                restore_device(&device, state.pci_id, expected_boot)?;
                return Err(error);
            }
        };
        Ok(Self {
            device,
            expected_id: state.pci_id,
            expected_boot,
            map: Some(map),
            resource: Some(resource),
            active: true,
        })
    }

    fn read_word(&self, offset: u32) -> Result<u32, LinuxBarError> {
        if !offset.is_multiple_of(4) || offset as usize + 4 > BAR0_MAP_LENGTH {
            return Err(LinuxBarError::Offset);
        }
        let map = self.map.as_ref().ok_or(LinuxBarError::Restore)?;
        let ptr = unsafe { map.as_ptr().add(offset as usize) as *const u32 };
        Ok(u32::from_le(unsafe { ptr.read_volatile() }))
    }

    fn write_word(&mut self, offset: u32, value: u32) -> Result<(), LinuxBarError> {
        if !offset.is_multiple_of(4) || offset as usize + 4 > BAR0_MAP_LENGTH {
            return Err(LinuxBarError::Offset);
        }
        let map = self.map.as_mut().ok_or(LinuxBarError::Restore)?;
        let ptr = unsafe { map.as_mut_ptr().add(offset as usize) as *mut u32 };
        unsafe { ptr.write_volatile(value.to_le()) };
        fence(Ordering::SeqCst);
        Ok(())
    }

    fn close(&mut self) -> Result<(), LinuxBarError> {
        if !self.active {
            return Ok(());
        }
        self.map.take();
        self.resource.take();
        restore_device(&self.device, self.expected_id, self.expected_boot)?;
        self.active = false;
        Ok(())
    }

    pub fn finish(mut self) -> Result<(), LinuxBarError> {
        let result = self.close();
        /* An explicit close may fail after changing PCI state. Report that
         * uncertainty once; Drop must not issue a hidden second restore. */
        self.active = false;
        result
    }
}

impl Gpu0Bar for LinuxBar {
    fn read32(&mut self, offset: u32) -> Result<u32, String> {
        self.read_word(offset).map_err(|error| error.to_string())
    }

    fn select_fuse_row(&mut self, row: u16) -> Result<(), String> {
        if usize::from(row) >= ROW_COUNT {
            return Err("row outside full512".into());
        }
        self.write_word(FUSEADDR, u32::from(row))
            .map_err(|error| error.to_string())
    }

    fn issue_cmd_read(&mut self) -> Result<(), String> {
        self.write_word(FUSECTRL, 1)
            .map_err(|error| error.to_string())
    }

    fn restore_fuse_address(&mut self, original: u32) -> Result<(), String> {
        self.write_word(FUSEADDR, original)
            .map_err(|error| error.to_string())
    }
}

impl Drop for LinuxBar {
    fn drop(&mut self) {
        let _ = self.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(command: u16, enable: u32, driver_bound: bool) -> PathBuf {
        let path = std::env::temp_dir().join(format!("gpu0-pci-prep-{}", Uuid::new_v4()));
        fs::create_dir(&path).unwrap();
        let mut config = [0u8; 64];
        config[..4].copy_from_slice(&0x20c2_10de_u32.to_le_bytes());
        config[4..6].copy_from_slice(&command.to_le_bytes());
        fs::write(path.join("config"), config).unwrap();
        fs::write(path.join("enable"), format!("{enable}\n")).unwrap();
        if driver_bound {
            fs::write(path.join("driver"), b"bound").unwrap();
        }
        path
    }

    #[test]
    fn clears_only_unbound_firmware_command_seven() {
        let path = fixture(7, 0, false);
        let after = clear_firmware_command_at(&path, &[0x20c2_10de]).unwrap();
        assert_eq!(after.command, 0);
        assert_eq!(after.enable, 0);
        assert!(!after.driver_bound);
        assert_eq!(pci_command(&path).unwrap(), 0);
        fs::remove_dir_all(path).unwrap();
    }

    #[test]
    fn refuses_other_commands_enable_counts_and_bound_drivers_without_writing() {
        for (command, enable, bound) in [(6, 0, false), (7, 1, false), (7, 0, true)] {
            let path = fixture(command, enable, bound);
            let before = fs::read(path.join("config")).unwrap();
            assert!(matches!(
                clear_firmware_command_at(&path, &[0x20c2_10de]),
                Err(LinuxBarError::Busy { .. })
            ));
            assert_eq!(fs::read(path.join("config")).unwrap(), before);
            fs::remove_dir_all(path).unwrap();
        }
    }
}

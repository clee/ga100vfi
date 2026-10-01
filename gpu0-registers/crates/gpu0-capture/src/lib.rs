//! GPU0-only readout engine. Its adapter cannot express a fuse write.
pub mod baseline;
#[cfg(all(target_os = "linux", feature = "linux-bar0"))]
pub mod linux;
pub mod maps;
use glitch_contract::Digest;
use std::time::{Duration, Instant};
use thiserror::Error;

pub const BAR0_MAP_LENGTH: usize = 0x95d000;
pub const PMC_BOOT_0: u32 = 0x0;
pub const FUSECTRL: u32 = 0x820000;
pub const FUSEADDR: u32 = 0x820004;
pub const FUSEDATA: u32 = 0x820008;
pub const FUSEDEBUG: u32 = 0x820060;
pub const ROW_COUNT: usize = 512;

pub fn valid_bdf(bdf: &str) -> bool {
    glitch_contract::valid_pci_bdf(bdf)
}

/// A GPU0-scoped BAR adapter. Only these two command writes are expressible.
/// The implementation must bind one verified BDF and reject any other device.
pub trait Gpu0Bar {
    fn read32(&mut self, offset: u32) -> Result<u32, String>;
    fn select_fuse_row(&mut self, row: u16) -> Result<(), String>;
    fn issue_cmd_read(&mut self) -> Result<(), String>;
    fn restore_fuse_address(&mut self, original: u32) -> Result<(), String>;
}

#[derive(Clone, Copy, Debug)]
pub struct Policy {
    pub per_row_timeout: Duration,
    pub total_timeout: Duration,
}

impl Policy {
    pub const fn legacy_readonly() -> Self {
        Self {
            per_row_timeout: Duration::from_millis(200),
            total_timeout: Duration::from_secs(20),
        }
    }
}

#[derive(Debug, Error, Eq, PartialEq)]
pub enum CaptureError {
    #[error("GPU0 BAR read/write failed: {0}")]
    Io(String),
    #[error("GPU0 BAR0 has an inaccessible sentinel")]
    Sentinel,
    #[error("fuse controller was not idle before row {0}")]
    NotIdle(u16),
    #[error("fuse address did not latch for row {0}")]
    AddressMismatch(u16),
    #[error("fuse read timed out at row {0}")]
    RowTimeout(u16),
    #[error("full row capture exceeded its deadline")]
    TotalTimeout,
    #[error("failed to restore the prior fuse address: {0}")]
    Restore(String),
    #[error("invalid capture deadline policy")]
    Policy,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Full512 {
    pub first: Vec<u32>,
    pub second: Vec<u32>,
    pub first_sha256_le_u32: Digest,
    pub second_sha256_le_u32: Digest,
    pub two_pass_equal: bool,
    pub original_address: u32,
}

/// Keeps every word read before a failure. An incomplete observation is
/// evidence, but cannot enroll a boot or establish a negative image.
#[derive(Debug, Eq, PartialEq)]
pub struct PartialFull512 {
    pub first: Vec<u32>,
    pub second: Vec<u32>,
    pub original_address: Option<u32>,
    pub read_error: Option<CaptureError>,
    pub restore_error: Option<CaptureError>,
}

fn read<B: Gpu0Bar>(bar: &mut B, offset: u32) -> Result<u32, CaptureError> {
    bar.read32(offset).map_err(CaptureError::Io)
}

fn idle(control: u32) -> bool {
    ((control >> 16) & 31) == 4 && control & 3 == 0
}

fn pass<B: Gpu0Bar>(
    bar: &mut B,
    policy: Policy,
    start: Instant,
    words: &mut Vec<u32>,
) -> Result<(), CaptureError> {
    for index in 0..ROW_COUNT {
        let row = index as u16;
        if start.elapsed() > policy.total_timeout {
            return Err(CaptureError::TotalTimeout);
        }
        if !idle(read(bar, FUSECTRL)?) || read(bar, FUSEDEBUG)? != 0 {
            return Err(CaptureError::NotIdle(row));
        }
        bar.select_fuse_row(row).map_err(CaptureError::Io)?;
        if read(bar, FUSEADDR)? != u32::from(row) {
            return Err(CaptureError::AddressMismatch(row));
        }
        bar.issue_cmd_read().map_err(CaptureError::Io)?;
        let row_start = Instant::now();
        loop {
            if idle(read(bar, FUSECTRL)?) {
                break;
            }
            if row_start.elapsed() > policy.per_row_timeout {
                return Err(CaptureError::RowTimeout(row));
            }
            std::thread::yield_now();
        }
        words.push(read(bar, FUSEDATA)?);
    }
    Ok(())
}

fn word_digest(words: &[u32]) -> Digest {
    let mut bytes = Vec::with_capacity(words.len() * 4);
    for word in words {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    Digest::of(bytes)
}

/// Two independent bounded passes. Every successfully read row survives a
/// later error. Restoration is attempted even if a pass stopped early.
pub fn capture_full512_partial<B: Gpu0Bar>(bar: &mut B, policy: Policy) -> PartialFull512 {
    let mut observation = PartialFull512 {
        first: Vec::with_capacity(ROW_COUNT),
        second: Vec::with_capacity(ROW_COUNT),
        original_address: None,
        read_error: None,
        restore_error: None,
    };
    if policy.per_row_timeout.is_zero()
        || policy.total_timeout.is_zero()
        || policy.per_row_timeout > Duration::from_secs(1)
        || policy.total_timeout > Duration::from_secs(60)
    {
        observation.read_error = Some(CaptureError::Policy);
        return observation;
    }
    let pmc = match read(bar, PMC_BOOT_0) {
        Ok(value) => value,
        Err(error) => {
            observation.read_error = Some(error);
            return observation;
        }
    };
    if pmc == 0 || pmc == u32::MAX || pmc & 0xffff_0000 == 0xbadf_0000 {
        observation.read_error = Some(CaptureError::Sentinel);
        return observation;
    }
    let original_address = match read(bar, FUSEADDR) {
        Ok(value) => value,
        Err(error) => {
            observation.read_error = Some(error);
            return observation;
        }
    };
    observation.original_address = Some(original_address);
    let start = Instant::now();
    observation.read_error = pass(bar, policy, start, &mut observation.first)
        .and_then(|()| pass(bar, policy, start, &mut observation.second))
        .err();
    observation.restore_error = bar
        .restore_fuse_address(original_address)
        .map_err(CaptureError::Restore)
        .err();
    observation
}

/// Compatibility wrapper for callers that require a complete image. A
/// restore failure takes precedence because the device may be left modified.
pub fn capture_full512<B: Gpu0Bar>(bar: &mut B, policy: Policy) -> Result<Full512, CaptureError> {
    let observation = capture_full512_partial(bar, policy);
    if let Some(error) = observation.restore_error {
        return Err(error);
    }
    if let Some(error) = observation.read_error {
        return Err(error);
    }
    let original_address = observation.original_address.ok_or(CaptureError::Restore(
        "original fuse address missing".into(),
    ))?;
    let first = observation.first;
    let second = observation.second;
    Ok(Full512 {
        first_sha256_le_u32: word_digest(&first),
        second_sha256_le_u32: word_digest(&second),
        two_pass_equal: first == second,
        first,
        second,
        original_address,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    struct FakeBar {
        address: u32,
        data: u32,
        control: u32,
        debug: u32,
        issued: usize,
        changed_second_pass: bool,
        fail_at_issue: Option<usize>,
        fail_restore: bool,
        fail_core_read: bool,
        flip_sbfe_after_first: bool,
        sbfe_reads: usize,
    }

    impl Default for FakeBar {
        fn default() -> Self {
            Self {
                address: 77,
                data: 0,
                control: 0xe0040000,
                debug: 0,
                issued: 0,
                changed_second_pass: false,
                fail_at_issue: None,
                fail_restore: false,
                fail_core_read: false,
                flip_sbfe_after_first: false,
                sbfe_reads: 0,
            }
        }
    }

    impl Gpu0Bar for FakeBar {
        fn read32(&mut self, offset: u32) -> Result<u32, String> {
            if self.fail_core_read && offset == 0x8200f8 {
                return Err("injected controller read failure".into());
            }
            if offset == 0x821108 && self.flip_sbfe_after_first {
                self.sbfe_reads += 1;
                return Ok(offset ^ 0x5a5a_5a5a ^ u32::from(self.sbfe_reads > 1));
            }
            match offset {
                PMC_BOOT_0 => Ok(0x1000_0001),
                FUSECTRL => Ok(self.control),
                FUSEADDR => Ok(self.address),
                FUSEDATA => Ok(self.data),
                FUSEDEBUG => Ok(self.debug),
                _ => Ok(offset ^ 0x5a5a_5a5a),
            }
        }
        fn select_fuse_row(&mut self, row: u16) -> Result<(), String> {
            self.address = u32::from(row);
            Ok(())
        }
        fn issue_cmd_read(&mut self) -> Result<(), String> {
            if self.fail_at_issue == Some(self.issued) {
                return Err("injected read interruption".into());
            }
            self.issued += 1;
            self.data = self.address
                ^ if self.changed_second_pass && self.issued > ROW_COUNT {
                    1
                } else {
                    0
                };
            Ok(())
        }
        fn restore_fuse_address(&mut self, original: u32) -> Result<(), String> {
            if self.fail_restore {
                return Err("injected restore failure".into());
            }
            self.address = original;
            Ok(())
        }
    }

    #[test]
    fn full512_reads_twice_and_restores_address() {
        let mut bar = FakeBar::default();
        let capture = capture_full512(&mut bar, Policy::legacy_readonly()).unwrap();
        assert_eq!(capture.first.len(), ROW_COUNT);
        assert_eq!(capture.second.len(), ROW_COUNT);
        assert!(capture.two_pass_equal);
        assert_eq!(capture.first[18], 18);
        assert_eq!(bar.issued, ROW_COUNT * 2);
        assert_eq!(bar.address, 77);
    }

    #[test]
    fn unstable_second_pass_is_preserved_as_data() {
        let mut bar = FakeBar {
            changed_second_pass: true,
            ..FakeBar::default()
        };
        let capture = capture_full512(&mut bar, Policy::legacy_readonly()).unwrap();
        assert!(!capture.two_pass_equal);
        assert_ne!(capture.first_sha256_le_u32, capture.second_sha256_le_u32);
    }

    #[test]
    fn interrupted_second_pass_keeps_first_image_and_partial_rows() {
        let mut bar = FakeBar {
            fail_at_issue: Some(ROW_COUNT + 7),
            ..FakeBar::default()
        };
        let capture = capture_full512_partial(&mut bar, Policy::legacy_readonly());
        assert_eq!(capture.first.len(), ROW_COUNT);
        assert_eq!(capture.second.len(), 7);
        assert_eq!(capture.second[6], 6);
        assert_eq!(
            capture.read_error,
            Some(CaptureError::Io("injected read interruption".into()))
        );
        assert_eq!(capture.restore_error, None);
        assert_eq!(bar.address, 77);
    }

    #[test]
    fn restore_failure_is_recorded_separately() {
        let mut bar = FakeBar {
            fail_at_issue: Some(18),
            fail_restore: true,
            ..FakeBar::default()
        };
        let capture = capture_full512_partial(&mut bar, Policy::legacy_readonly());
        assert_eq!(capture.first.len(), 18);
        assert!(matches!(capture.read_error, Some(CaptureError::Io(_))));
        assert!(matches!(
            capture.restore_error,
            Some(CaptureError::Restore(_))
        ));
        assert_eq!(bar.address, 18);
    }

    #[test]
    fn baseline_record_preserves_interrupted_rows_without_enrollment() {
        use crate::baseline::{BaselineSource, capture_baseline, failed_before_open};
        let source = BaselineSource {
            capture_id: uuid::Uuid::new_v4(),
            boot_id: uuid::Uuid::new_v4(),
            sample_id: Digest::of(b"sample"),
            image_digest: Digest::of(b"image"),
            calibration_digest: Digest::of(b"calibration"),
        };
        let mut bar = FakeBar {
            fail_at_issue: Some(ROW_COUNT + 7),
            ..FakeBar::default()
        };
        let evidence = capture_baseline(&mut bar, source, Policy::legacy_readonly()).unwrap();
        assert_eq!(evidence.rows_first.len(), ROW_COUNT);
        assert_eq!(evidence.rows_second.len(), 7);
        assert_eq!(evidence.options_before.len(), 672);
        assert_eq!(evidence.statuses_before.len(), 84);
        assert!(evidence.options_after.is_empty());
        assert!(evidence.failure.as_deref().unwrap().contains("row read"));
        assert_eq!(evidence.stable_fingerprint().unwrap(), None);
        assert_eq!(bar.address, 77);
        let complete =
            capture_baseline(&mut FakeBar::default(), source, Policy::legacy_readonly()).unwrap();
        assert_eq!(complete.failure, None);
        assert!(complete.stable_fingerprint().unwrap().is_some());
        let inaccessible = failed_before_open(source, "device not present").unwrap();
        assert!(inaccessible.rows_first.is_empty());
        assert!(inaccessible.options_before.is_empty());
        assert!(inaccessible.failure.unwrap().contains("device not present"));
    }

    #[test]
    fn provisional_capture_uses_the_same_bounded_read_without_sample_or_calibration() {
        use crate::baseline::{
            ProvisionalSource, capture_provisional, provisional_failed_before_open,
        };
        let source = ProvisionalSource {
            capture_id: uuid::Uuid::new_v4(),
            boot_id: uuid::Uuid::new_v4(),
            specimen_claim: Digest::of(b"provisioned claim"),
            activation_id: uuid::Uuid::new_v4(),
            active_generation: 1,
            image_digest: Digest::of(b"image"),
            gpu0_bdf: "0000:01:00.0".into(),
            gpu0_pci_id: 0x20c2_10de,
        };
        let mut bar = FakeBar::default();
        let captured =
            capture_provisional(&mut bar, source.clone(), Policy::legacy_readonly()).unwrap();
        assert_eq!(captured.rows_first.len(), ROW_COUNT);
        assert_eq!(captured.rows_second.len(), ROW_COUNT);
        assert_eq!(captured.core_before.len(), 15);
        assert_eq!(captured.core_after.len(), 15);
        assert!(captured.stable_fingerprint().unwrap().is_some());
        assert_eq!(bar.address, 77);
        let missing = provisional_failed_before_open(source, "missing GPU0").unwrap();
        assert!(missing.rows_first.is_empty());
        assert_eq!(missing.stable_fingerprint().unwrap(), None);
        assert!(missing.failure.unwrap().contains("missing GPU0"));
    }

    #[test]
    fn partial_controller_read_preserves_prefix_without_issuing_rows() {
        use crate::baseline::{ProvisionalSource, capture_provisional};
        let source = ProvisionalSource {
            capture_id: uuid::Uuid::new_v4(),
            boot_id: uuid::Uuid::new_v4(),
            specimen_claim: Digest::of(b"claim"),
            activation_id: uuid::Uuid::new_v4(),
            active_generation: 1,
            image_digest: Digest::of(b"image"),
            gpu0_bdf: "0000:01:00.0".into(),
            gpu0_pci_id: 0x20c2_10de,
        };
        let mut bar = FakeBar {
            fail_core_read: true,
            ..FakeBar::default()
        };
        let evidence = capture_provisional(&mut bar, source, Policy::legacy_readonly()).unwrap();
        assert_eq!(evidence.version, 4);
        assert!(!evidence.core_before.is_empty());
        assert!(evidence.core_before.len() < 15);
        assert!(evidence.rows_first.is_empty());
        assert_eq!(bar.issued, 0);
        assert!(evidence.failure.unwrap().contains("core before"));
    }

    #[test]
    fn published_sbfe_and_controller_alias_must_agree_before_rows() {
        use crate::baseline::{ProvisionalSource, capture_provisional};
        let source = ProvisionalSource {
            capture_id: uuid::Uuid::new_v4(),
            boot_id: uuid::Uuid::new_v4(),
            specimen_claim: Digest::of(b"claim"),
            activation_id: uuid::Uuid::new_v4(),
            active_generation: 1,
            image_digest: Digest::of(b"image"),
            gpu0_bdf: "0000:01:00.0".into(),
            gpu0_pci_id: 0x20c2_10de,
        };
        let mut bar = FakeBar {
            flip_sbfe_after_first: true,
            ..FakeBar::default()
        };
        let evidence = capture_provisional(&mut bar, source, Policy::legacy_readonly()).unwrap();
        assert!(evidence.rows_first.is_empty());
        assert_eq!(bar.issued, 0);
        assert!(
            evidence
                .failure
                .unwrap()
                .contains("controller/published option before differs")
        );
    }

    #[test]
    fn closed_controller_blocks_all_row_commands() {
        let mut bar = FakeBar {
            control: 0,
            ..FakeBar::default()
        };
        assert_eq!(
            capture_full512(&mut bar, Policy::legacy_readonly()),
            Err(CaptureError::NotIdle(0))
        );
        assert_eq!(bar.issued, 0);
        assert_eq!(bar.address, 77);
    }

    #[test]
    fn bdf_parser_rejects_traversal_and_out_of_range_function() {
        assert!(valid_bdf("0000:01:00.0"));
        assert!(!valid_bdf("../../gpu0"));
        assert!(!valid_bdf("0000:01:20.0"));
        assert!(!valid_bdf("0000:01:00.8"));
    }
}

//! Shared identities and monotonic attempt states. No hardware access lives here.

use serde::{Deserialize, Serialize};
use sha2::{Digest as _, Sha256};
use std::fmt;
use std::path::Path;
use thiserror::Error;
use uuid::Uuid;

/// A SHA-256 digest. Its wire representation is exactly 64 lowercase hex digits.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Digest([u8; 32]);

#[derive(Debug, Error, Eq, PartialEq)]
pub enum DigestError {
    #[error("SHA-256 hex digest must contain exactly 64 characters")]
    Length,
    #[error("SHA-256 hex digest must use lowercase hexadecimal")]
    Encoding,
}

impl Digest {
    pub fn of(bytes: impl AsRef<[u8]>) -> Self {
        Self(Sha256::digest(bytes).into())
    }

    pub fn from_bytes(bytes: [u8; 32]) -> Self {
        Self(bytes)
    }

    pub fn as_bytes(&self) -> &[u8; 32] {
        &self.0
    }

    pub fn parse_hex(text: &str) -> Result<Self, DigestError> {
        if text.len() != 64 {
            return Err(DigestError::Length);
        }
        let mut bytes = [0_u8; 32];
        for (index, pair) in text.as_bytes().chunks_exact(2).enumerate() {
            let hi = hex_nibble(pair[0]).ok_or(DigestError::Encoding)?;
            let lo = hex_nibble(pair[1]).ok_or(DigestError::Encoding)?;
            bytes[index] = (hi << 4) | lo;
        }
        Ok(Self(bytes))
    }

    pub fn to_hex(self) -> String {
        let mut result = String::with_capacity(64);
        for byte in self.0 {
            use fmt::Write as _;
            write!(&mut result, "{byte:02x}").expect("writing to String cannot fail");
        }
        result
    }
}

fn hex_nibble(byte: u8) -> Option<u8> {
    match byte {
        b'0'..=b'9' => Some(byte - b'0'),
        b'a'..=b'f' => Some(byte - b'a' + 10),
        _ => None,
    }
}

/// Pure lexical check for one PCI function BDF. Device existence and vendor
/// identity are separate checks performed only by the scoped target adapter.
pub fn valid_pci_bdf(bdf: &str) -> bool {
    let bytes = bdf.as_bytes();
    if bytes.len() != 12
        || bytes[4] != b':'
        || bytes[7] != b':'
        || bytes[10] != b'.'
        || !matches!(bytes[11], b'0'..=b'7')
    {
        return false;
    }
    for (index, byte) in bytes.iter().enumerate() {
        if !matches!(index, 4 | 7 | 10) && !byte.is_ascii_hexdigit() {
            return false;
        }
    }
    u8::from_str_radix(&bdf[8..10], 16).is_ok_and(|device| device <= 31)
}

/// Exact configured PWMVID CDC link. It names one by-id entry and cannot
/// select a generic tty, nested path, traversal, or shell-like filename.
pub fn valid_board_by_id_path(value: &str) -> bool {
    let path = Path::new(value);
    path.parent() == Some(Path::new("/dev/serial/by-id"))
        && path
            .file_name()
            .and_then(|name| name.to_str())
            .is_some_and(|name| {
                !name.is_empty()
                    && name.len() <= 255
                    && !matches!(name, "." | "..")
                    && name.bytes().all(|byte| {
                        byte.is_ascii_alphanumeric()
                            || matches!(byte, b'_' | b'-' | b'.' | b'+' | b'@' | b':')
                    })
            })
}

/// The pinned G431CB CDC descriptor formats its STM32 UID as 24 uppercase
/// hexadecimal characters. This checks syntax only, not authenticity.
pub fn valid_pwmvid_usb_serial(value: &str) -> bool {
    value.len() == 24
        && value.bytes().any(|byte| byte != b'0')
        && value
            .bytes()
            .all(|byte| byte.is_ascii_digit() || matches!(byte, b'A'..=b'F'))
}

impl fmt::Display for Digest {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.to_hex())
    }
}

impl Serialize for Digest {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: serde::Serializer,
    {
        serializer.serialize_str(&self.to_hex())
    }
}

impl<'de> Deserialize<'de> for Digest {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: serde::Deserializer<'de>,
    {
        let text = String::deserialize(deserializer)?;
        Self::parse_hex(&text).map_err(serde::de::Error::custom)
    }
}

/// Every value in this key must be attested before a physical attempt.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct AttemptKey {
    pub campaign_id: Uuid,
    pub ordinal: u64,
    pub attempt_id: Uuid,
    pub sample_id: Digest,
    pub boot_id: Uuid,
    pub image_digest: Digest,
    pub calibration_digest: Digest,
    pub proposal_digest: Digest,
}

/// Immutable controller-side campaign segment. It contains only proposal digests,
/// never executable code or target commands.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CampaignSegment {
    pub version: u32,
    pub campaign_id: Uuid,
    pub start_ordinal: u64,
    pub end_ordinal: u64,
    pub variants: Vec<CampaignVariant>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CampaignVariant {
    pub name: String,
    pub proposal_sha256: Digest,
    pub weight: u32,
}

/// MCU-side ADC correction and rail windows accompanying the compiler's
/// voltage-to-CCR endpoints. These bounds mirror the firmware PROFILE input.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct McuHealthProfile {
    pub gain_ppm: u32,
    pub offset_mv: i32,
    pub boot_min_mv: u16,
    pub boot_max_mv: u16,
    pub load_min_mv: u16,
    pub load_max_mv: u16,
}

impl McuHealthProfile {
    pub fn validate(self) -> bool {
        (100_000..=10_000_000).contains(&self.gain_ppm)
            && (-1200..=1200).contains(&self.offset_mv)
            && (100..=1200).contains(&self.boot_min_mv)
            && (100..=1200).contains(&self.boot_max_mv)
            && self.boot_min_mv <= self.boot_max_mv
            && (100..=1200).contains(&self.load_min_mv)
            && (100..=1200).contains(&self.load_max_mv)
            && self.load_min_mv <= self.load_max_mv
    }
}

#[derive(Debug, Error, Eq, PartialEq)]
pub enum CampaignSegmentError {
    #[error("campaign segment version, identity or ordinal range invalid")]
    Identity,
    #[error("campaign variants or weights invalid")]
    Variants,
}

impl CampaignSegment {
    pub fn validate(&self) -> Result<(), CampaignSegmentError> {
        if self.version != 1
            || self.campaign_id.is_nil()
            || self.start_ordinal == 0
            || self.end_ordinal < self.start_ordinal
            || self.end_ordinal > i64::MAX as u64
        {
            return Err(CampaignSegmentError::Identity);
        }
        let mut names = std::collections::BTreeSet::new();
        let mut total = 0_u32;
        if self.variants.is_empty() {
            return Err(CampaignSegmentError::Variants);
        }
        for variant in &self.variants {
            if variant.name.is_empty()
                || variant.name.len() > 64
                || !variant
                    .name
                    .bytes()
                    .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'-' | b'.'))
                || !names.insert(&variant.name)
                || !(1..=1000).contains(&variant.weight)
            {
                return Err(CampaignSegmentError::Variants);
            }
            total = total
                .checked_add(variant.weight)
                .ok_or(CampaignSegmentError::Variants)?;
        }
        if total > 1000 {
            return Err(CampaignSegmentError::Variants);
        }
        Ok(())
    }

    pub fn proposal_for(&self, ordinal: u64) -> Result<Option<Digest>, CampaignSegmentError> {
        self.validate()?;
        if ordinal < self.start_ordinal || ordinal > self.end_ordinal {
            return Ok(None);
        }
        let total: u64 = self
            .variants
            .iter()
            .map(|variant| u64::from(variant.weight))
            .sum();
        let mut slot = (ordinal - self.start_ordinal) % total;
        for variant in &self.variants {
            if slot < u64::from(variant.weight) {
                return Ok(Some(variant.proposal_sha256));
            }
            slot -= u64::from(variant.weight);
        }
        Err(CampaignSegmentError::Variants)
    }
}

#[derive(Debug, Error, Eq, PartialEq)]
pub enum AttemptKeyError {
    #[error("attempt ordinal must be nonzero")]
    ZeroOrdinal,
    #[error("campaign, attempt and boot UUIDs must be non-nil")]
    NilIdentity,
}

impl AttemptKey {
    pub fn validate(&self) -> Result<(), AttemptKeyError> {
        if self.ordinal == 0 {
            return Err(AttemptKeyError::ZeroOrdinal);
        }
        if [self.campaign_id, self.attempt_id, self.boot_id]
            .iter()
            .any(Uuid::is_nil)
        {
            return Err(AttemptKeyError::NilIdentity);
        }
        Ok(())
    }

    pub fn digest(&self) -> Digest {
        let mut bytes = b"gpu0-attempt-key-v1\0".to_vec();
        bytes.extend_from_slice(self.campaign_id.as_bytes());
        bytes.extend_from_slice(&self.ordinal.to_le_bytes());
        bytes.extend_from_slice(self.attempt_id.as_bytes());
        bytes.extend_from_slice(self.sample_id.as_bytes());
        bytes.extend_from_slice(self.boot_id.as_bytes());
        bytes.extend_from_slice(self.image_digest.as_bytes());
        bytes.extend_from_slice(self.calibration_digest.as_bytes());
        bytes.extend_from_slice(self.proposal_digest.as_bytes());
        Digest::of(bytes)
    }
}

/// The ledger is the authority for this monotonic state.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AttemptState {
    Reserved,
    Validated,
    MayHaveIssued,
    Observing,
    Negative,
    Candidate,
    UncertainSpent,
    SkippedUnissued,
}

impl AttemptState {
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Reserved => "reserved",
            Self::Validated => "validated",
            Self::MayHaveIssued => "may_have_issued",
            Self::Observing => "observing",
            Self::Negative => "negative",
            Self::Candidate => "candidate",
            Self::UncertainSpent => "uncertain_spent",
            Self::SkippedUnissued => "skipped_unissued",
        }
    }

    pub fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Negative | Self::Candidate | Self::UncertainSpent | Self::SkippedUnissued
        )
    }

    pub fn permits(self, next: Self) -> bool {
        matches!(
            (self, next),
            (Self::Reserved, Self::Validated)
                | (Self::Validated, Self::MayHaveIssued)
                | (Self::MayHaveIssued, Self::Observing)
                | (Self::MayHaveIssued, Self::UncertainSpent)
                | (Self::Observing, Self::Negative)
                | (Self::Observing, Self::Candidate)
                | (Self::Observing, Self::UncertainSpent)
                | (Self::Reserved, Self::SkippedUnissued)
                | (Self::Validated, Self::SkippedUnissued)
        )
    }
}

impl TryFrom<&str> for AttemptState {
    type Error = &'static str;

    fn try_from(value: &str) -> Result<Self, Self::Error> {
        match value {
            "reserved" => Ok(Self::Reserved),
            "validated" => Ok(Self::Validated),
            "may_have_issued" => Ok(Self::MayHaveIssued),
            "observing" => Ok(Self::Observing),
            "negative" => Ok(Self::Negative),
            "candidate" => Ok(Self::Candidate),
            "uncertain_spent" => Ok(Self::UncertainSpent),
            "skipped_unissued" => Ok(Self::SkippedUnissued),
            _ => Err("unknown attempt state"),
        }
    }
}

/// Immutable capture graph stored by digest. Chunks are contiguous and ordered.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct CaptureChunkRef {
    pub index: u32,
    pub offset: u64,
    pub length: u64,
    pub sha256: Digest,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct CaptureManifest {
    pub version: u32,
    pub attempt_id: Uuid,
    pub chunks: Vec<CaptureChunkRef>,
    pub total_size: u64,
}

/// Source-bound terminal witness. Device-specific observations are held in
/// the capture graph, not inferred from the terminal label.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct TerminalReceipt {
    pub version: u32,
    pub attempt_key: AttemptKey,
    pub outcome: AttemptState,
    pub compiled_waveform_sha256: Digest,
    pub board_payload_sha256: Digest,
    pub board_payload_crc32: u32,
    pub capture_manifest_sha256: Digest,
    pub ledger_may_issue_sha256: Digest,
    pub board_sequence_id: u32,
    pub simulated: bool,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub capture_payload_sha256: Option<Digest>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub board_preparation_sha256: Option<Digest>,
}

/// Readback from a passive, qualified MCU before the no-drive delayed LOAD.
/// The ledger binds this artifact to its one-use issue event.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardPreparationReceipt {
    pub version: u32,
    pub attempt_key: AttemptKey,
    pub specimen_claim: Digest,
    pub pwmvid_usb_serial: String,
    pub calibration_generation: u64,
    pub board_sequence_id: u32,
    pub compiled_waveform_sha256: Digest,
    pub board_payload_sha256: Digest,
    pub board_arm_crc32: u32,
    pub profile_mv1: u16,
    pub profile_ccr1: u16,
    pub profile_mv2: u16,
    pub profile_ccr2: u16,
    pub profile_health: McuHealthProfile,
    pub passive: bool,
    pub fixture: bool,
    pub qualified: bool,
    pub profile_valid: bool,
    pub fault: u8,
    pub calibrated_mv: i32,
}

impl BoardPreparationReceipt {
    pub fn validate(&self) -> Result<(), &'static str> {
        if self.version != 1
            || self.attempt_key.validate().is_err()
            || self.specimen_claim.as_bytes().iter().all(|byte| *byte == 0)
            || !valid_pwmvid_usb_serial(&self.pwmvid_usb_serial)
            || self.calibration_generation == 0
            || self.board_sequence_id == 0
            || self
                .compiled_waveform_sha256
                .as_bytes()
                .iter()
                .all(|byte| *byte == 0)
            || self
                .board_payload_sha256
                .as_bytes()
                .iter()
                .all(|byte| *byte == 0)
            || !(100..=1200).contains(&self.profile_mv1)
            || self.profile_mv1 >= self.profile_mv2
            || self.profile_mv2 > 1200
            || !(16..=224).contains(&self.profile_ccr1)
            || !(16..=224).contains(&self.profile_ccr2)
            || self.profile_ccr1 == self.profile_ccr2
            || !self.profile_health.validate()
            || !self.passive
            || !self.fixture
            || !self.qualified
            || !self.profile_valid
            || self.fault != 0
            || self.calibrated_mv < i32::from(self.profile_health.boot_min_mv)
            || self.calibrated_mv > i32::from(self.profile_health.boot_max_mv)
        {
            return Err("invalid board preparation source or passive readback");
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PhysicalBoardFinalState {
    Completed,
    TimedOut,
    Fault,
    Unknown,
}

/// Post-S3 GPU0 observation. The provisional read vectors preserve partial
/// evidence, while this wrapper binds them to one attempt and MCU outcome.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PhysicalAttemptCapture {
    pub version: u32,
    pub attempt_key: AttemptKey,
    pub specimen_claim: Digest,
    pub pwmvid_usb_serial: String,
    pub calibration_generation: u64,
    pub board_sequence_id: u32,
    pub board_state: PhysicalBoardFinalState,
    /// CRC over the delayed plan and trigger, acknowledged by one ARM reply.
    pub board_arm_crc32: u32,
    pub post_gpu0: ProvisionalBaseline,
}

#[derive(Debug, Error)]
pub enum PhysicalAttemptCaptureError {
    #[error("physical attempt capture source differs")]
    Source,
    #[error(transparent)]
    Baseline(#[from] BaselineError),
}

impl PhysicalAttemptCapture {
    pub fn validate(&self) -> Result<Option<Digest>, PhysicalAttemptCaptureError> {
        if self.version != 5
            || self.attempt_key.validate().is_err()
            || self.specimen_claim.as_bytes().iter().all(|byte| *byte == 0)
            || !valid_pwmvid_usb_serial(&self.pwmvid_usb_serial)
            || self.calibration_generation == 0
            || self.board_sequence_id == 0
            || self.post_gpu0.boot_id != self.attempt_key.boot_id
            || self.post_gpu0.image_digest != self.attempt_key.image_digest
            || self.post_gpu0.specimen_claim != self.specimen_claim
            || !valid_pci_bdf(&self.post_gpu0.gpu0_bdf)
        {
            return Err(PhysicalAttemptCaptureError::Source);
        }
        Ok(self.post_gpu0.stable_fingerprint()?)
    }
}

/// A target-local one-use warm-reboot request sourced from the latest
/// terminal negative attempt on the old boot. It is not an actuator command;
/// the target must independently validate physical negativity first, and the
/// ledger must commit its barrier before any reboot.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct NegativeRebootIntent {
    pub version: u32,
    pub attempt_key: AttemptKey,
    pub source_terminal_sha256: Digest,
    pub simulated: bool,
}

/// The only target-local actions that may be journaled as routine recovery.
/// Waveform issue and host reboot use separate one-use barriers.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum RecoveryAction {
    None,
    Preserve,
    ReconnectUsb,
    QueryBoard,
    ClearBoardFault,
    RearmBoard,
    ProbeGpu0,
    CapturePairedFull512,
    CommitOneUseRebootIntent,
    WarmRebootOnce,
    AwaitBootChange,
    Unavailable,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum RecoveryFault {
    UsbDisconnected,
    BoardFaultLatched,
    Gpu0Missing,
    S3Timeout,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum RecoveryObservation {
    UsbConnected,
    BoardReady,
    BoardFaultLatched,
    BoardFaultCleared,
    BoardArmed,
    Gpu0Present,
    Gpu0Missing,
    PairedBaseline,
    CaptureUnknown,
    BootChanged,
    RebootIntentCommitted {
        source_receipt: Digest,
        intent_receipt: Digest,
    },
    ActionFailed,
}

impl RecoveryAction {
    pub fn is_local_recovery(self) -> bool {
        matches!(
            self,
            Self::ReconnectUsb
                | Self::QueryBoard
                | Self::ClearBoardFault
                | Self::RearmBoard
                | Self::ProbeGpu0
                | Self::CapturePairedFull512
        )
    }

    /// A local recovery observation cannot grant a waveform or reboot.
    pub fn accepts_local_observation(self, observation: RecoveryObservation) -> bool {
        use RecoveryAction as A;
        use RecoveryObservation as O;
        matches!(
            (self, observation),
            (A::ReconnectUsb, O::UsbConnected | O::ActionFailed)
                | (
                    A::QueryBoard,
                    O::BoardReady | O::BoardFaultLatched | O::ActionFailed
                )
                | (A::ClearBoardFault, O::BoardFaultCleared | O::ActionFailed)
                | (A::RearmBoard, O::BoardArmed | O::ActionFailed)
                | (
                    A::ProbeGpu0,
                    O::Gpu0Present | O::Gpu0Missing | O::ActionFailed
                )
                | (
                    A::CapturePairedFull512,
                    O::PairedBaseline | O::CaptureUnknown | O::ActionFailed
                )
        )
    }
}

/// Durable receipt for one observed local recovery step after a terminal
/// negative attempt. Evidence bytes live in the content-addressed object store.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct RecoveryStepReceipt {
    pub version: u32,
    pub attempt_key: AttemptKey,
    pub source_terminal_sha256: Digest,
    pub recovery_id: Uuid,
    #[serde(default = "legacy_recovery_fault")]
    pub fault: RecoveryFault,
    #[serde(default = "legacy_recovery_retries")]
    pub max_retries: u8,
    pub step_index: u32,
    pub previous_step_sha256: Option<Digest>,
    pub action: RecoveryAction,
    pub observation: RecoveryObservation,
    pub evidence_sha256: Digest,
    pub simulated: bool,
}

#[derive(Debug, Error)]
pub enum RecoveryStepDecodeError {
    #[error(transparent)]
    Json(#[from] serde_json::Error),
    #[error("unsupported recovery receipt version")]
    Version,
    #[error("version 2 recovery receipt lacks explicit policy fields")]
    MissingPolicy,
    #[error("version 1 recovery receipt has unexpected policy fields")]
    LegacyPolicy,
}

impl RecoveryStepReceipt {
    pub fn decode_json(bytes: &[u8]) -> Result<Self, RecoveryStepDecodeError> {
        let value: serde_json::Value = serde_json::from_slice(bytes)?;
        let version = value.get("version").and_then(serde_json::Value::as_u64);
        match version {
            Some(1) | Some(2) => {}
            _ => return Err(RecoveryStepDecodeError::Version),
        }
        let has_fault = value.get("fault").is_some();
        let has_retries = value.get("max_retries").is_some();
        if version == Some(2) && (!has_fault || !has_retries) {
            return Err(RecoveryStepDecodeError::MissingPolicy);
        }
        let receipt: Self = serde_json::from_value(value)?;
        if version == Some(1)
            && (receipt.fault != legacy_recovery_fault()
                || receipt.max_retries != legacy_recovery_retries())
        {
            return Err(RecoveryStepDecodeError::LegacyPolicy);
        }
        Ok(receipt)
    }
}

fn legacy_recovery_fault() -> RecoveryFault {
    RecoveryFault::UsbDisconnected
}

fn legacy_recovery_retries() -> u8 {
    2
}

/// One source-bound GPU0 baseline observation. Arrays may be partial after
/// an interrupted read; only two complete matching observations enroll boot.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct BaselineEvidence {
    pub version: u32,
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub sample_id: Digest,
    pub image_digest: Digest,
    pub calibration_digest: Digest,
    pub option_map_digest: Digest,
    pub status_map_digest: Digest,
    pub rows_first: Vec<u32>,
    pub rows_second: Vec<u32>,
    pub options_before: Vec<u32>,
    pub options_after: Vec<u32>,
    pub statuses_before: Vec<u32>,
    pub statuses_after: Vec<u32>,
    pub simulated: bool,
    /// Capture or exact-address restoration failure. Older v1 evidence omits
    /// this field; any failure prevents boot enrollment.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub failure: Option<String>,
}

/// Read-only GPU0 observation before a sample ID or board calibration exists.
/// The controller's separately provisioned claim is an inventory authority, not a
/// target-supplied sample ID. The ledger binds it to paired physical evidence.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct ProvisionalBaseline {
    pub version: u32,
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub specimen_claim: Digest,
    pub activation_id: Uuid,
    pub active_generation: u64,
    pub image_digest: Digest,
    pub gpu0_bdf: String,
    pub gpu0_pci_id: u32,
    pub option_map_digest: Digest,
    pub status_map_digest: Digest,
    pub core_map_digest: Digest,
    pub rows_first: Vec<u32>,
    pub rows_second: Vec<u32>,
    pub options_before: Vec<u32>,
    pub options_after: Vec<u32>,
    pub statuses_before: Vec<u32>,
    pub statuses_after: Vec<u32>,
    pub core_before: Vec<u32>,
    pub core_after: Vec<u32>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub failure: Option<String>,
}

#[derive(Debug, Error, Eq, PartialEq)]
pub enum BaselineError {
    #[error("baseline identity or version invalid")]
    Identity,
    #[error("baseline vector exceeds the GPU0 capture map")]
    TooLarge,
}

impl BaselineEvidence {
    /// Returns None for partial or unstable evidence; it remains recordable.
    pub fn stable_fingerprint(&self) -> Result<Option<Digest>, BaselineError> {
        if self.version != 1 || self.capture_id.is_nil() || self.boot_id.is_nil() {
            return Err(BaselineError::Identity);
        }
        stable_scan_fingerprint(b"gpu0-baseline-v1", self.into())
    }
}

impl ProvisionalBaseline {
    pub fn stable_fingerprint(&self) -> Result<Option<Digest>, BaselineError> {
        if self.version != 4
            || self.capture_id.is_nil()
            || self.boot_id.is_nil()
            || self.activation_id.is_nil()
            || self.active_generation == 0
            || self.gpu0_bdf.is_empty()
            || self.gpu0_bdf.len() > 32
            || self.gpu0_pci_id & 0xffff != 0x10de
        {
            return Err(BaselineError::Identity);
        }
        stable_scan_fingerprint(b"gpu0-baseline-v4", self.into())
    }
}

struct ScanView<'a> {
    option_map: Digest,
    status_map: Digest,
    core_map: Option<Digest>,
    rows_first: &'a [u32],
    rows_second: &'a [u32],
    options_before: &'a [u32],
    options_after: &'a [u32],
    statuses_before: &'a [u32],
    statuses_after: &'a [u32],
    core_before: &'a [u32],
    core_after: &'a [u32],
    failure: Option<&'a str>,
}

impl<'a> From<&'a BaselineEvidence> for ScanView<'a> {
    fn from(value: &'a BaselineEvidence) -> Self {
        Self {
            option_map: value.option_map_digest,
            status_map: value.status_map_digest,
            core_map: None,
            rows_first: &value.rows_first,
            rows_second: &value.rows_second,
            options_before: &value.options_before,
            options_after: &value.options_after,
            statuses_before: &value.statuses_before,
            statuses_after: &value.statuses_after,
            core_before: &[],
            core_after: &[],
            failure: value.failure.as_deref(),
        }
    }
}

impl<'a> From<&'a ProvisionalBaseline> for ScanView<'a> {
    fn from(value: &'a ProvisionalBaseline) -> Self {
        Self {
            option_map: value.option_map_digest,
            status_map: value.status_map_digest,
            core_map: Some(value.core_map_digest),
            rows_first: &value.rows_first,
            rows_second: &value.rows_second,
            options_before: &value.options_before,
            options_after: &value.options_after,
            statuses_before: &value.statuses_before,
            statuses_after: &value.statuses_after,
            core_before: &value.core_before,
            core_after: &value.core_after,
            failure: value.failure.as_deref(),
        }
    }
}

fn stable_scan_fingerprint(
    domain: &[u8],
    scan: ScanView<'_>,
) -> Result<Option<Digest>, BaselineError> {
    if scan.rows_first.len() > 512
        || scan.rows_second.len() > 512
        || scan.options_before.len() > 672
        || scan.options_after.len() > 672
        || scan.statuses_before.len() > 84
        || scan.statuses_after.len() > 84
        || scan.core_before.len() > 15
        || scan.core_after.len() > 15
        || scan.failure.is_some_and(|error| error.len() > 1024)
    {
        return Err(BaselineError::TooLarge);
    }
    if scan.failure.is_some()
        || scan.rows_first.len() != 512
        || scan.rows_second.len() != 512
        || scan.options_before.len() != 672
        || scan.options_after.len() != 672
        || scan.statuses_before.len() != 84
        || scan.statuses_after.len() != 84
        || scan.rows_first != scan.rows_second
        || scan.options_before != scan.options_after
        || scan.statuses_before != scan.statuses_after
        || (scan.core_map.is_some()
            && (scan.core_before.len() != 15
                || scan.core_after.len() != 15
                || scan.core_before != scan.core_after))
    {
        return Ok(None);
    }
    let mut bytes = domain.to_vec();
    bytes.extend_from_slice(scan.option_map.as_bytes());
    bytes.extend_from_slice(scan.status_map.as_bytes());
    if let Some(core_map) = scan.core_map {
        bytes.extend_from_slice(core_map.as_bytes());
    }
    for word in scan
        .rows_first
        .iter()
        .chain(scan.options_before)
        .chain(scan.statuses_before)
        .chain(scan.core_before)
    {
        bytes.extend_from_slice(&word.to_le_bytes());
    }
    Ok(Some(Digest::of(bytes)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn attempt_key_digest_binds_proposal_and_boot() {
        let key = AttemptKey {
            campaign_id: Uuid::new_v4(),
            ordinal: 1,
            attempt_id: Uuid::new_v4(),
            sample_id: Digest::of(b"sample"),
            boot_id: Uuid::new_v4(),
            image_digest: Digest::of(b"image"),
            calibration_digest: Digest::of(b"calibration"),
            proposal_digest: Digest::of(b"proposal"),
        };
        let mut changed = key.clone();
        changed.proposal_digest = Digest::of(b"variant");
        assert_ne!(key.digest(), changed.digest());
        changed = key.clone();
        changed.boot_id = Uuid::new_v4();
        assert_ne!(key.digest(), changed.digest());
    }

    #[test]
    fn version_one_recovery_receipt_keeps_legacy_usb_policy_on_read() {
        let receipt = RecoveryStepReceipt {
            version: 1,
            attempt_key: AttemptKey {
                campaign_id: Uuid::new_v4(),
                ordinal: 1,
                attempt_id: Uuid::new_v4(),
                sample_id: Digest::of(b"sample"),
                boot_id: Uuid::new_v4(),
                image_digest: Digest::of(b"image"),
                calibration_digest: Digest::of(b"calibration"),
                proposal_digest: Digest::of(b"proposal"),
            },
            source_terminal_sha256: Digest::of(b"terminal"),
            recovery_id: Uuid::new_v4(),
            fault: RecoveryFault::UsbDisconnected,
            max_retries: 2,
            step_index: 0,
            previous_step_sha256: None,
            action: RecoveryAction::ReconnectUsb,
            observation: RecoveryObservation::UsbConnected,
            evidence_sha256: Digest::of(b"evidence"),
            simulated: true,
        };
        let mut legacy = serde_json::to_value(&receipt).unwrap();
        legacy.as_object_mut().unwrap().remove("fault");
        legacy.as_object_mut().unwrap().remove("max_retries");
        let parsed =
            RecoveryStepReceipt::decode_json(&serde_json::to_vec(&legacy).unwrap()).unwrap();
        assert_eq!(parsed, receipt);

        legacy
            .as_object_mut()
            .unwrap()
            .insert("version".into(), 2.into());
        assert!(matches!(
            RecoveryStepReceipt::decode_json(&serde_json::to_vec(&legacy).unwrap()),
            Err(RecoveryStepDecodeError::MissingPolicy)
        ));
    }

    #[test]
    fn digest_roundtrip_and_canonical_encoding() {
        let digest = Digest::of(b"waveform");
        let serialized = serde_json::to_string(&digest).unwrap();
        assert_eq!(serialized.len(), 66);
        assert_eq!(serde_json::from_str::<Digest>(&serialized).unwrap(), digest);
        assert_eq!(Digest::parse_hex(&digest.to_hex()), Ok(digest));
        assert_eq!(
            Digest::parse_hex(&digest.to_hex().to_uppercase()),
            Err(DigestError::Encoding)
        );
    }

    #[test]
    fn pwmvid_usb_serial_requires_nonzero_uppercase_uid() {
        assert!(valid_pwmvid_usb_serial("0123456789ABCDEF01234567"));
        assert!(!valid_pwmvid_usb_serial("000000000000000000000000"));
        assert!(!valid_pwmvid_usb_serial("0123456789abcdef01234567"));
    }

    #[test]
    fn terminal_attempts_cannot_move_or_replay() {
        assert!(AttemptState::Reserved.permits(AttemptState::Validated));
        assert!(AttemptState::Validated.permits(AttemptState::MayHaveIssued));
        assert!(AttemptState::MayHaveIssued.permits(AttemptState::UncertainSpent));
        assert!(!AttemptState::UncertainSpent.permits(AttemptState::Validated));
        assert!(!AttemptState::Candidate.permits(AttemptState::MayHaveIssued));
        assert!(!AttemptState::Negative.permits(AttemptState::Observing));
    }

    #[test]
    fn partial_baseline_is_recordable_but_cannot_enroll() {
        let sample = BaselineEvidence {
            version: 1,
            capture_id: Uuid::new_v4(),
            boot_id: Uuid::new_v4(),
            sample_id: Digest::of(b"sample"),
            image_digest: Digest::of(b"image"),
            calibration_digest: Digest::of(b"cal"),
            option_map_digest: Digest::of(b"map-options"),
            status_map_digest: Digest::of(b"map-status"),
            rows_first: vec![0; 512],
            rows_second: vec![0; 511],
            options_before: vec![0; 672],
            options_after: vec![0; 672],
            statuses_before: vec![0; 84],
            statuses_after: vec![0; 84],
            simulated: true,
            failure: None,
        };
        assert_eq!(sample.stable_fingerprint().unwrap(), None);
        let mut stable = sample;
        stable.rows_second.push(0);
        assert!(stable.stable_fingerprint().unwrap().is_some());
        assert!(
            !serde_json::to_string(&stable)
                .unwrap()
                .contains("\"failure\"")
        );
        stable.failure = Some("address restore failed".into());
        assert_eq!(stable.stable_fingerprint().unwrap(), None);
    }

    #[test]
    fn provisional_fingerprint_is_source_distinct_from_legacy_capture() {
        let provisional = ProvisionalBaseline {
            version: 4,
            capture_id: Uuid::new_v4(),
            boot_id: Uuid::new_v4(),
            specimen_claim: Digest::of(b"claim"),
            activation_id: Uuid::new_v4(),
            active_generation: 1,
            image_digest: Digest::of(b"image"),
            gpu0_bdf: "0000:01:00.0".into(),
            gpu0_pci_id: 0x20c2_10de,
            option_map_digest: Digest::of(b"options"),
            status_map_digest: Digest::of(b"statuses"),
            core_map_digest: Digest::of(b"core"),
            rows_first: vec![0; 512],
            rows_second: vec![0; 512],
            options_before: vec![0; 672],
            options_after: vec![0; 672],
            statuses_before: vec![0; 84],
            statuses_after: vec![0; 84],
            core_before: vec![0; 15],
            core_after: vec![0; 15],
            failure: None,
        };
        let provisional_fp = provisional.stable_fingerprint().unwrap().unwrap();
        let mut unstable_core = provisional.clone();
        unstable_core.core_after[0] = 1;
        assert_eq!(unstable_core.stable_fingerprint().unwrap(), None);
        let legacy = BaselineEvidence {
            version: 1,
            capture_id: provisional.capture_id,
            boot_id: provisional.boot_id,
            sample_id: Digest::of(b"old sample"),
            image_digest: provisional.image_digest,
            calibration_digest: Digest::of(b"calibration"),
            option_map_digest: provisional.option_map_digest,
            status_map_digest: provisional.status_map_digest,
            rows_first: provisional.rows_first.clone(),
            rows_second: provisional.rows_second.clone(),
            options_before: provisional.options_before.clone(),
            options_after: provisional.options_after.clone(),
            statuses_before: provisional.statuses_before.clone(),
            statuses_after: provisional.statuses_after.clone(),
            simulated: true,
            failure: None,
        };
        assert_ne!(
            provisional_fp,
            legacy.stable_fingerprint().unwrap().unwrap()
        );
        let mut interrupted = provisional;
        interrupted.rows_second.pop();
        assert_eq!(interrupted.stable_fingerprint().unwrap(), None);
    }
}

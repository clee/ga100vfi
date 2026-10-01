//! Versioned runner-to-leaf API. No ledger or recovery command is expressible.
use glitch_contract::{AttemptKey, Digest};
use glitch_waveform::{Calibration, Proposal};
use serde::{Deserialize, Serialize};
use uuid::Uuid;

pub const VERSION: u32 = 2;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum TargetState {
    Booting,
    Unenrolled,
    Ready,
    Busy,
    Recovering,
    Preserved,
    Unavailable,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum AttemptState {
    Accepted,
    Observing,
    Observed,
    UnknownSpent,
    RejectedNoIssue,
    NotFoundOnThisBoot,
}

impl AttemptState {
    pub fn terminal(self) -> bool {
        matches!(
            self,
            Self::Observed | Self::UnknownSpent | Self::RejectedNoIssue | Self::NotFoundOnThisBoot
        )
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CaptureProfile {
    CompactGpu0,
    Full512Gpu0,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum PciPreparationPhase {
    Idle,
    MayHaveCleared,
    Cleared,
    Refused,
    Uncertain,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PciState {
    pub command: u16,
    pub enable_count: u32,
    pub driver_bound: bool,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PciPreparation {
    pub boot_id: Uuid,
    pub gpu0_bdf: String,
    pub gpu0_pci_id: u32,
    pub phase: PciPreparationPhase,
    pub before: Option<PciState>,
    pub after: Option<PciState>,
    pub detail: Option<String>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Description {
    pub protocol_version: u32,
    pub boot_id: Uuid,
    pub image_digest: Digest,
    pub sample_id: Digest,
    pub calibration_digest: Digest,
    pub gpu0_pci_id: u32,
    pub pwmvid_serial: String,
    pub board_error: Option<String>,
    #[serde(default)]
    pub gpu0_pci_preparation: Option<PciPreparation>,
    pub revision: u64,
    pub state: TargetState,
    pub first_capture_id: Option<Uuid>,
    pub second_capture_id: Option<Uuid>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Baseline {
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub image_digest: Digest,
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
    pub failure: Option<String>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum BoardSource {
    Standalone,
    FollowGpu,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardResetIntent {
    pub boot_id: Uuid,
    pub image_digest: Digest,
    pub sample_id: Digest,
    pub pwmvid_serial: String,
    pub fault_before_reset: u8,
    pub action: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub calibration_point_id: Option<Uuid>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardResetEvidence {
    pub intent: BoardResetIntent,
    pub intent_sha256: Digest,
    pub reset_reply: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_off_telemetry: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub output_off_telemetry_sha256: Option<Digest>,
}

/// Passive PWMVID/rail measurement before a specimen profile is admitted.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardObservation {
    pub version: u32,
    pub observation_id: Uuid,
    pub boot_id: Uuid,
    pub image_digest: Digest,
    pub sample_id: Digest,
    pub gpu0_bdf: String,
    pub gpu0_pci_id: u32,
    pub pwmvid_serial: String,
    pub reset_evidence: Option<BoardResetEvidence>,
    pub passive_healthy: bool,
    pub parse_error: Option<String>,
    pub rail_mv: Option<u16>,
    pub ccr: Option<u16>,
    pub adc_code: Option<u16>,
    pub vdda_mv: Option<u16>,
    pub source: Option<BoardSource>,
    pub telemetry_sha256: Digest,
    pub telemetry: String,
}

/// Runtime calibration data; never an admission of a physical glitch profile.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CalibrationPointRequest {
    pub version: u32,
    pub point_id: Uuid,
    pub expected_boot_id: Uuid,
    pub expected_image_digest: Digest,
    pub expected_sample_id: Digest,
    pub expected_board_serial: String,
    pub expected_gpu0_pci_id: u32,
    pub baseline_observation_id: Uuid,
    pub baseline_rail_mv: u16,
    pub provisional: Calibration,
    pub target_ccr: u16,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub anchor_point_id: Option<Uuid>,
    /// Version 3 only: one bounded low-duty diagnostic sequence, not a
    /// physical glitch profile or a calibration-fit point.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub probe_low_ms: Option<u16>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub probe_background_ccr: Option<u16>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum CalibrationPointState {
    Accepted,
    RejectedNoIssue,
    Observed,
    UnknownSpent,
    NotFoundOnThisBoot,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CalibrationIssueIntent {
    pub point_id: Uuid,
    pub boot_id: Uuid,
    pub image_digest: Digest,
    pub sample_id: Digest,
    pub board_serial: String,
    pub request_sha256: Digest,
    pub target_ccr: u16,
    pub lease_ms: u16,
}

impl CalibrationPointState {
    pub fn terminal(self) -> bool {
        !matches!(self, Self::Accepted)
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct CalibrationPointReply {
    pub version: u32,
    pub point_id: Uuid,
    pub boot_id: Uuid,
    pub image_digest: Digest,
    pub sample_id: Digest,
    pub state: CalibrationPointState,
    pub proposal_digest: Digest,
    pub issue_intent: Option<CalibrationIssueIntent>,
    pub intent_sha256: Option<Digest>,
    pub before: Option<BoardObservation>,
    pub active_telemetry: Option<String>,
    pub active_telemetry_sha256: Option<Digest>,
    pub active_rail_mv: Option<u16>,
    pub after: Option<BoardObservation>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub post_captures: Option<[Baseline; 2]>,
    /// Version 3 only: bounded raw ADC telemetry polled during the low step.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub probe_samples: Vec<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub probe_start_reply: Option<String>,
    pub safe_to_continue: bool,
    pub detail: Option<String>,
}

impl Baseline {
    pub fn stable_fingerprint(&self) -> Option<Digest> {
        if self.failure.is_some()
            || self.rows_first.len() != 512
            || self.rows_first != self.rows_second
            || self.options_before.len() != 672
            || self.options_before != self.options_after
            || self.statuses_before.len() != 84
            || self.statuses_before != self.statuses_after
            || self.core_before.len() != 15
            || self.core_before != self.core_after
        {
            return None;
        }
        let mut bytes = Vec::with_capacity(4 + 512 * 4 + 672 * 4 + 84 * 4 + 15 * 4);
        bytes.extend_from_slice(&self.gpu0_pci_id.to_le_bytes());
        for words in [
            &self.rows_first,
            &self.options_before,
            &self.statuses_before,
            &self.core_before,
        ] {
            for word in words {
                bytes.extend_from_slice(&word.to_le_bytes());
            }
        }
        Some(Digest::of(bytes))
    }
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PreviewRequest {
    pub expected_boot_id: Uuid,
    pub waveform: Proposal,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct PreviewReply {
    pub proposal_digest: Digest,
    pub compiled_digest: Digest,
    pub canonical_waveform: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct RunAttemptRequest {
    pub key: AttemptKey,
    pub expected_revision: u64,
    pub waveform: Proposal,
    pub capture_profile: CaptureProfile,
    pub timeout_ms: u32,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AttemptReply {
    pub boot_id: Uuid,
    pub attempt_id: Uuid,
    pub state: AttemptState,
    pub compiled_digest: Option<Digest>,
    pub board_sequence_id: Option<u32>,
    pub capture_sha256: Option<Digest>,
    pub capture: Option<Vec<u8>>,
    /// Same-boot board-only evidence after a spent GPU0 capture failure.
    /// It is diagnostic and cannot qualify an attempt as observed or FF eligible.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub board_only_diagnostic: Option<BoardOnlyDiagnostic>,
    pub detail: Option<String>,
}

/// The source-bound full GPU0 observation nested in a physical terminal.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AttemptCapture {
    pub version: u32,
    pub key: AttemptKey,
    pub gpu0_bdf: String,
    pub baseline: Baseline,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardStatusEvidence {
    pub state: String,
    pub sequence_id: u32,
    pub plan_crc32: u32,
}

/// PWMVID's terminal DELAY_STATUS fields. The voltage fields describe the
/// rail-loss trigger crossing; they are not samples of the glitch waveform.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardDelayStatusEvidence {
    pub provenance: String,
    pub state: String,
    pub sequence_id: u32,
    pub threshold_mv: u16,
    pub armed_ms: u32,
    pub low_ms: u32,
    pub low_mv: u32,
    pub high_ms: u32,
    pub high_mv: u32,
    pub start_ms: u32,
    pub finish_ms: u32,
    pub fault: u32,
    pub run: bool,
}

/// MCU ADC windows read after a matched delayed-sequence terminal status.
/// Each pair in `raw_pairs_le` is one little-endian sense and VREF ADC code.
/// These are raw measurements, not a voltage claim or FF classification.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardAdcTraceEvidence {
    pub provenance: String,
    pub sequence_id: u32,
    pub completed_edges: u8,
    pub armed: bool,
    pub samples_per_edge: u16,
    pub pre_samples: u16,
    pub cycles_per_scan: u32,
    pub min_block_cycles: u32,
    pub max_block_cycles: u32,
    pub hclk_hz: u32,
    pub trigger_code: u16,
    pub vref_cal: u16,
    pub raw_pairs_le: Vec<u8>,
    pub raw_pairs_sha256: Digest,
}

/// Source-bound board terminal preserved when the GPU0 compact capture fails.
/// No GPU0 security state can be inferred from this evidence alone.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct BoardOnlyDiagnostic {
    pub version: u32,
    pub key: AttemptKey,
    pub compiled_digest: Digest,
    pub issue_receipt_digest: Digest,
    pub board_sequence_id: u32,
    pub board_arm_crc32: u32,
    pub board_status: BoardStatusEvidence,
    /// Distinguishes a direct board CRC from the accepted text-commit CRC
    /// plus matched terminal sequence; this is not an independent CRC read.
    pub board_status_crc_provenance: String,
    pub board_delay_status: Option<BoardDelayStatusEvidence>,
    pub board_adc_trace: Option<BoardAdcTraceEvidence>,
    pub board_adc_trace_error: Option<String>,
}

/// The exact byte-hashed result returned by `GetAttempt` after physical S3.
#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct AttemptTiming {
    /// Wall time from S3 preparation through same-boot resume.
    pub suspend_wall_ms: Option<u64>,
    /// Wall time for the scoped post-S3 GPU0 capture.
    pub capture_wall_ms: Option<u64>,
    /// Wall time from verified board ARM acknowledgment to terminal receipt.
    pub terminal_wall_ms: Option<u64>,
}

/// The exact byte-hashed result returned by `GetAttempt` after physical S3.
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct TerminalEvidence {
    pub version: u32,
    pub key: AttemptKey,
    pub compiled_digest: Digest,
    pub issue_receipt_digest: Digest,
    pub board_sequence_id: u32,
    pub board_arm_crc32: u32,
    pub board_status: Option<BoardStatusEvidence>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub board_delay_status: Option<BoardDelayStatusEvidence>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub board_adc_trace: Option<BoardAdcTraceEvidence>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub board_adc_trace_error: Option<String>,
    pub board_status_error: Option<String>,
    pub board_status_crc_provenance: String,
    pub board_profile_ack_only: bool,
    pub suspend_error: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub timing: Option<AttemptTiming>,
    pub gpu0_capture_sha256: Digest,
    pub gpu0_capture: Vec<u8>,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct ErrorReply {
    pub code: String,
    pub detail: String,
}

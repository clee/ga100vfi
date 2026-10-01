//! Pure, deterministic PWMVID proposal compiler. It never opens a device.

use glitch_contract::Digest;
use serde::{Deserialize, Serialize};
use thiserror::Error;

const PWM_HZ: u128 = 675_000;
const NS_PER_SECOND: u128 = 1_000_000_000;
const TIMER_TICKS: u32 = 240;
const FORMAT: u8 = 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(tag = "kind", content = "value", rename_all = "snake_case")]
pub enum Target {
    Ccr(u16),
    Millivolts(u16),
    DutyBasisPoints(u16),
    Lowest,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Step {
    pub target: Target,
    pub duration_ns: u64,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Proposal {
    pub background: Target,
    pub steps: Vec<Step>,
    pub post: Target,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub delayed_trigger: Option<DelayedTrigger>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct DelayedTrigger {
    pub threshold_mv: u16,
    pub timeout_ms: u32,
}

impl DelayedTrigger {
    /// Matches the MCU's guarded S3 rail-loss trigger envelope.
    pub fn validate(self) -> Result<(), CompileError> {
        if !(150..=500).contains(&self.threshold_mv) || !(1000..=120_000).contains(&self.timeout_ms)
        {
            return Err(CompileError::Trigger);
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Calibration {
    pub mv1: u16,
    pub ccr1: u16,
    pub mv2: u16,
    pub ccr2: u16,
}

impl Calibration {
    /// Stable versioned bytes for the calibration actually used by the
    /// compiler. A future physical profile adds sample, board and measurement
    /// provenance around this value.
    pub fn canonical_bytes(self) -> [u8; 9] {
        let mut bytes = [0_u8; 9];
        bytes[0] = 1;
        bytes[1..3].copy_from_slice(&self.mv1.to_le_bytes());
        bytes[3..5].copy_from_slice(&self.ccr1.to_le_bytes());
        bytes[5..7].copy_from_slice(&self.mv2.to_le_bytes());
        bytes[7..9].copy_from_slice(&self.ccr2.to_le_bytes());
        bytes
    }

    pub fn digest(self) -> Digest {
        Digest::of(self.canonical_bytes())
    }

    pub fn validate(self) -> Result<(), CompileError> {
        if !(100 <= self.mv1
            && self.mv1 < self.mv2
            && self.mv2 <= 1200
            && (16..=224).contains(&self.ccr1)
            && (16..=224).contains(&self.ccr2)
            && self.ccr1 != self.ccr2)
        {
            return Err(CompileError::Calibration);
        }
        Ok(())
    }

    fn contains_ccr(self, value: u16) -> bool {
        let lower = self.ccr1.min(self.ccr2);
        let upper = self.ccr1.max(self.ccr2);
        (lower..=upper).contains(&value)
    }
}

/// Immutable image policy. Per-sample calibration is supplied separately.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Policy {
    pub min_ccr: u16,
    pub max_ccr: u16,
    pub installed_min_ccr: u16,
    pub max_segments: usize,
    pub sustained_periods: u32,
    pub short_span_max_periods: u32,
    pub experimental_low_max_periods: u32,
}

impl Policy {
    /// Stable image-policy identity for a signed per-sample calibration.
    pub fn canonical_bytes(self) -> [u8; 27] {
        let mut bytes = [0_u8; 27];
        bytes[0] = 1;
        bytes[1..3].copy_from_slice(&self.min_ccr.to_le_bytes());
        bytes[3..5].copy_from_slice(&self.max_ccr.to_le_bytes());
        bytes[5..7].copy_from_slice(&self.installed_min_ccr.to_le_bytes());
        bytes[7..15].copy_from_slice(&(self.max_segments as u64).to_le_bytes());
        bytes[15..19].copy_from_slice(&self.sustained_periods.to_le_bytes());
        bytes[19..23].copy_from_slice(&self.short_span_max_periods.to_le_bytes());
        bytes[23..27].copy_from_slice(&self.experimental_low_max_periods.to_le_bytes());
        bytes
    }

    pub fn digest(self) -> Digest {
        Digest::of(self.canonical_bytes())
    }

    /// Port of the existing board compiler's representability limits.
    /// A production policy must be selected by the signed image manifest.
    pub const fn legacy_v1() -> Self {
        Self {
            min_ccr: 1,
            max_ccr: 224,
            installed_min_ccr: 69,
            max_segments: 32,
            sustained_periods: 3375,
            short_span_max_periods: 6750,
            experimental_low_max_periods: 8_100_000,
        }
    }

    pub fn validate(self) -> Result<(), CompileError> {
        if self.min_ccr == 0
            || self.min_ccr > self.max_ccr
            || self.max_ccr >= TIMER_TICKS as u16
            || self.max_segments == 0
            || self.max_segments > 32
            || self.installed_min_ccr < self.min_ccr
            || self.installed_min_ccr > self.max_ccr
        {
            return Err(CompileError::Policy);
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CompiledStep {
    pub index: usize,
    pub ccr: u16,
    pub start_period: u32,
    pub end_period: u32,
    pub requested_end_ns: u128,
    pub end_error_ns_numerator: i128,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Compiled {
    pub background_ccr: u16,
    pub post_ccr: u16,
    pub total_periods: u32,
    pub steps: Vec<CompiledStep>,
    pub delayed_trigger: Option<DelayedTrigger>,
    /// Hash of canonical physical waveform bytes, excluding board sequence ID.
    pub waveform_digest: Digest,
}

impl Compiled {
    pub fn canonical_bytes(&self) -> Vec<u8> {
        let mut bytes = Vec::with_capacity(10 + self.steps.len() * 6);
        bytes.push(FORMAT);
        bytes.extend_from_slice(&self.background_ccr.to_le_bytes());
        bytes.extend_from_slice(&self.post_ccr.to_le_bytes());
        bytes.push(self.steps.len() as u8);
        bytes.extend_from_slice(&self.total_periods.to_le_bytes());
        for step in &self.steps {
            bytes.extend_from_slice(&step.end_period.to_le_bytes());
            bytes.extend_from_slice(&step.ccr.to_le_bytes());
        }
        bytes
    }

    /// Existing board v1 payload shape: format, sequence ID, background,
    /// post, step count, total periods, then end-period/CCR pairs.
    pub fn board_v1_payload(&self, sequence_id: u32) -> Result<Vec<u8>, CompileError> {
        if sequence_id == 0 {
            return Err(CompileError::SequenceId);
        }
        let mut bytes = Vec::with_capacity(14 + self.steps.len() * 6);
        bytes.push(FORMAT);
        bytes.extend_from_slice(&sequence_id.to_le_bytes());
        bytes.extend_from_slice(&self.background_ccr.to_le_bytes());
        bytes.extend_from_slice(&self.post_ccr.to_le_bytes());
        bytes.push(self.steps.len() as u8);
        bytes.extend_from_slice(&self.total_periods.to_le_bytes());
        for step in &self.steps {
            bytes.extend_from_slice(&step.end_period.to_le_bytes());
            bytes.extend_from_slice(&step.ccr.to_le_bytes());
        }
        Ok(bytes)
    }
}

#[derive(Clone, Debug, Error, Eq, PartialEq)]
pub enum CompileError {
    #[error("calibration endpoints are invalid")]
    Calibration,
    #[error("image waveform policy is invalid")]
    Policy,
    #[error("waveform must have one through the allowed maximum steps")]
    SegmentCount,
    #[error("target value is outside hardware or calibrated range")]
    Target,
    #[error("step duration is zero or its cumulative time overflows")]
    Duration,
    #[error("step is shorter than one representable PWM period")]
    Unrepresentable,
    #[error("sequence exceeds the board's 32-bit period range")]
    TooLong,
    #[error("consecutive short steps exceed the feedback window")]
    ShortSpan,
    #[error("experimental low-duty step exceeds its time bound")]
    ExperimentalHold,
    #[error("board sequence ID must be nonzero")]
    SequenceId,
    #[error("delayed rail trigger is outside MCU bounds")]
    Trigger,
}

pub fn compile(
    proposal: &Proposal,
    calibration: Calibration,
    policy: Policy,
) -> Result<Compiled, CompileError> {
    calibration.validate()?;
    policy.validate()?;
    if let Some(trigger) = proposal.delayed_trigger {
        trigger.validate()?;
    }
    if proposal.steps.is_empty() || proposal.steps.len() > policy.max_segments {
        return Err(CompileError::SegmentCount);
    }
    let background_ccr = target_ccr(proposal.background, calibration, policy)?;
    let post_ccr = target_ccr(proposal.post, calibration, policy)?;
    if background_ccr < policy.installed_min_ccr
        || post_ccr < policy.installed_min_ccr
        || !calibration.contains_ccr(background_ccr)
        || !calibration.contains_ccr(post_ccr)
    {
        return Err(CompileError::Target);
    }

    let mut cumulative_ns = 0_u128;
    let mut previous_period = 0_u32;
    let mut short_span = 0_u32;
    let mut steps = Vec::with_capacity(proposal.steps.len());

    for (index, step) in proposal.steps.iter().enumerate() {
        if step.duration_ns == 0 {
            return Err(CompileError::Duration);
        }
        let ccr = target_ccr(step.target, calibration, policy)?;
        if ccr >= policy.installed_min_ccr && !calibration.contains_ccr(ccr) {
            return Err(CompileError::Target);
        }
        cumulative_ns = cumulative_ns
            .checked_add(u128::from(step.duration_ns))
            .ok_or(CompileError::Duration)?;
        let numerator = cumulative_ns
            .checked_mul(PWM_HZ)
            .and_then(|value| value.checked_add(NS_PER_SECOND / 2))
            .ok_or(CompileError::Duration)?;
        let end = numerator / NS_PER_SECOND;
        if end > u128::from(u32::MAX) {
            return Err(CompileError::TooLong);
        }
        let end_period = end as u32;
        if end_period <= previous_period {
            return Err(CompileError::Unrepresentable);
        }
        let periods = end_period - previous_period;
        if ccr < policy.installed_min_ccr && periods > policy.experimental_low_max_periods {
            return Err(CompileError::ExperimentalHold);
        }
        if periods < policy.sustained_periods {
            short_span = short_span
                .checked_add(periods)
                .ok_or(CompileError::ShortSpan)?;
            if short_span > policy.short_span_max_periods {
                return Err(CompileError::ShortSpan);
            }
        } else {
            short_span = 0;
        }
        let actual_ns_num = i128::from(end_period) * NS_PER_SECOND as i128;
        let requested_ns_num = (cumulative_ns * PWM_HZ) as i128;
        steps.push(CompiledStep {
            index,
            ccr,
            start_period: previous_period,
            end_period,
            requested_end_ns: cumulative_ns,
            end_error_ns_numerator: actual_ns_num - requested_ns_num,
        });
        previous_period = end_period;
    }
    let mut compiled = Compiled {
        background_ccr,
        post_ccr,
        total_periods: previous_period,
        steps,
        delayed_trigger: proposal.delayed_trigger,
        waveform_digest: Digest::of([]),
    };
    compiled.waveform_digest = Digest::of(compiled.canonical_bytes());
    Ok(compiled)
}

fn target_ccr(
    target: Target,
    calibration: Calibration,
    policy: Policy,
) -> Result<u16, CompileError> {
    let ccr = match target {
        Target::Lowest => u32::from(policy.min_ccr),
        Target::Ccr(value) => u32::from(value),
        Target::DutyBasisPoints(value) if value <= 10_000 => {
            (u32::from(value) * TIMER_TICKS + 5_000) / 10_000
        }
        Target::DutyBasisPoints(_) => return Err(CompileError::Target),
        Target::Millivolts(value) if (calibration.mv1..=calibration.mv2).contains(&value) => {
            let numerator = i64::from(value - calibration.mv1)
                * (i64::from(calibration.ccr2) - i64::from(calibration.ccr1));
            let denominator = i64::from(calibration.mv2 - calibration.mv1);
            let rounded = (numerator.abs() + denominator / 2) / denominator;
            let signed =
                i64::from(calibration.ccr1) + if numerator >= 0 { rounded } else { -rounded };
            u32::try_from(signed).map_err(|_| CompileError::Target)?
        }
        Target::Millivolts(_) => return Err(CompileError::Target),
    };
    if ccr < u32::from(policy.min_ccr) || ccr > u32::from(policy.max_ccr) {
        return Err(CompileError::Target);
    }
    Ok(ccr as u16)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn example_waveform_is_representable_from_measured_profile() {
        let proposal: Proposal =
            serde_json::from_str(include_str!("../../../examples/example-waveform.json")).unwrap();
        let compiled = compile(
            &proposal,
            Calibration {
                mv1: 500,
                ccr1: 51,
                mv2: 1000,
                ccr2: 168,
            },
            Policy::legacy_v1(),
        )
        .unwrap();
        assert_eq!(compiled.background_ccr, 146);
        assert_eq!(compiled.post_ccr, 146);
        assert_eq!(
            compiled
                .steps
                .iter()
                .map(|step| step.ccr)
                .collect::<Vec<_>>(),
            vec![146, 2, 146, 3, 146]
        );
        assert_eq!(
            compiled.delayed_trigger,
            Some(DelayedTrigger {
                threshold_mv: 200,
                timeout_ms: 120_000
            })
        );
    }

    fn calibration() -> Calibration {
        Calibration {
            mv1: 700,
            ccr1: 155,
            mv2: 950,
            ccr2: 130,
        }
    }

    #[test]
    fn calibration_identity_binds_exact_compiler_endpoints() {
        let source = calibration();
        assert_eq!(
            source.canonical_bytes(),
            [1, 0xbc, 0x02, 0x9b, 0x00, 0xb6, 0x03, 0x82, 0x00]
        );
        let mut changed = source;
        changed.ccr2 += 1;
        assert_ne!(source.digest(), changed.digest());
    }

    #[test]
    fn policy_identity_changes_with_any_limit() {
        let source = Policy::legacy_v1();
        let mut changed = source;
        changed.experimental_low_max_periods += 1;
        assert_ne!(source.digest(), changed.digest());
        assert_eq!(source.canonical_bytes().len(), 27);
    }

    #[test]
    fn sequence_id_does_not_change_physical_waveform_digest() {
        let proposal = Proposal {
            background: Target::Millivolts(900),
            steps: vec![Step {
                target: Target::DutyBasisPoints(100),
                duration_ns: 14_000,
            }],
            post: Target::Millivolts(900),
            delayed_trigger: None,
        };
        let compiled = compile(&proposal, calibration(), Policy::legacy_v1()).unwrap();
        let first = compiled.board_v1_payload(1).unwrap();
        let second = compiled.board_v1_payload(2).unwrap();
        assert_ne!(first, second);
        assert_eq!(
            compiled.waveform_digest,
            Digest::of(compiled.canonical_bytes())
        );
    }

    #[test]
    fn unrepresentable_short_step_fails_before_hardware() {
        let proposal = Proposal {
            background: Target::Ccr(145),
            steps: vec![Step {
                target: Target::Lowest,
                duration_ns: 1,
            }],
            post: Target::Ccr(145),
            delayed_trigger: None,
        };
        assert_eq!(
            compile(&proposal, calibration(), Policy::legacy_v1()),
            Err(CompileError::Unrepresentable)
        );
    }

    #[test]
    fn delayed_trigger_bounds_are_checked_without_changing_waveform_bytes() {
        let mut proposal = Proposal {
            background: Target::Millivolts(900),
            steps: vec![Step {
                target: Target::Lowest,
                duration_ns: 98_000,
            }],
            post: Target::Millivolts(900),
            delayed_trigger: None,
        };
        let immediate = compile(&proposal, calibration(), Policy::legacy_v1()).unwrap();
        proposal.delayed_trigger = Some(DelayedTrigger {
            threshold_mv: 225,
            timeout_ms: 60_000,
        });
        let delayed = compile(&proposal, calibration(), Policy::legacy_v1()).unwrap();
        assert_eq!(immediate.canonical_bytes(), delayed.canonical_bytes());
        assert_eq!(delayed.delayed_trigger, proposal.delayed_trigger);
        proposal.delayed_trigger = Some(DelayedTrigger {
            threshold_mv: 149,
            timeout_ms: 60_000,
        });
        assert_eq!(
            compile(&proposal, calibration(), Policy::legacy_v1()),
            Err(CompileError::Trigger)
        );
    }
}

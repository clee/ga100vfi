//! One bounded GPU0 observation can be wrapped in the legacy enrolled format
//! or in a provisional physical format with no sample/calibration assertion.
//! Every successfully read word survives a later BAR failure.
use crate::maps::{Ga100Maps, MapError, RegisterMap, capture_named_partial};
use crate::{Gpu0Bar, Policy, capture_full512_partial};
use glitch_contract::{BaselineEvidence, Digest, ProvisionalBaseline};
use uuid::Uuid;

#[derive(Clone, Copy)]
pub struct BaselineSource {
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub sample_id: Digest,
    pub image_digest: Digest,
    pub calibration_digest: Digest,
}

#[derive(Clone)]
pub struct ProvisionalSource {
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub specimen_claim: Digest,
    pub activation_id: Uuid,
    pub active_generation: u64,
    pub image_digest: Digest,
    pub gpu0_bdf: String,
    pub gpu0_pci_id: u32,
}

/// Unclaimed read-only evidence, with no ledger or specimen-administration fields.
pub struct RawCapture {
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

fn empty_raw(maps: &Ga100Maps) -> RawCapture {
    RawCapture {
        option_map_digest: maps.options.sha256,
        status_map_digest: maps.statuses.sha256,
        core_map_digest: maps.core.sha256,
        rows_first: Vec::new(),
        rows_second: Vec::new(),
        options_before: Vec::new(),
        options_after: Vec::new(),
        statuses_before: Vec::new(),
        statuses_after: Vec::new(),
        core_before: Vec::new(),
        core_after: Vec::new(),
        failure: None,
    }
}

fn bounded_failure(reason: &str) -> String {
    let mut failure = reason.to_owned();
    if failure.len() > 1024 {
        let mut end = 1024;
        while !failure.is_char_boundary(end) {
            end -= 1;
        }
        failure.truncate(end);
    }
    failure
}

fn read_named<B: Gpu0Bar>(bar: &mut B, map: &RegisterMap, output: &mut Vec<u32>) -> Option<String> {
    let partial = capture_named_partial(bar, map);
    *output = partial.words.into_iter().map(|word| word.value).collect();
    partial.error.map(|error| error.to_string())
}

fn overlapping_words_match(
    options: &RegisterMap,
    option_words: &[u32],
    core: &RegisterMap,
    core_words: &[u32],
) -> bool {
    core.registers
        .iter()
        .enumerate()
        .all(|(core_index, register)| {
            options
                .registers
                .iter()
                .position(|option| option.offset == register.offset)
                .is_none_or(|option_index| {
                    core_words.get(core_index) == option_words.get(option_index)
                })
        })
}

fn capture_raw<B: Gpu0Bar>(
    bar: &mut B,
    policy: Policy,
    with_core: bool,
    with_rows: bool,
) -> Result<RawCapture, MapError> {
    let maps = Ga100Maps::embedded()?;
    let mut raw = empty_raw(&maps);
    if let Some(error) = read_named(bar, &maps.options, &mut raw.options_before) {
        raw.failure = Some(bounded_failure(&format!("option before: {error}")));
        return Ok(raw);
    }
    if let Some(error) = read_named(bar, &maps.statuses, &mut raw.statuses_before) {
        raw.failure = Some(bounded_failure(&format!("status before: {error}")));
        return Ok(raw);
    }
    if with_core && let Some(error) = read_named(bar, &maps.core, &mut raw.core_before) {
        raw.failure = Some(bounded_failure(&format!("core before: {error}")));
        return Ok(raw);
    }
    if with_core
        && !overlapping_words_match(
            &maps.options,
            &raw.options_before,
            &maps.core,
            &raw.core_before,
        )
    {
        raw.failure = Some("controller/published option before differs".into());
        return Ok(raw);
    }
    if with_rows {
        let rows = capture_full512_partial(bar, policy);
        raw.rows_first = rows.first;
        raw.rows_second = rows.second;
        if rows.read_error.is_some() || rows.restore_error.is_some() {
            let mut errors = Vec::new();
            if let Some(error) = rows.read_error {
                errors.push(format!("row read: {error}"));
            }
            if let Some(error) = rows.restore_error {
                errors.push(format!("address restore: {error}"));
            }
            raw.failure = Some(bounded_failure(&errors.join("; ")));
            return Ok(raw);
        }
    }
    if let Some(error) = read_named(bar, &maps.options, &mut raw.options_after) {
        raw.failure = Some(bounded_failure(&format!("option after: {error}")));
        return Ok(raw);
    }
    if let Some(error) = read_named(bar, &maps.statuses, &mut raw.statuses_after) {
        raw.failure = Some(bounded_failure(&format!("status after: {error}")));
        return Ok(raw);
    }
    if with_core && let Some(error) = read_named(bar, &maps.core, &mut raw.core_after) {
        raw.failure = Some(bounded_failure(&format!("core after: {error}")));
        return Ok(raw);
    }
    if with_core
        && !overlapping_words_match(
            &maps.options,
            &raw.options_after,
            &maps.core,
            &raw.core_after,
        )
    {
        raw.failure = Some("controller/published option after differs".into());
    }
    Ok(raw)
}

fn failed_raw(reason: &str) -> Result<RawCapture, MapError> {
    let maps = Ga100Maps::embedded()?;
    let mut raw = empty_raw(&maps);
    raw.failure = Some(bounded_failure(&format!("GPU0 BAR0 open: {reason}")));
    Ok(raw)
}

fn legacy(source: BaselineSource, raw: RawCapture) -> BaselineEvidence {
    BaselineEvidence {
        version: 1,
        capture_id: source.capture_id,
        boot_id: source.boot_id,
        sample_id: source.sample_id,
        image_digest: source.image_digest,
        calibration_digest: source.calibration_digest,
        option_map_digest: raw.option_map_digest,
        status_map_digest: raw.status_map_digest,
        rows_first: raw.rows_first,
        rows_second: raw.rows_second,
        options_before: raw.options_before,
        options_after: raw.options_after,
        statuses_before: raw.statuses_before,
        statuses_after: raw.statuses_after,
        simulated: false,
        failure: raw.failure,
    }
}

fn provisional(source: ProvisionalSource, raw: RawCapture) -> ProvisionalBaseline {
    ProvisionalBaseline {
        version: 4,
        capture_id: source.capture_id,
        boot_id: source.boot_id,
        specimen_claim: source.specimen_claim,
        activation_id: source.activation_id,
        active_generation: source.active_generation,
        image_digest: source.image_digest,
        gpu0_bdf: source.gpu0_bdf,
        gpu0_pci_id: source.gpu0_pci_id,
        option_map_digest: raw.option_map_digest,
        status_map_digest: raw.status_map_digest,
        core_map_digest: raw.core_map_digest,
        rows_first: raw.rows_first,
        rows_second: raw.rows_second,
        options_before: raw.options_before,
        options_after: raw.options_after,
        statuses_before: raw.statuses_before,
        statuses_after: raw.statuses_after,
        core_before: raw.core_before,
        core_after: raw.core_after,
        failure: raw.failure,
    }
}

pub fn capture_baseline<B: Gpu0Bar>(
    bar: &mut B,
    source: BaselineSource,
    policy: Policy,
) -> Result<BaselineEvidence, MapError> {
    Ok(legacy(source, capture_raw(bar, policy, false, true)?))
}

pub fn capture_provisional<B: Gpu0Bar>(
    bar: &mut B,
    source: ProvisionalSource,
    policy: Policy,
) -> Result<ProvisionalBaseline, MapError> {
    Ok(provisional(source, capture_raw(bar, policy, true, true)?))
}

pub fn capture_raw_unclaimed<B: Gpu0Bar>(
    bar: &mut B,
    policy: Policy,
) -> Result<RawCapture, MapError> {
    capture_raw(bar, policy, true, true)
}

/// Post-glitch latch/controller observation. The boot-enrollment row image
/// remains the physical-row source until a burn episode changes it.
pub fn capture_raw_compact_unclaimed<B: Gpu0Bar>(bar: &mut B) -> Result<RawCapture, MapError> {
    capture_raw(bar, Policy::legacy_readonly(), true, false)
}

pub fn failed_raw_unclaimed(reason: &str) -> Result<RawCapture, MapError> {
    failed_raw(reason)
}

pub fn failed_before_open(
    source: BaselineSource,
    reason: &str,
) -> Result<BaselineEvidence, MapError> {
    Ok(legacy(source, failed_raw(reason)?))
}

pub fn provisional_failed_before_open(
    source: ProvisionalSource,
    reason: &str,
) -> Result<ProvisionalBaseline, MapError> {
    Ok(provisional(source, failed_raw(reason)?))
}

#[cfg(test)]
mod compact_tests {
    use super::*;

    struct NoFuseRows;
    impl Gpu0Bar for NoFuseRows {
        fn read32(&mut self, offset: u32) -> Result<u32, String> {
            Ok(offset)
        }
        fn select_fuse_row(&mut self, _: u16) -> Result<(), String> {
            panic!("compact capture selected a fuse row")
        }
        fn issue_cmd_read(&mut self) -> Result<(), String> {
            panic!("compact capture issued CMD_READ")
        }
        fn restore_fuse_address(&mut self, _: u32) -> Result<(), String> {
            panic!("compact capture touched fuse address")
        }
    }

    #[test]
    fn compact_reads_named_latches_without_physical_rows() {
        let raw = capture_raw_compact_unclaimed(&mut NoFuseRows).unwrap();
        assert!(raw.failure.is_none());
        assert!(raw.rows_first.is_empty() && raw.rows_second.is_empty());
        assert_eq!(raw.options_before.len(), 672);
        assert_eq!(raw.statuses_before.len(), 84);
        assert_eq!(raw.core_before.len(), 15);
        assert_eq!(raw.options_before, raw.options_after);
        assert_eq!(raw.statuses_before, raw.statuses_after);
        assert_eq!(raw.core_before, raw.core_after);
    }
}

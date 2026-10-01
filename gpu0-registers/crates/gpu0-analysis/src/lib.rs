//! Pure controller-side analysis of immutable GA100 GPU0 captures. This crate
//! reports observations and preservation relationships, with no actuator
//! or FF-burn eligibility approval path.
use glitch_contract::{BaselineError, Digest, ProvisionalBaseline, valid_pci_bdf};
use gpu0_capture::maps::{Ga100Maps, MapError, RegisterMap};
use leaf_api::Baseline;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use thiserror::Error;
use uuid::Uuid;

/// The 17 published option aliases historically compared with security
/// rows 0/1. They are BAR0 option reads, never physical CMD_READ rows.
pub const PUBLISHED_SECURITY_ALIASES: [&str; 17] = [
    "NV_FUSE_OPT_DEBUG_FEATURE_SW_OVERRIDE_ENABLE",
    "NV_FUSE_OPT_DEBUG_SW_OVERRIDE_ENABLE",
    "NV_FUSE_OPT_HDCP_RAM_ACCESS_DIS",
    "NV_FUSE_OPT_IST_DEBUG_FA",
    "NV_FUSE_OPT_PRIVKEY_ACC_DIS",
    "NV_FUSE_OPT_PRIV_SEC_EN",
    "NV_FUSE_OPT_SECURED_BURN_FEATURE_ENABLE",
    "NV_FUSE_OPT_SECURE_FUSE_RESENSE_DISABLE",
    "NV_FUSE_OPT_SECURE_GSP_DEBUG_DIS",
    "NV_FUSE_OPT_SECURE_MINION_DEBUG_DIS",
    "NV_FUSE_OPT_SECURE_NVDEC_DEBUG_DIS",
    "NV_FUSE_OPT_SECURE_PMU_DEBUG_DIS",
    "NV_FUSE_OPT_SECURE_PRI_SOURCE_ISOLATION_EN",
    "NV_FUSE_OPT_SECURE_SECENGINE_DEBUG_DIS",
    "NV_FUSE_OPT_SECURE_XUSB_DEBUG_DIS",
    "NV_FUSE_OPT_SFK_ACC_DIS",
    "NV_FUSE_OPT_XV2CLK_RESET_RETIME_DISABLE",
];

const CORE_FACTS: [&str; 12] = [
    "PMC_BOOT_0",
    "FUSECTRL",
    "CRC_STATUS",
    "RING_GLOBAL_CTL",
    "OPT_SECURED_BURN_FEATURE_ENABLE",
    "EN_SW_OVERRIDE",
    "DISABLE_SW_OVERRIDE_STATUS",
    "ENABLE_FUSE_PROGRAM_STATUS",
    "DISABLE_FUSE_PROGRAM_STATUS",
    "FUSECTRL_PLM",
    "FUSEOPT_PLM",
    "FUSEWDATA_PLM",
];

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Relationship {
    IncompletePreserve,
    UncomparedPreserve,
    BaselineMatch,
    ChangedPreserve,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Eligibility {
    Unproven,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct PhysicalRows {
    pub row0: Option<u32>,
    pub row1: Option<u32>,
    pub row18: Option<u32>,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Assessment {
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub stable_fingerprint: Option<Digest>,
    pub relationship: Relationship,
    pub published_alias_values: BTreeMap<String, Option<u32>>,
    pub published_alias_zero_count: Option<u8>,
    pub physical_rows: PhysicalRows,
    pub controller_values: BTreeMap<String, Option<u32>>,
    pub crc55_idle_ring: bool,
    pub direct_register_indicators: bool,
    pub ff_burn_eligibility: Eligibility,
}

#[derive(Debug, Error)]
pub enum AnalysisError {
    #[error(transparent)]
    Baseline(#[from] BaselineError),
    #[error(transparent)]
    Map(#[from] MapError),
    #[error("capture map digest differs from the pinned GA100 maps")]
    MapDigest,
    #[error("baseline source or stable enrollment identity differs")]
    BaselineSource,
    #[error("pinned alias or controller name is missing")]
    MissingName,
    #[error("GPU0 PCI BDF is invalid")]
    Bdf,
    #[error("controller and published option reads disagree at one BAR0 address")]
    MapOverlap,
    #[error("leaf capture source differs from the expected boot, image or GPU0")]
    LeafSource,
}

/// A necessary FF eligibility condition is independently false. This does
/// not assert that the pulse ran or that the image is otherwise healthy.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
#[serde(tag = "result", rename_all = "snake_case")]
pub enum LeafFfExclusion {
    Preserve,
    InvalidControllerState { crc: u32 },
    PublishedAliasNonzero { name: String, value: u32 },
}

#[derive(Clone, Debug, Serialize)]
pub struct CompactFacts {
    pub capture_id: Uuid,
    pub boot_id: Uuid,
    pub physical_rows_read: bool,
    pub published_maps_complete_and_stable: bool,
    pub crc_status: Option<u32>,
    pub fusectrl: Option<u32>,
    pub ring_global_ctl: Option<u32>,
    pub published_alias_zero_count: Option<u8>,
    pub published_alias_nonzero: BTreeMap<String, u32>,
    pub disable_sw_override_status: Option<u32>,
    pub secured_burn_feature_enable: Option<u32>,
    pub fusectrl_plm: Option<u32>,
    pub fuseopt_plm: Option<u32>,
    pub fusewdata_plm: Option<u32>,
    pub exclusion: LeafFfExclusion,
}

pub fn compact_facts(
    capture: &Baseline,
    expected_boot: Uuid,
    expected_image: Digest,
    expected_gpu0_pci_id: u32,
) -> Result<CompactFacts, AnalysisError> {
    let exclusion =
        exclude_compact_leaf_ff(capture, expected_boot, expected_image, expected_gpu0_pci_id)?;
    let maps = Ga100Maps::embedded()?;
    let mut nonzero = BTreeMap::new();
    let mut zeros = 0_u8;
    let mut all_aliases = true;
    for name in PUBLISHED_SECURITY_ALIASES {
        match word(&maps.options, &capture.options_before, name)? {
            Some(0) => zeros += 1,
            Some(value) => {
                nonzero.insert(name.to_owned(), value);
            }
            None => all_aliases = false,
        }
    }
    let core = |name| word(&maps.core, &capture.core_before, name);
    Ok(CompactFacts {
        capture_id: capture.capture_id,
        boot_id: capture.boot_id,
        physical_rows_read: !capture.rows_first.is_empty() || !capture.rows_second.is_empty(),
        published_maps_complete_and_stable: capture.failure.is_none()
            && capture.options_before.len() == maps.options.registers.len()
            && capture.statuses_before.len() == maps.statuses.registers.len()
            && capture.core_before.len() == maps.core.registers.len()
            && capture.options_before == capture.options_after
            && capture.statuses_before == capture.statuses_after
            && capture.core_before == capture.core_after,
        crc_status: core("CRC_STATUS")?,
        fusectrl: core("FUSECTRL")?,
        ring_global_ctl: core("RING_GLOBAL_CTL")?,
        published_alias_zero_count: all_aliases.then_some(zeros),
        published_alias_nonzero: nonzero,
        disable_sw_override_status: core("DISABLE_SW_OVERRIDE_STATUS")?,
        secured_burn_feature_enable: core("OPT_SECURED_BURN_FEATURE_ENABLE")?,
        fusectrl_plm: core("FUSECTRL_PLM")?,
        fuseopt_plm: core("FUSEOPT_PLM")?,
        fusewdata_plm: core("FUSEWDATA_PLM")?,
        exclusion,
    })
}

/// Decide only necessary FF conditions from a post-glitch compact latch
/// observation. Physical rows are represented by the paired boot enrollment;
/// reading them again after an ordinary glitch would add no new row evidence.
pub fn exclude_compact_leaf_ff(
    capture: &Baseline,
    expected_boot: Uuid,
    expected_image: Digest,
    expected_gpu0_pci_id: u32,
) -> Result<LeafFfExclusion, AnalysisError> {
    if capture.boot_id != expected_boot
        || capture.image_digest != expected_image
        || capture.gpu0_pci_id != expected_gpu0_pci_id
        || !capture.rows_first.is_empty()
        || !capture.rows_second.is_empty()
    {
        return Err(AnalysisError::LeafSource);
    }
    let maps = Ga100Maps::embedded()?;
    if capture.option_map_digest != maps.options.sha256
        || capture.status_map_digest != maps.statuses.sha256
        || capture.core_map_digest != maps.core.sha256
    {
        return Err(AnalysisError::MapDigest);
    }
    if capture.failure.is_some()
        || capture.options_before.len() != maps.options.registers.len()
        || capture.statuses_before.len() != maps.statuses.registers.len()
        || capture.core_before.len() != maps.core.registers.len()
        || capture.options_before != capture.options_after
        || capture.statuses_before != capture.statuses_after
        || capture.core_before != capture.core_after
    {
        return Ok(LeafFfExclusion::Preserve);
    }
    for (core_index, core) in maps.core.registers.iter().enumerate() {
        if let Some(option_index) = maps
            .options
            .registers
            .iter()
            .position(|option| option.offset == core.offset)
            && capture.core_before[core_index] != capture.options_before[option_index]
        {
            return Err(AnalysisError::MapOverlap);
        }
    }
    let core = |name| word(&maps.core, &capture.core_before, name);
    let pmc = core("PMC_BOOT_0")?.ok_or(AnalysisError::MissingName)?;
    if pmc == 0 || pmc == u32::MAX || pmc & 0xffff_0000 == 0xbadf_0000 {
        return Ok(LeafFfExclusion::Preserve);
    }
    let crc = core("CRC_STATUS")?.ok_or(AnalysisError::MissingName)?;
    if crc != 0x55 {
        return Ok(LeafFfExclusion::InvalidControllerState { crc });
    }
    if core("FUSECTRL")? != Some(0xe004_0000) || core("RING_GLOBAL_CTL")? != Some(0) {
        return Ok(LeafFfExclusion::Preserve);
    }
    for name in PUBLISHED_SECURITY_ALIASES {
        match word(&maps.options, &capture.options_before, name)? {
            Some(value) if value != 0 => {
                return Ok(LeafFfExclusion::PublishedAliasNonzero {
                    name: name.to_owned(),
                    value,
                });
            }
            Some(_) => {}
            None => return Ok(LeafFfExclusion::Preserve),
        }
    }
    Ok(LeafFfExclusion::Preserve)
}

/// Conservatively refute FF eligibility from one complete, internally
/// consistent leaf capture. All uncertain evidence stays `Preserve`.
pub fn exclude_leaf_ff(
    capture: &Baseline,
    expected_boot: Uuid,
    expected_image: Digest,
    expected_gpu0_pci_id: u32,
) -> Result<LeafFfExclusion, AnalysisError> {
    if capture.boot_id != expected_boot
        || capture.image_digest != expected_image
        || capture.gpu0_pci_id != expected_gpu0_pci_id
    {
        return Err(AnalysisError::LeafSource);
    }
    let maps = Ga100Maps::embedded()?;
    if capture.option_map_digest != maps.options.sha256
        || capture.status_map_digest != maps.statuses.sha256
        || capture.core_map_digest != maps.core.sha256
    {
        return Err(AnalysisError::MapDigest);
    }
    if capture.stable_fingerprint().is_none() {
        return Ok(LeafFfExclusion::Preserve);
    }
    for (core_index, core) in maps.core.registers.iter().enumerate() {
        if let Some(option_index) = maps
            .options
            .registers
            .iter()
            .position(|option| option.offset == core.offset)
            && capture.core_before[core_index] != capture.options_before[option_index]
        {
            return Err(AnalysisError::MapOverlap);
        }
    }
    let core = |name| word(&maps.core, &capture.core_before, name);
    let pmc = core("PMC_BOOT_0")?.ok_or(AnalysisError::MissingName)?;
    if pmc == 0
        || pmc == u32::MAX
        || pmc & 0xffff_0000 == 0xbadf_0000
        || core("FUSECTRL")? != Some(0xe004_0000)
        || core("CRC_STATUS")? != Some(0x55)
        || core("RING_GLOBAL_CTL")? != Some(0)
    {
        return Ok(LeafFfExclusion::Preserve);
    }
    for name in PUBLISHED_SECURITY_ALIASES {
        match word(&maps.options, &capture.options_before, name)? {
            Some(value) if value != 0 => {
                return Ok(LeafFfExclusion::PublishedAliasNonzero {
                    name: name.to_owned(),
                    value,
                });
            }
            Some(_) => {}
            None => return Ok(LeafFfExclusion::Preserve),
        }
    }
    Ok(LeafFfExclusion::Preserve)
}

fn word(map: &RegisterMap, values: &[u32], name: &str) -> Result<Option<u32>, AnalysisError> {
    let index = map
        .registers
        .iter()
        .position(|register| register.name == name)
        .ok_or(AnalysisError::MissingName)?;
    Ok(values.get(index).copied())
}

fn validate_maps(capture: &ProvisionalBaseline, maps: &Ga100Maps) -> Result<(), AnalysisError> {
    if !valid_pci_bdf(&capture.gpu0_bdf) {
        return Err(AnalysisError::Bdf);
    }
    if capture.option_map_digest != maps.options.sha256
        || capture.status_map_digest != maps.statuses.sha256
        || capture.core_map_digest != maps.core.sha256
    {
        return Err(AnalysisError::MapDigest);
    }
    for (core_index, core) in maps.core.registers.iter().enumerate() {
        if let Some(option_index) = maps
            .options
            .registers
            .iter()
            .position(|option| option.offset == core.offset)
        {
            for (options, controller) in [
                (&capture.options_before, &capture.core_before),
                (&capture.options_after, &capture.core_after),
            ] {
                if let (Some(option), Some(core)) =
                    (options.get(option_index), controller.get(core_index))
                    && option != core
                {
                    return Err(AnalysisError::MapOverlap);
                }
            }
        }
    }
    Ok(())
}

pub fn analyze(
    candidate: &ProvisionalBaseline,
    baseline: Option<&ProvisionalBaseline>,
) -> Result<Assessment, AnalysisError> {
    let maps = Ga100Maps::embedded()?;
    validate_maps(candidate, &maps)?;
    let stable = candidate.stable_fingerprint()?;
    let baseline_fingerprint = baseline
        .map(|enrolled| {
            validate_maps(enrolled, &maps)?;
            if candidate.specimen_claim != enrolled.specimen_claim
                || candidate.gpu0_pci_id != enrolled.gpu0_pci_id
                || candidate.gpu0_bdf != enrolled.gpu0_bdf
            {
                return Err(AnalysisError::BaselineSource);
            }
            enrolled
                .stable_fingerprint()?
                .ok_or(AnalysisError::BaselineSource)
        })
        .transpose()?;
    let relationship = match (stable, baseline_fingerprint) {
        (None, _) => Relationship::IncompletePreserve,
        (Some(_), None) => Relationship::UncomparedPreserve,
        (Some(actual), Some(expected)) if actual == expected => Relationship::BaselineMatch,
        (Some(_), Some(_)) => Relationship::ChangedPreserve,
    };
    let mut aliases = BTreeMap::new();
    for name in PUBLISHED_SECURITY_ALIASES {
        aliases.insert(
            name.to_owned(),
            word(&maps.options, &candidate.options_before, name)?,
        );
    }
    let zero_count = aliases
        .values()
        .all(Option::is_some)
        .then(|| aliases.values().filter(|value| **value == Some(0)).count() as u8);
    let mut core = BTreeMap::new();
    for name in CORE_FACTS {
        core.insert(
            name.to_owned(),
            word(&maps.core, &candidate.core_before, name)?,
        );
    }
    let core_value = |name: &str| core.get(name).copied().flatten();
    let controller_valid = stable.is_some()
        && core_value("FUSECTRL") == Some(0xe004_0000)
        && core_value("CRC_STATUS") == Some(0x55)
        && core_value("RING_GLOBAL_CTL") == Some(0);
    let direct_register_indicators = controller_valid
        && zero_count == Some(17)
        && core_value("DISABLE_SW_OVERRIDE_STATUS") == Some(0)
        && core_value("OPT_SECURED_BURN_FEATURE_ENABLE") == Some(0)
        && core_value("EN_SW_OVERRIDE") == Some(1)
        && core_value("DISABLE_FUSE_PROGRAM_STATUS") == Some(0)
        && core_value("FUSECTRL_PLM") == Some(u32::MAX)
        && core_value("FUSEOPT_PLM") == Some(u32::MAX)
        && core_value("FUSEWDATA_PLM") == Some(u32::MAX)
        && word(
            &maps.options,
            &candidate.options_before,
            "NV_FUSE_OPT_SECURE_FUSE_WDATA_WR_SECURE",
        )? == Some(0);
    Ok(Assessment {
        capture_id: candidate.capture_id,
        boot_id: candidate.boot_id,
        stable_fingerprint: stable,
        relationship,
        published_alias_values: aliases,
        published_alias_zero_count: zero_count,
        physical_rows: PhysicalRows {
            row0: candidate.rows_first.first().copied(),
            row1: candidate.rows_first.get(1).copied(),
            row18: candidate.rows_first.get(18).copied(),
        },
        controller_values: core,
        crc55_idle_ring: controller_valid,
        direct_register_indicators,
        ff_burn_eligibility: Eligibility::Unproven,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn compact_capture_excludes_by_crc_or_published_alias_without_rows() {
        let maps = Ga100Maps::embedded().unwrap();
        let boot = Uuid::new_v4();
        let image = Digest::of(b"image");
        let mut capture = Baseline {
            capture_id: Uuid::new_v4(),
            boot_id: boot,
            image_digest: image,
            gpu0_pci_id: 0x20c2_10de,
            option_map_digest: maps.options.sha256,
            status_map_digest: maps.statuses.sha256,
            core_map_digest: maps.core.sha256,
            rows_first: vec![],
            rows_second: vec![],
            options_before: vec![0; maps.options.registers.len()],
            options_after: vec![0; maps.options.registers.len()],
            statuses_before: vec![0; maps.statuses.registers.len()],
            statuses_after: vec![0; maps.statuses.registers.len()],
            core_before: vec![0; maps.core.registers.len()],
            core_after: vec![0; maps.core.registers.len()],
            failure: None,
        };
        let core_index = |name: &str| {
            maps.core
                .registers
                .iter()
                .position(|r| r.name == name)
                .unwrap()
        };
        capture.core_before[core_index("PMC_BOOT_0")] = 1;
        capture.core_before[core_index("FUSECTRL")] = 0xe004_0000;
        capture.core_before[core_index("CRC_STATUS")] = 0x77;
        capture.core_after = capture.core_before.clone();
        assert_eq!(
            exclude_compact_leaf_ff(&capture, boot, image, 0x20c2_10de).unwrap(),
            LeafFfExclusion::InvalidControllerState { crc: 0x77 }
        );
        capture.core_before[core_index("CRC_STATUS")] = 0x55;
        capture.core_after = capture.core_before.clone();
        let alias_index = maps
            .options
            .registers
            .iter()
            .position(|r| r.name == PUBLISHED_SECURITY_ALIASES[0])
            .unwrap();
        capture.options_before[alias_index] = 1;
        capture.options_after = capture.options_before.clone();
        assert!(matches!(
            exclude_compact_leaf_ff(&capture, boot, image, 0x20c2_10de).unwrap(),
            LeafFfExclusion::PublishedAliasNonzero { .. }
        ));
        let facts = compact_facts(&capture, boot, image, 0x20c2_10de).unwrap();
        assert!(!facts.physical_rows_read);
        assert!(facts.published_maps_complete_and_stable);
        assert_eq!(facts.crc_status, Some(0x55));
        assert_eq!(facts.published_alias_zero_count, Some(16));
        assert_eq!(facts.published_alias_nonzero.len(), 1);
    }

    fn record() -> ProvisionalBaseline {
        let maps = Ga100Maps::embedded().unwrap();
        let mut core = vec![0; 15];
        let set = |values: &mut Vec<u32>, map: &RegisterMap, name: &str, value: u32| {
            let index = map
                .registers
                .iter()
                .position(|word| word.name == name)
                .unwrap();
            values[index] = value;
        };
        set(&mut core, &maps.core, "FUSECTRL", 0xe004_0000);
        set(&mut core, &maps.core, "CRC_STATUS", 0x55);
        set(&mut core, &maps.core, "FUSECTRL_PLM", u32::MAX);
        set(&mut core, &maps.core, "FUSEOPT_PLM", u32::MAX);
        set(&mut core, &maps.core, "FUSEWDATA_PLM", u32::MAX);
        set(&mut core, &maps.core, "EN_SW_OVERRIDE", 1);
        let mut rows = vec![0; 512];
        rows[0] = 0x5355_7c3c;
        rows[1] = 0x1020_3040;
        rows[18] = 0xfcc8_04f8;
        ProvisionalBaseline {
            version: 4,
            capture_id: Uuid::new_v4(),
            boot_id: Uuid::new_v4(),
            specimen_claim: Digest::of(b"claim"),
            activation_id: Uuid::new_v4(),
            active_generation: 1,
            image_digest: Digest::of(b"image"),
            gpu0_bdf: "0000:01:00.0".into(),
            gpu0_pci_id: 0x20c2_10de,
            option_map_digest: maps.options.sha256,
            status_map_digest: maps.statuses.sha256,
            core_map_digest: maps.core.sha256,
            rows_first: rows.clone(),
            rows_second: rows,
            options_before: vec![0; 672],
            options_after: vec![0; 672],
            statuses_before: vec![0; 84],
            statuses_after: vec![0; 84],
            core_before: core.clone(),
            core_after: core,
            failure: None,
        }
    }

    fn leaf_record() -> Baseline {
        let source = record();
        let maps = Ga100Maps::embedded().unwrap();
        let mut core = source.core_before;
        let pmc = maps
            .core
            .registers
            .iter()
            .position(|word| word.name == "PMC_BOOT_0")
            .unwrap();
        core[pmc] = 0x20c2_0001;
        Baseline {
            capture_id: source.capture_id,
            boot_id: source.boot_id,
            image_digest: source.image_digest,
            gpu0_pci_id: source.gpu0_pci_id,
            option_map_digest: source.option_map_digest,
            status_map_digest: source.status_map_digest,
            core_map_digest: source.core_map_digest,
            rows_first: source.rows_first,
            rows_second: source.rows_second,
            options_before: source.options_before,
            options_after: source.options_after,
            statuses_before: source.statuses_before,
            statuses_after: source.statuses_after,
            core_before: core.clone(),
            core_after: core,
            failure: None,
        }
    }

    fn leaf_result(capture: &Baseline) -> Result<LeafFfExclusion, AnalysisError> {
        exclude_leaf_ff(
            capture,
            capture.boot_id,
            capture.image_digest,
            capture.gpu0_pci_id,
        )
    }

    #[test]
    fn complete_crc55_image_with_nonzero_published_alias_is_ff_ineligible() {
        let mut capture = leaf_record();
        let maps = Ga100Maps::embedded().unwrap();
        let name = PUBLISHED_SECURITY_ALIASES[0];
        let index = maps
            .options
            .registers
            .iter()
            .position(|word| word.name == name)
            .unwrap();
        capture.options_before[index] = 1;
        capture.options_after[index] = 1;
        assert_eq!(
            leaf_result(&capture).unwrap(),
            LeafFfExclusion::PublishedAliasNonzero {
                name: name.into(),
                value: 1,
            }
        );
        let crc = maps
            .core
            .registers
            .iter()
            .position(|word| word.name == "CRC_STATUS")
            .unwrap();
        capture.core_before[crc] = 0x77;
        capture.core_after[crc] = 0x77;
        assert_eq!(leaf_result(&capture).unwrap(), LeafFfExclusion::Preserve);
    }

    #[test]
    fn incomplete_or_all_zero_leaf_capture_stays_preserved() {
        let mut capture = leaf_record();
        assert_eq!(leaf_result(&capture).unwrap(), LeafFfExclusion::Preserve);
        capture.rows_second.pop();
        assert_eq!(leaf_result(&capture).unwrap(), LeafFfExclusion::Preserve);
    }

    #[test]
    fn leaf_source_and_map_drift_are_errors() {
        let mut capture = leaf_record();
        assert!(matches!(
            exclude_leaf_ff(
                &capture,
                Uuid::new_v4(),
                capture.image_digest,
                capture.gpu0_pci_id
            ),
            Err(AnalysisError::LeafSource)
        ));
        capture.option_map_digest = Digest::of(b"wrong map");
        assert!(matches!(
            leaf_result(&capture),
            Err(AnalysisError::MapDigest)
        ));
    }

    #[test]
    fn leaf_overlapping_option_and_controller_reads_must_agree() {
        let mut capture = leaf_record();
        let maps = Ga100Maps::embedded().unwrap();
        let core_index = maps
            .core
            .registers
            .iter()
            .enumerate()
            .find_map(|(core_index, core)| {
                maps.options
                    .registers
                    .iter()
                    .any(|option| option.offset == core.offset)
                    .then_some(core_index)
            })
            .unwrap();
        capture.core_before[core_index] ^= 1;
        capture.core_after[core_index] ^= 1;
        assert!(matches!(
            leaf_result(&capture),
            Err(AnalysisError::MapOverlap)
        ));
    }

    #[test]
    fn zero_aliases_and_direct_registers_still_do_not_prove_eligibility() {
        let baseline = record();
        let candidate = baseline.clone();
        let result = analyze(&candidate, Some(&baseline)).unwrap();
        assert_eq!(result.published_alias_zero_count, Some(17));
        assert_eq!(result.physical_rows.row0, Some(0x5355_7c3c));
        assert_eq!(result.relationship, Relationship::BaselineMatch);
        assert!(result.direct_register_indicators);
        assert_eq!(result.ff_burn_eligibility, Eligibility::Unproven);
    }

    #[test]
    fn invalid_crc_with_zero_aliases_is_preserved() {
        let baseline = record();
        let mut candidate = baseline.clone();
        let maps = Ga100Maps::embedded().unwrap();
        let crc = maps
            .core
            .registers
            .iter()
            .position(|word| word.name == "CRC_STATUS")
            .unwrap();
        candidate.core_before[crc] = 0x77;
        candidate.core_after[crc] = 0x77;
        let result = analyze(&candidate, Some(&baseline)).unwrap();
        assert_eq!(result.published_alias_zero_count, Some(17));
        assert_eq!(result.relationship, Relationship::ChangedPreserve);
        assert!(!result.crc55_idle_ring);
        assert!(!result.direct_register_indicators);
        assert_eq!(result.ff_burn_eligibility, Eligibility::Unproven);
    }

    #[test]
    fn partial_or_unpinned_capture_never_matches_baseline() {
        let baseline = record();
        let mut candidate = baseline.clone();
        candidate.rows_second.clear();
        assert_eq!(
            analyze(&candidate, Some(&baseline)).unwrap().relationship,
            Relationship::IncompletePreserve
        );
        candidate.core_map_digest = Digest::of(b"wrong map");
        assert!(matches!(
            analyze(&candidate, Some(&baseline)),
            Err(AnalysisError::MapDigest)
        ));
    }

    #[test]
    fn forged_published_sbfe_and_controller_overlap_is_rejected() {
        let mut candidate = record();
        let maps = Ga100Maps::embedded().unwrap();
        let core_index = maps
            .core
            .registers
            .iter()
            .position(|word| word.name == "OPT_SECURED_BURN_FEATURE_ENABLE")
            .unwrap();
        candidate.core_before[core_index] = 1;
        candidate.core_after[core_index] = 1;
        assert!(matches!(
            analyze(&candidate, None),
            Err(AnalysisError::MapOverlap)
        ));
    }
}

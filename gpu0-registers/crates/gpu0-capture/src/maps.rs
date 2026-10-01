//! Versioned GA100 published option and status register maps. These are BAR0
//! reads, separate from the physical CMD_READ row image.
use crate::{BAR0_MAP_LENGTH, Gpu0Bar};
use glitch_contract::Digest;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, BTreeSet};
use thiserror::Error;

const OPTIONS_JSON: &str = include_str!("../data/ga100-options.json");
const STATUS_JSON: &str = include_str!("../data/ga100-status.json");
const CORE_JSON: &str = include_str!("../data/ga100-core.json");

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct RegisterOffset {
    pub name: String,
    pub offset: u32,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct NamedWord {
    pub name: String,
    pub offset: u32,
    pub value: u32,
}

#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct NamedSnapshot {
    pub map_sha256: Digest,
    pub values_sha256_le_u32: Digest,
    pub words: Vec<NamedWord>,
}

/// The prefix of a named map that was read before the first BAR failure.
pub struct PartialNamedSnapshot {
    pub words: Vec<NamedWord>,
    pub error: Option<MapError>,
}

impl NamedSnapshot {
    pub fn get(&self, name: &str) -> Option<u32> {
        self.words
            .iter()
            .find(|word| word.name == name)
            .map(|word| word.value)
    }
}

#[derive(Clone, Debug)]
pub struct RegisterMap {
    pub sha256: Digest,
    pub registers: Vec<RegisterOffset>,
}

#[derive(Clone, Debug)]
pub struct Ga100Maps {
    pub options: RegisterMap,
    pub statuses: RegisterMap,
    pub core: RegisterMap,
}

#[derive(Debug, Error)]
pub enum MapError {
    #[error("register map JSON: {0}")]
    Json(#[from] serde_json::Error),
    #[error("register map name, count or offset differs")]
    Invalid,
    #[error("GPU0 BAR read failed at {0:#x}: {1}")]
    Read(u32, String),
}

fn parse(text: &str, expected_count: usize) -> Result<RegisterMap, MapError> {
    let entries: BTreeMap<String, String> = serde_json::from_str(text)?;
    if entries.len() != expected_count {
        return Err(MapError::Invalid);
    }
    let mut seen_offsets = BTreeSet::new();
    let mut registers = Vec::with_capacity(entries.len());
    for (name, value) in entries {
        let hex = value.strip_prefix("0x").ok_or(MapError::Invalid)?;
        let offset = u32::from_str_radix(hex, 16).map_err(|_| MapError::Invalid)?;
        if name.is_empty()
            || !offset.is_multiple_of(4)
            || offset as usize + 4 > BAR0_MAP_LENGTH
            || !seen_offsets.insert(offset)
        {
            return Err(MapError::Invalid);
        }
        registers.push(RegisterOffset { name, offset });
    }
    registers.sort_by_key(|register| register.offset);
    Ok(RegisterMap {
        sha256: Digest::of(text.as_bytes()),
        registers,
    })
}

impl Ga100Maps {
    pub fn embedded() -> Result<Self, MapError> {
        Ok(Self {
            options: parse(OPTIONS_JSON, 672)?,
            statuses: parse(STATUS_JSON, 84)?,
            core: parse(CORE_JSON, 15)?,
        })
    }
}

pub fn capture_named_partial<B: Gpu0Bar>(bar: &mut B, map: &RegisterMap) -> PartialNamedSnapshot {
    let mut words = Vec::with_capacity(map.registers.len());
    for register in &map.registers {
        let value = match bar.read32(register.offset) {
            Ok(value) => value,
            Err(error) => {
                return PartialNamedSnapshot {
                    words,
                    error: Some(MapError::Read(register.offset, error)),
                };
            }
        };
        words.push(NamedWord {
            name: register.name.clone(),
            offset: register.offset,
            value,
        });
    }
    PartialNamedSnapshot { words, error: None }
}

pub fn capture_named<B: Gpu0Bar>(
    bar: &mut B,
    map: &RegisterMap,
) -> Result<NamedSnapshot, MapError> {
    let partial = capture_named_partial(bar, map);
    if let Some(error) = partial.error {
        return Err(error);
    }
    let words = partial.words;
    let canonical_values: Vec<u8> = words
        .iter()
        .flat_map(|word| word.value.to_le_bytes())
        .collect();
    Ok(NamedSnapshot {
        map_sha256: map.sha256,
        values_sha256_le_u32: Digest::of(canonical_values),
        words,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    struct OffsetBar;
    impl Gpu0Bar for OffsetBar {
        fn read32(&mut self, offset: u32) -> Result<u32, String> {
            Ok(offset ^ 0x5a5a_5a5a)
        }
        fn select_fuse_row(&mut self, _: u16) -> Result<(), String> {
            Err("not allowed".into())
        }
        fn issue_cmd_read(&mut self) -> Result<(), String> {
            Err("not allowed".into())
        }
        fn restore_fuse_address(&mut self, _: u32) -> Result<(), String> {
            Err("not allowed".into())
        }
    }

    #[test]
    fn embedded_maps_are_bounded_and_distinct_from_physical_rows() {
        let maps = Ga100Maps::embedded().unwrap();
        assert_eq!(maps.options.registers.len(), 672);
        assert_eq!(maps.statuses.registers.len(), 84);
        assert_eq!(maps.core.registers.len(), 15);
        assert!(
            maps.core
                .registers
                .iter()
                .any(|r| r.name == "FUSEWDATA_PLM")
        );
        assert!(
            maps.options
                .registers
                .iter()
                .any(|r| r.name == "NV_FUSE_OPT_SECURED_BURN_FEATURE_ENABLE")
        );
        assert!(
            maps.statuses
                .registers
                .iter()
                .any(|r| r.name == "STATUS_OPT_FBP")
        );
        let snapshot = capture_named(&mut OffsetBar, &maps.options).unwrap();
        assert_eq!(snapshot.words.len(), 672);
        let alias = maps
            .options
            .registers
            .iter()
            .find(|r| r.name == "NV_FUSE_OPT_SECURED_BURN_FEATURE_ENABLE")
            .unwrap();
        assert_eq!(snapshot.get(&alias.name), Some(alias.offset ^ 0x5a5a_5a5a));
        assert_ne!(snapshot.map_sha256, snapshot.values_sha256_le_u32);
    }
}

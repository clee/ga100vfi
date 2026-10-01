# GA100 published register maps

The JSON files list register names and BAR0 offsets, sorted by name:

| File | Register group | Entries |
| --- | --- | ---: |
| `ga100-options.json` | Published fuse options | 672 |
| `ga100-status.json` | Fuse status | 84 |
| `ga100-core.json` | Controller, ring, PLM, and boot state | 15 |

The capture code checks offset alignment, uniqueness, and BAR bounds before
reading. Published option values and physical `CMD_READ` rows are recorded
separately. Review the maps when adapting the tool to another GPU revision.

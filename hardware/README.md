# Hardware

Hardware assets for the controller: the KiCad board project, PCB production files, and the bill of materials.

## Structure
- `kicad/` - KiCad project sources:
  - `main.kicad_pro` - project file;
  - `main.kicad_sch` - top-level schematic;
  - `main.kicad_pcb` - PCB;
  - additional schematic sheets: `ads`, `esp`, `fan_power`, `fans`, `ina`, `power`.
- `production/gerbers/` - `Gerber` files for board fabrication.
- `production/drill/` - drill files.
- `production/bom/main.csv` - BOM.
- `production/schematic-pdf/main.pdf` - schematic PDF export.

## Production artifacts
[`../scripts/build_hardware.sh`](../scripts/build_hardware.sh) builds the following archive in `dist/`:

```text
pc-water-cooling-controller-<version>-hardware-gerbers-drill.zip
```

It contains:
- all files from `production/gerbers/`;
- all files from `production/drill/`.

The BOM is kept in the repository separately and is not packed by this script, so it should be treated as an independent manufacturing artifact.

## Practical use
If the task is related to board manufacturing or revision work, this directory contains the working inputs:
- source files for schematic and PCB changes;
- ready-to-send fabrication exports;
- the current component list.

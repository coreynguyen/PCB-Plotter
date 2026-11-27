# H8H research (Database -> Research without the editor)

When the house component DB is set (FlexDB page, "House conventions"), every H8H export fills each part's machine
data from the part's own DB rows. The output is byte-for-byte what the HLC editor writes after Database -> Research.
Parts the DB does not have get data from the package library or from built-in rules (below), so an export always
creates every part.

## What gets written

| Block | Record | Source |
|---|---|---|
| 4 | component (1822 B) | Compo, Feeder, Expansion (+ GripNozzle), Mech, Vision, Laser, the machine tables (VisionGx5 / CX / Gx6, LaserGx5 / FX1 / CX / Gx6 / Fx3 / Fx2), Fluxer, Inspection |
| 7 | vision control (1052 B) | VisionData (+ Side, CoplaGX4, BGA), the four recognition sets (GX4Vision, VCSControlGx5 / CX / Gx6) |
| 18 | lights (5240 B) | LightControl*, LightDivAsgn*, LightPattern* (one 1296 B set per recognition set) |
| 19 | fixed record (36 B) | constant (element header for class 501, below) |
| 16 | BGA ball grid (12830 B, one per pattern name) | BGA.Array of custom-pattern BGAs (type 255) |
| 19 | element header (class 501) | Gx4ExtendVision |
| 20 / 21 / 22 | element groups (170 B), elements (112 B), extended arrays (3142 B) | ElementGroup + MissPattern, Element + Shape*, ExtendArray + ElemLoc; linked lists renumbered program-wide |

The rules are in `src/io/juki_research_rules.cpp`. Each one describes, in our own code, how the editor's DB read turns
columns into bytes: the set flags, re-codings (tape feed codes, stick codes, nozzle numbers + 1, vacuum levels below
0x33 not set), NULL handling, and the editor's checks after the read (completeness, vision state, the lead-side
fields each class needs, unused light patterns).

Research does not overwrite the program's record. It merges the DB part into it (`research_merge_b4`, in the writer).
The name, feeder slots, placement counters and long name stay; the outline, class, feeder, nozzle, laser, vision,
speeds and inspection come from the DB.

## Parts that are not in the component DB

Every H8H export gives every part machine data. For each part, in this order:

1. **Component DB.** The part is looked up by its name. A part number longer than 20 characters is matched by
   `ExtCompoName`. Failing that, it is matched by its first 20 characters, but only when that DB part has no
   other long part number. The program then uses the DB part's name.
2. **Package library** (the FlexLine `.flx` set on the FlexDB page). Teaching picks a profile for the part, the same
   way the Components page does (package, description, body, machine history). The profile's rows are researched
   with the part's own values put over them, as applying the profile and then the part's machine data to a
   component DB would:
   - body (the long edge as Width), height and description;
   - carrier and pitch;
   - nozzle;
   - auto-teach for paper tape.

   See `template_rows`.
3. **Built-in rules** (`src/io/juki_autopart.cpp`). These need no database and no library, so the exe alone
   creates the parts. They produce laser-centring data from the package family and body size:
   - The class code, nozzle, packaging, tape width, pitch and supply angle come from the part's machine data
     (Packages > Machine). Without that, from the body.
   - Mechanical centring is added for chips, SOT and SOP.
   - Laser height is a fraction of the body height that depends on the family. Where the part has no height, an
     assumed height is used and noted.
   - Speeds, accelerations and vacuum codes come from four tiers: chip, SOT, IC and large IC.
   - Tray parts get a JEDEC matrix tray.
   - 8 mm tape feeds only 2 or 4 mm pitch, and paper carriers are only used for flat chips.

   Every record passes the editor's completeness check. Vision recognition data is never generated. Bodies over
   33.5 mm are flagged for vision teaching.

The export check lists how many parts came from the library and how many from the rules, with what was assumed.
It says "Check them in the editor before running." The job option `juki.autopart=off` turns the rules off (those
parts then get the plain default record). `juki.research=off` turns all of this off.

## Part names (20 characters)

The H8H part name is 20 characters long. The long-name field (60 characters) always holds the full part number.
`machine_names` in the writer works like this:

- A part number of up to 20 characters is its own name.
- A longer one is named by its first 20 characters when no other part of the program has them.
- On a clash, the name keeps as much of the beginning as fits, then `~`, then the shortest end of the part number
  that differs between all of the clashing part numbers. Where it fits, the end is extended to whole tokens.
  Example: `ABCDEFGHIJKLMNOPQRSTUV-TR` and `ABCDEFGHIJKLMNOPQRSTUV-CT` become `ABCDEFGHIJKLMNOPQ~TR` and
  `ABCDEFGHIJKLMNOPQ~CT`.
- When the ends are the same, the name ends with the segment where the numbers start to differ. Failing that, it
  ends with `~` and a number.
- Names are compared case-insensitively. A name never takes another part's whole name.
- A DB part keeps its DB name when that name is free.

Each clash is a warning in the export check. Plain truncations are listed in one warning.

## Inputs

- the component DB, read only (the house conventions DB; PCB Plotter keeps a copy); optional
- the package library (.flx), read only; optional
- optional: the HLC software folder (default `C:\HLC`). Its `SubCompo.ini`, `SubCompoFx1r.ini` and
  `SubCompoFx2.ini` give the default table accelerations where the DB leaves them at 0. Without them those
  accelerations stay 0.

Researched programs are no longer needed.

## Known difference

An extended array's name field (block 22 +24, 31 bytes) holds, after the name's terminating NUL, whatever text the
editor's heap had there (left-over SQL / table names). PCB Plotter writes zeros. Nothing reads past the NUL.

## Verification

`tests/test_juki_research.cpp` compares against programs the editor researched (private data via
`tests/local_data.map`: `research_db`, `research_truth`, `research_before`, `research_machine`). On the 40-part
RESEARCH_TEST and the four mapping runs (1151 parts, each with one DB column varied, 92 of them element-group
parts with 194 element groups), every part is byte-exact in blocks 4, 7, 16, 18, 19, 20, 21 and 22, apart from the
name tail above.

## Shared with Juki Tool

Juki Tool compiles a copy of four of these files, in `src/shared/pcbp_juki/`. PCB Plotter is the source of truth:
change them here, run `tests/test_juki_research.cpp`, then copy them over unchanged.

- `juki_research_rules` and `juki_record`: the research of one part from its rows, with no database and no files.
  `juki_research` (the FlexLine DB) and the built-in rules both run through `research_with`.
- `juki_builtin`: the built-in rules, from a plain input (class, body, packaging). `juki_autopart` builds that input
  from a project part.
- `juki_resize`: a size edit on a component record, like the editor's. The outline changes, and the nozzle, vacuum
  and laser height follow the new size. The feeder is only suggested.

  Which record bytes count as nozzle, vacuum or laser height is not a hand-made list. It is measured once from the
  rules: every bit that changes when only those columns change.

  Laser fields change only on laser parts, mechanical-centring fields only on mech parts. Vision parts keep their
  nozzle.

The nozzle step tables (body short edge, per package group) were fitted to the house component DB. Resizing every
laser part of the house DB to its own size gives the DB's own GX-4 nozzle for 87 % of the parts and one size off for
94 %. Chips match 92 %, SOT 86 %, SOP 69 %, QFP 87 %, QFN 83 % and connectors 94 %.

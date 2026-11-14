# PCB Plotter — Operator Workflow

Short procedure for turning customer data into a JUKI program. Each stage
has its own section; keep the headings stable (other sections are added by
the feature owners).

Conventions used everywhere: millimetres internally, board viewed from the
top, +X right, +Y up, angles counter-clockwise. Bottom parts are stored in
top-view coordinates with the footprint mirrored.

## 1. Import

1. **File > Import > CAD File…** — the format is detected; ODB++ jobs open the
   *ODB++ Import* step chooser (below). Gerber, centroid (XY) and BOM files
   have their own Import entries.
2. Every import ends with an **Import Report**. Read it: it says which data was
   used, what was guessed (drills, units, rotation convention) and what was
   missing. The report is also stored in the project (sources list) with the
   file path and the settings used, so the import can be reproduced.

### 1.1 ODB++ jobs
* Open the `.tgz` / `.zip` directly — no need to extract. Compressed members
  (`.Z`, `.gz`) and a zip that contains the `.tgz` are handled.
* The step table lists every step with its type guess, size, top / bottom
  component counts, layers and step-repeat. The step marked **(recommended)** is
  preselected. Normally this is the single board (`pcb`).
* A **panel** step has no components of its own. Choose it only to get the
  panel layout: *board + panel images* imports the board once and one panel
  image per step-repeat instance; *every board expanded* makes separate
  components (`R1`, `R1_2`, …).
* *coupon* steps are test coupons — never import them for placement.
* If no step has component layers the job is artwork only: outline and
  copper / silk / mask / paste / drill come in as layers for digitizing, and
  placements must come from a centroid file.

### 1.2 Board images (photo / AOI)
* **Import > Board Image…** loads PNG / JPEG / BMP / TIFF. The image is fitted
  over the board and the *Board Images* dialog opens.
* Register it: click a point in the image (left pane), then the same feature
  on the board (right pane, snaps to pads, fiducials, frame corners), or type
  the board X / Y. 2 points fix scale + rotation; 4 points (e.g. the four board
  corners) also remove phone-camera perspective. The residual (mm) is shown —
  above ~0.1 mm re-pick or add points.
* Use *Opacity*, *Visible*, *Lock* (prevents accidental re-registration) and
  *Side* (shown in top or bottom view). A bottom-side scan viewed from the top
  needs *Mirrored* for a 2-point solve.
* Best source: the AOI image (no perspective, no lens distortion). Phone
  photos: shoot square-on from a distance with zoom, diffuse light.

## 2. Inspect

* **View from Top / Bottom** (T / B): Bottom turns the board over (mirrored)
  so markings read as on the real board; the side you view is the active side.
* **Normal / Ghost / X-ray** (N / G / X): Normal shows only the active side;
  Ghost draws the far side faint and not selectable; X-ray draws both. The
  **lock** (K) decides whether far-side parts can be picked in X-ray.
* **Pin 1** toolbar button cycles *off / oriented / all*: draws each package's
  pin-one marker (dot, arrow or custom) so polarity can be checked against the
  board image. *oriented* = polarized parts only.
* If the window is narrow, tools that do not fit move into the **»** menu at
  the end of the toolbar.

## 3. Correct

### 3.1 Packages and pin one
* Open the package from the part (right-click > Package…) or the Packages
  page. Package **0°** means the part as it sits in the cut tape / pocket.
* Set the pin-one marker type (dot / arrow / custom) and position, and whether
  the part is polarized. Fix a wrong package origin here, not by moving parts.
* Package changes apply to every placement that uses that package.
* **Centroid-only imports** (no pad graphics): every package name gets an entry at once, so the
  package dropdowns are never empty. **Auto-resolve** (Packages page, unresolved package, or the
  Gerber tab's *Package from Gerber* card) fills what it can: a same-name library package, a resolved
  package with an equivalent name (R_0603 = 0603 = RESC1608), or a built-in chip / SOT-23 template;
  the summary lists what is still unresolved. The rest: **Build from Gerber** — box the pads of one
  placement, then *Build & use for all N placements of <package>*; the built pads stay drawn on every
  placement until *Next package*.

### 3.1b Align the placements to the Gerbers
* Gerber tab, **Align Plot to Gerber**: click a placement (any circuit), box its pads on the paste or
  copper layer. Its centroid goes to the centre of the boxed pads (bounding box). Blue ghosts show
  every placement where Apply would put it; pick 0/90/180/270 / Mirror until they sit on the pads,
  or box a second placement far away and choose *From refs*. Apply (Ctrl+Z undoes; the tool stays
  open to repeat). Drag the card's title to move it.
* *Move by Two Points*, *Rotate About Point* and *Two Reference Points* (Board menu) are manual plot
  moves; *Rebase Coordinates to Origin* only re-zeroes the coordinates.

### 3.2 Rotation and group edit
* Only one angle matters: the **Board View angle** (CCW, as drawn). The
  machine angle is calculated from it at export — there is no second copy to
  keep in sync.
* Select the parts (Edit > Select By reference / part number / package, or
  saved selection sets), then **Group Edit / Rotate…**:
  *Add rotation delta* (e.g. +90 for all of one package), *Set absolute
  rotation*, *Reset to imported rotation*, change package, assign process or
  group label. Toolbar buttons rotate the selection by 90 / 180 / 270.
* *Pre-rotation (CW)* is feeder/supply information only; it never changes the
  placement angle.
* Use **Inspect outliers** to find parts of one package whose angle differs
  from the others.

## 4. Validate

The Export page shows a **Validation** box for the current program (or for
all sides when Side 1 / Side 2 are set up). It is recomputed live.

Nothing blocks export. Every Generate (menu, Ctrl+G, Export page, linked
pair, plugin formats) shows one **Generate anyway?** confirmation listing the
errors first, then the warnings; with errors, Cancel is the default button.

* **Error** — the program is probably wrong for the machine: no machine
  selected, duplicate or missing reference, invalid position/angle, parts of
  the other side in this program, side setup broken, **no fiducial marks on
  the programmed side** (or none that the Board / Circuit mode writes).
  Marks are never borrowed from the other side. Set **No marks
  (intentional)** for a layer that really runs without marks.
* **Warning** — review: package not in library, through-hole part in an SMT
  program, centroid outside the circuit frame, more than 3 marks (Fiducial
  1 / 2 / 3 are written, by number).
* **Info** — notes only, e.g. the marks written, the old CircuitCAM machine
  angle differs from the exported one, or pre-rotation is not written for JUKI.

The Export page shows **Marks written**: name, Fiducial number and machine
X / Y of exactly the marks in the program.

Click a row to select that part and zoom to it in the machine view.

## 5. Export

### 5.1 Sides
* Two-sided jobs: set up **Side 1 / Side 2** on the Export page (which
  physical side runs first; one shared or separate export profile per side).
  *Swap order*, *Reset*, *Assign unassigned by side* are next to it.
* **Export all sides** writes `<Job> [Side 1]` and `<Job> [Side 2]` programs,
  a placements CSV per side and a sides report (HTML/TXT). **This point only**
  / **Generate Program** writes the current program alone.
* Validation runs first in both cases: errors and warnings are confirmed once.
* The program is checked in memory before the file is touched, written to a
  temporary file, read back, then replaces the old file (a failure keeps the
  old file). A failed read-back or program check is a failed export (red
  status, no success message).

### 5.2 Transform chain
* Select a placement and press **Transform chain** (Validation box) or
  right-click it in the machine view. It lists every step from board X/Y/angle
  through origin, orientation, flip and angle convention to the machine
  X/Y/angle — the same numbers written to the program.
* The **Machine angle preview** table lists board angle, pre-rotation, machine
  angle and the legacy CircuitCAM angle for every part of the program.

## 6. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Export button does nothing but show a message | Validation error — fix the listed rows first. |
| Machine angle differs from the old CircuitCAM program | CircuitCAM kept a separate stored angle. Check the part against the board image; correct the Board View angle, not the program. |
| A part is placed rotated on the machine | Check the package 0° (tape orientation) and pin-one marker, then the transform chain of that part. |

### 6.1 Imports
| Symptom | Cause / fix |
|---|---|
| ODB++: "No placements imported from step 'panel'… Step 'pcb' has N components" | Wrong step. Re-import and pick the recommended step. |
| ODB++: "no component layer files" | The job carries no placement data (artwork only). Get a centroid / XY file from the customer or digitize from the layers. |
| ODB++: "could not decompress …" | Corrupt or truncated archive — ask for a fresh export. |
| ODB++: "rotation convention: toeprints fit counter-clockwise angles…" | The exporter wrote CCW angles; PCB Plotter used them as CCW. Spot-check a polarized part. |
| ODB++: "properties without a matching part field" | Listed attributes (e.g. assembly notes) were not stored; use the BOM import for them. |
| "N through-hole pins without drill data: drill estimated" | Drill layer missing; drills are a guess (60 % of pad). Fine for placement, not for insertion checks. |
| Board image looks skewed / bent | Add 4 well-spread points (perspective). A curved distortion that remains is lens distortion — use the AOI image instead. |
| CircuitCAM imported nothing from the same job | Known CircuitCAM limits: needs an extracted folder, ignores `.Z` members and step-repeat, needs `# CMP` comments, ignores `UNITS=MM`. |

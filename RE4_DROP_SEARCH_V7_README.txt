RE4 JPN GENERIC DROP SEARCH v7
================================

WHAT CHANGED
------------
v7 is one configurable search engine instead of a patch tied to one drop scene. It supports
1 through 10 simultaneous drops, order-independent or per-slot targets, calibrated controller
mutations, separate grenade/money/target corpora, adaptive-difficulty logging, and automatic
generation of the winning DTM.

CURRENT BUNDLED PROFILE
-----------------------
The included RE4DropSearch.ini is already configured for the supplied village movie:

  - neutral DTM: re4-wr-tas1.dtm (8089 frames / 16173 inputs)
  - snapshot: frame 7823
  - manipulation starts in the 7825/7826 TAS slot
  - two simultaneous drops
  - target: Hand Grenade + 9900 ptas, either order
  - movement cost limit: 4 emulated frames
  - budget: 200000 attempts

USAGE
-----
1. Extract the entire archive to a new folder. Keep RE4DropSearch.ini beside Dolphin.exe.
2. Open Dolphin.exe and play re4-wr-tas1.dtm read-only using the same RE4 JPN setup.
3. The HUD first reports validation and a short calibration, then phase 2 adaptive search.
4. A success pauses emulation and writes these files under User/Logs:

     re4_jpn_village_dual_v7_FOUND.dtm
     re4_jpn_village_dual_v7_FOUND.txt
     re4_jpn_village_dual_v7_summary.txt
     re4_jpn_village_dual_v7_results.csv

The FOUND.dtm is a copy of the movie you loaded with the exact winning controller polls patched
in. It includes subframe inputs automatically, so no manual analog editing is required.

MAKING A NEW DROP PROFILE
-------------------------
Edit RE4DropSearch.ini before starting the next movie. No recompilation is needed.

Search:
  ExpectedMovieFrames / ExpectedMovieInputs protect against loading the wrong DTM. Set either
  to 0 only when intentionally disabling that guard. ExpectedDrops accepts 1..10; 0 records up
  to 10 events and normally waits until HardEndFrame unless the target has already been found.

Window:
  CaptureFrame must be before every mutable input. FirstSlot and LastSlot define the candidate
  input range. HardEndFrame closes attempts that do not complete normally. MovementFirstFrame,
  MovementLastFrame, and MaxMovementCostFrames protect the optimized walking section.

Target modes:
  EXACT_ANY_ORDER     all completed drops must be exactly the listed multiset
  CONTAINS_ANY_ORDER  the listed targets may appear among additional drops
  PER_SLOT            Target 1 matches drop 1, Target 2 matches drop 2, etc.

Target syntax:
  HAND                alias from [Catalog]
  PTAS:9900           exact money amount (must be divisible by 10)
  ANY_PTAS            any money drop
  GRENADE:2           raw final grenade ID
  ANY_GRENADE         any grenade route result
  ITEM:0x78:990       raw late result: item ID and game value
  ANY                 wildcard, mainly useful with PER_SLOT

Multiple goals use a comma-separated list, for example:

  Targets = HAND, PTAS:9900
  Targets = HAND, INCENDIARY, PTAS:3300
  Targets = ANY, FLASH

Add INCENDIARY and FLASH to [Catalog] only after their final IDs have been confirmed. Using a raw
GRENADE:<id> target avoids depending on an alias.

Mutations:
  AllowedKinds controls what the calibration tests. ForcedKinds keeps combo-only controls active
  even if they do not change the result alone. For a new scene, start with every kind enabled:

  AllowedKinds = B_OFF, L, R, SX, SY, SY_CENTER, CX, CY
  ForcedKinds = B_OFF, L, R, SY, SY_CENTER

FullCalibration=True tests every two-frame slot and is thorough but slower. The default short
calibration spreads CalibrationAnchors probes over the window and includes every Hotspot.

PARALLEL WORKERS
----------------
Copies of the same portable folder can search independent streams. Set WorkerCount to the number
of copies and give each one a different WorkerId from 0 to WorkerCount-1. Their output filenames
receive _w0, _w1, and so on. Do not run two workers from the same User folder.

NOTES
-----
- The game-specific drop probe addresses are for RE4 JPN GameCube (G4BJ08).
- Changing target items, number of drops, movie guard, window, or mutation controls needs only
  an INI edit. Porting the engine to another regional executable still requires new probe PCs.
- The CSV is written with a fixed classic locale, so large values no longer gain thousands
  separators that break columns.

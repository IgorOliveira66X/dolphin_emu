RE4 JPN TAS HUD v1
==================

Target
------
Game ID: G4BJ08 (Biohazard 4 JPN)
Leon player pointer: 0x80281760
Position: player pointer + 0x94 (X/Y/Z)
Yaw: player pointer + 0xA4
Dynamic difficulty points: 0x80287FA0
Dynamic difficulty rank: 0x80287FA4

HUD fields
----------
F / I       Dolphin movie frame and input count.
DA Rank     Current adaptive-difficulty rank.
Pontos      Exact adaptive-difficulty point value (shows progress between ranks).
POS         Leon's absolute X/Y/Z coordinates.
dPoll       Coordinate change since the previous controller poll.
Vel H / VI  Horizontal distance moved per Dolphin VI frame.
Vel H / poll
            Horizontal distance moved per game controller poll.
Dir         Direction of the last horizontal displacement, in degrees.
Leon Yaw    Raw Y-axis rotation stored by the game.

The HUD is read-only. It does not modify emulated memory, controller input,
movie data, RNG state, or savestates. Rewinding the movie automatically
starts a fresh delta sample so a backward jump is not reported as movement.

If the player structure is unavailable (for example, before gameplay begins),
the HUD displays "Aguardando Leon" and the current pointer value.

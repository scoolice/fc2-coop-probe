# fc2-coop-probe

A research tool for **Far Cry 2** (Steam, version 1.03, Dunia engine): a `dinput8.dll` proxy
that you drop into a **test copy** of the game. Inside the running game it

- logs Winsock, file-open and exit calls of `Dunia.dll`,
- runs Lua 4.0 code on the game's main thread and gives the result back,
- runs native commands (read an entity, find the player's physics body, teleport it, scan
  memory),
- and, as a prototype, exchanges the player's position between two copies over its own UDP
  socket and shows the peer either as an invincible puppet avatar or as an **AI body**: a
  spawned buddy that walks and runs after the peer's position with the engine's own
  locomotion and animation (see [AI body](#ai-body-an-animated-peer)).

It is **not** a playable co-op mod: there are no weapons, health or mission sync, and the AI body
trails the peer by a few metres. It is the code behind the Far Cry 2 write-up in
[coop-anything](https://github.com/cranebeaker/coop-anything/tree/main/games/far-cry-2)
(pull request: https://github.com/cranebeaker/coop-anything/pull/43); the findings, dates and
evidence are there.

## Read this first

- Single-player campaign only. Do not use it online; PunkBuster is not touched and nothing
  here is meant to be used in other people's multiplayer games.
- **Use a copy of the game folder, never your real install.** Back up before you change
  anything. The proxy is loaded only if `dinput8.dll` sits next to `FarCry2.exe`.
- Every address (RVA) in `src/probe.c` is for this `Dunia.dll` only:
  SHA-256 `6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765`,
  20,183,176 bytes (Steam build id 52164). Other builds need new addresses.
- The game files are not included and not needed here.

## Build

32-bit MinGW (gcc 9.2 and 15.2 were used):

```
gcc -O1 -shared -o dinput8.dll src/probe.c src/probe.def -lws2_32 -static-libgcc
```

or run `build.bat`. Copy `dinput8.dll` into the `bin` folder of the test copy (next to
`FarCry2.exe`). On start it writes `coop_probe.log` in that folder.

## Use

The game is controlled through two files that you create in the same `bin` folder. The
probe polls them from the game's main loop about four times a second, runs them and
**deletes** them.

### `coop_cmd.lua`: Lua code

The code runs in the game's main Lua state. Put the answer into the global `__out` (a string);
it is logged as `cmd executed (N bytes) status=S out(L)=...`. `print` and `_ALERT` show
nothing in this build. Examples:

```lua
__out = "alive " .. (1 + 2)
__out = GetLocalPlayerId()
__out = tostring(IsEntityValid(GetLocalPlayerId()))
s = "" foreach(globals(), function(k, v) s = s .. k .. " " end) __out = s
```

Lua is 4.0: use `foreach`, `getn`, `..`; there is no `#`. Status 0 is success, 1 a run error,
3 a syntax error. Entity ids are 64-bit and come as decimal strings.

### `coop_native.txt`: native commands, one per line

| Command | What it does |
|---|---|
| `pos <id>` | Log position and direction of the entity (matrix at `entity+0x30`) |
| `setpos <id> <x> <y> <z>` | Write the entity matrix position once (does not stick for a pawn, its body overrides it; sticks for an object such as a weapon lying on the ground) |
| `ent <id> <len>` | Log the entity object address, vtable and `len` bytes as hex, with a float column |
| `peek <hexaddr> <len>` | Hex dump of memory (float column) |
| `poke <hexaddr> <hex32>` / `pokef <hexaddr> <float>` | Write a 32-bit value / a float |
| `scan <x> <y>` | Scan memory for the float pair (tolerance 0.01; y within 16 bytes after x) |
| `findbody <id>` | Find the Havok body (class vtable RVA `0xE67658`) nearest to the entity and cache it |
| `posb` / `tpb <x> <y> <z>` | Read / write the cached body's position (teleport) |
| `walk <radius> <seconds> <speed>` | Move the cached (local) body on a circle every frame (needs `netstart` running) |
| `walklegs <speed> <pause> <dx1> <dy1> [...]` | Move the cached (local) body along up to 8 straight legs relative to its start, pausing `pause` s after each (needs `netstart` running) |
| `hold <id> <x> <y> <z>` / `unhold` | Write the entity matrix position every tick (does not stick; kept as the negative test) |
| `follow <id> <hex offsets>` / `tp <id> <x> <y> <z> <hex offsets>` | Follow a pointer chain from the entity; write the position at its end |
| `bodyvel` / `avvel <x> <y> <z> <hexoffset>` | Write three floats into the cached / avatar body at an offset |
| `netstart <localport> <peerip> <peerport> <id>` | Open the UDP transport and start the avatar logic (peer `127.x`: loopback only; any other address: listens on all interfaces, accepts packets from that address only) |
| `netrate <hz>` | Packets a second (5-120, default 30; was a fixed 20 Hz that GetTickCount made about 16) |
| `netstop` | Stop it and remove the avatar |
| `bodies` | Log every body of the class above with its position (AI pawns do not use this class) |
| `ents <id> <radius>` | Log every object of the same class as the entity's object within `radius` m, with its entity id (finds nearby pawns; `GetEntityName(id)` names them) |
| `aibody <archetype> remote\|local` | Spawn an AI pawn of `archetype` (for example `buddies.Buddies.Frank_Bilders_Unarmed`) at the target once there is one, make it invincible and follow the peer or the local player; the pawn and its marker are spawned again whenever the engine reports either as gone (`IsEntityValid`, checked once a second: a death or checkpoint reload removes both) or the pawn ignores five orders in a row |
| `aifollow <pawn> remote\|local\|circle [<r> <v>]` | Make an AI pawn follow the peer (`remote`), the local player (`local`, needs `findbody` first) or a test circle of radius `r` at `v` m/s (see [AI body](#ai-body-an-animated-peer)); with `remote` the puppet avatar is not spawned |
| `aistall <ms> <m>` / `ailead <s>` / `aispeed <n>` / `aitp <m>` | Follower tuning: re-order a pawn that stood `ms` while more than `m` away (default 6000 / 3); lead the marker by `s` s of the target's velocity (default 0); MoveTo speed value (default 3); teleport distance (default 10) |
| `aiglide <on> <off>` | Above `on` m/s of target speed (a vehicle; default 8) the pawn stops walking and is teleported onto the marker every frame, the marker extrapolated from the last packet by up to 0.25 s; below `off` m/s (default 5) it walks again. `aiglide 0 0` turns it off. A two-copy run with A moving at 10 m/s: glide on 0.5 s after the start, buddy 0.00-0.31 m from A, walking again 0.3 s after the stop |
| `airescue <seconds>` | The longest the buddy stays away while the game's rescue buddy revives the local player (default 180; 0 = ignore rescues). See "Buddy rescue" |
| `vinfo <id>` / `vphys <id> 0\|1` / `vset <id> <x> <y> <z> <yaw>` | Log an entity's physics components; switch its physics off or on; place it with `CEntity::SetEuler` / `SetPosition` (yaw in radians). The way FC2JackalFix's noclip moves the player, and safe on vehicles, where `TeleportEntity` crashes (Dunia+0x9fcf02) |
| `winfo <pawn>` / `wadd <pawn> <weapon>` / `wsel <pawn> primary\|secondary\|hand_hand\|special` / `wdraw <pawn>` / `wholster <pawn>` | Weapons on any pawn: log its weapon manager (mode, state, weapon in hand); give it a spawned weapon entity; select a category (also draws); draw; holster |
| `sig <pawn> <name\|0xhash>` | Send a signal (crc32 of its name) to the pawn's state driver, as the player's input does (`config/inputactionmapcommon.xml`: `startshooting`, `reload`, `jump`, ...). On an AI pawn `startshooting` does nothing |
| `wbullet <pawn> [entity]` | Fire one bullet from the gun in the pawn's hand: a `shootbullet` animation event handed to its bullet fire strategy. The bullet follows the pawn's aim |
| `wfire <pawn> <dx> <dy> <dz>` | Like `wbullet`, along the given direction (test: the buddy's bullets do damage enemies) |
| `ctl <pawn> crouch\|sprint 0\|1\|-1` / `ctl <pawn> jump [updates]` / `ctl jumpfor <updates>` / `ctl <pawn> move <vx> <vy>\|off` / `ctl <pawn> hold <off> <float>\|off` / `ctl <pawn> blk` / `ctl 0` | Test: write the player's movement fields into one pawn's control block before and after each of its updates: crouch (+0xa4), sprint (+4 bit 0x40), jump (+4 bit 0x20 for a few updates; `ctl jumpfor` sets how many, default 6, also for the body's synced jumps), desired movement (+0xb0/+0xb4, world m/s, what the player's movement code writes). -1 leaves the field to the game. `hold` keeps any float of the block (hex offset, up to 8); `blk` logs both blocks' floats +0x30..+0xbc |
| `wshoot <pawn> [8\|10\|11]` / `wevent <pawn> <name>` | Test, any weapon type: call the held fire strategy's Fire (slot 8, default), StartShoot (slot 10) or StopShoot (slot 11); hand it an animation event such as `attach_projectile`. Both log the strategy's vtable and, for a projectile weapon, its loaded round |
| `wmute <pawn\|0>` | Test: that pawn's gun does not fire on its own: its weapon manager's fire-one-shot step (Dunia+0x1305f0) and start-shooting step are skipped and any own bullet is dropped (only `wbullet`/`wfire` and the peer's synced shots go out). Logs its AI state changes and the call path of its first 3 own bullets. 0 turns it off |
| `wmutebit <n>` | Test: never set bit `n` of the muted pawn's AI state mask (-1 = none). Bit 0x32 follows its bursts but holding it does not stop the firing |
| `coords 1\|0` | A small always-on-top strip over the game window with the local, peer and body positions and the body's distance from the peer |
| `waimto <pawn> <marker> <x y z>` / `wstop` | Test, on a `wmute`'d pawn: aim it at a point without firing, by moving the marker, sending `ShootAtTarget` and ending the shoot task after its first aim step (about 30 ms); it keeps the pose. `wstop` ends the running shoot task the same way |
| `wlower 1\|0` | Test: lower the `wmute`'d pawn's gun after `waimto`, by running its pawn attribute task with AimAt and LookAt off each frame. `waimto` raises it again |
| `wbit <n> <0\|1>` | Test: set or clear bit `n` of the muted pawn's AI state mask once (needs `wmute` and a running ShootAtTarget). Bit names: 0x01 DominoActive, 0x32 IsShooting, 0x3d ShootAtTarget |
| `waim <pawn> <x> <y> <z> [or4 or6 or7]` / `waim 0` | Test: after each update of the pawn, set aim flags in its control block (pawn data + 0x140; default +4\|03 AimAt+LookAt, +6\|04, +7\|01) and the aim point. The pawn then points its gun at its feet: these fields alone do not give an aim pose |
| `waimat <pawn> <entity>` | Like `waim`, with the entity as the aim target (block +0x18c, +6\|84). Same result: gun at its feet |
| `hwbp <hex address\|0>` / `hwlog` | Hardware write breakpoint (debug register 0, one byte) on every game thread; `hwlog` lists the instructions that wrote it and how often. `hwbp 0` clears it and keeps the counts |
| `getc <id> <rva>` | Call a `GetComponent<T>` getter (thiscall on the entity, RVA in hex) and log the component and its vtable RVA. `90930` is `CPawnNetworkComponent`: null on the campaign player, a spawned buddy and a vehicle, so the multiplayer third-party path cannot be switched on for campaign pawns as they are; `649f0` is the vehicle physics component |
| `aistop` | Stop the follower and remove its marker |

Helpers in `scripts/`: `fc2-session.ps1` starts one test copy, loads its newest save and leaves it
running; `fc2-lua.ps1` runs Lua in it and prints `__out` (a Lua error comes back as `LUAERROR <message>`);
`fc2-native.ps1` sends native commands and prints the log lines they wrote.

## Two copies on one PC

Each test copy needs three same-length byte patches in its own `Dunia.dll` (the copy of the
file in that test folder, never the original; keep a backup). Offsets for the build above:

1. **Mutex name.** The string `FarCry2Instance` (file offset `0xE08950`): change one
   character (for example `FarCry2InstancA` / `FarCry2InstancB`) so both copies can run.
2. **Profile folder.** The 10-byte string `\Far Cry 2` (file offset `0xE0E094`, next to
   `\GamerProfile.xml` and `\My Games`): replace it with another 10-byte string (for example
   `\FC2test_A` / `\FC2test_B`). The profile and the save that is opened then come from
   `Documents\My Games\FC2test_A\`. Give each copy that folder with a copy of your
   `GamerProfile.xml` (set `Fullscreen="0"` and a small resolution) and, to load a save, a
   `Saved Games` folder with a copy of one `.sav`. Without a profile file the client quits.
3. **Save list folder.** The 19-byte string `My Games\Far Cry 2\` (file offset `0xE089A0`):
   replace it with the same folder, e.g. `My Games\FC2test_A\`. The list of saves behind
   Continue / Load Game is built from this folder. Without this patch a copy lists the saves in
   your real profile folder, and on a PC where the game never ran it lists none (only
   New Story). This patch alone does not move the profile: patch 2 does that (check the
   `CreateFileW ... GamerProfile.xml` and `... .sav` lines in `coop_probe.log`).

Expect 17 changed bytes in total compared with the original `Dunia.dll`.

Then start each copy by hand, press Continue, and wait until you are in the world.

Then, in each `bin` folder, create `coop_native.txt` (the entity id of the main character
is the same in both copies when they load the same save; get it with `GetLocalPlayerId()`):

```
netstart 9101 127.0.0.1 9102 <id>     (copy A)
netstart 9102 127.0.0.1 9101 <id>     (copy B)
```

The log shows `net: tx=... rx=... drop=...` once a second. After the first packet each copy
spawns an avatar of the peer (`SpawnEntityFromArchetype` with the local player's own name),
makes it invincible (`CFCXCountersComponent_SetInvincible`) and drives its body.

### Two PCs

Give each copy the other PC's address instead of `127.0.0.1` (for example
`netstart 9101 <IP of PC B> 9102 <id>` on A and `netstart 9102 <IP of PC A> 9101 <id>` on B).
With a non-loopback peer the probe binds its port on all interfaces and counts only packets
of the right size and magic from the peer's IP (the source port is not checked, NAT may change
it); everything else increments `drop`. The PCs must reach each other: same LAN, a virtual LAN
(VPN), or UDP port forwarding. There is no NAT traversal, relay or encryption, so the port is
open to anyone who can reach it (only the peer's packets are used). Tested so far only with both
copies on one PC, sending to that PC's own LAN address.

### Through Steam, without IP addresses: `relay/fc2_relay.c` / `relay/fc2_relay.py`

A relay next to each game copy carries the probe's packets, so friends don't need IPs or port
forwarding. The probe talks only to its local relay: `netstart <game port> 127.0.0.1 <relay port> <id>`.
`relay/fc2_relay.c` builds a 64-bit `fc2_relay.exe` with the same flags, frames and log lines, so testers need no
Python (MSYS2 mingw64: `gcc -O2 -Wall -o fc2_relay.exe fc2_relay.c -lws2_32 -lwinmm -static`; it imports only Windows
DLLs and loads `steam_api64.dll` at run time). `fc2_relay.exe --check --steam-dll <dll>` loads the DLL, finds every
function and says whether Steam runs, without logging in. `start-coop.ps1` uses the exe when it is next to it (kit)
or in `relay\`, else `fc2_relay.py` (Python 3, 64-bit). The Steam transport needs the Steam client running and `steam_api64.dll`
from the Steamworks.NET Standalone release 2025.164.1 (`Windows-x64/steam_api64.dll`,
SHA-256 `eb17909a76668cf9ae0b92a618a34a50f6c73d3a6787cb4dd8ce36a8b10bfb75`; not included here).
It runs as Steam's free "Spacewar" app (AppID 480), so friends see you as "playing Spacewar".

```
host:   python relay\fc2_relay.py --transport steam --host --steam-dll <steam_api64.dll> --game-port 9101 --relay-port 9201 [--invite "<friend's Steam name>"]
friend: python relay\fc2_relay.py --transport steam --join friend --steam-dll <steam_api64.dll> --game-port 9102 --relay-port 9202
   or:  python relay\fc2_relay.py --transport steam --join wait   --steam-dll <steam_api64.dll> --game-port 9102 --relay-port 9202
```

The host creates a friends-only lobby (tagged `fc2coop=1`), sets its Steam rich presence `connect` to
`+connect_lobby <id>`, and logs the id; the friend joins by id (`--join <id>`), with `--join friend`, which
finds a friend hosting one, or with `--join wait`, which joins when the friend accepts the host's Steam
invite (`--invite`) or clicks Join Game on the host in the Steam friends list (Steam callbacks 333 and 337;
start the relay before accepting, otherwise Steam starts the real Spacewar instead). Sessions are accepted only
from lobby members; probe packets go unreliable on channel 0, a keep-alive and goodbye go reliable on
channel 1, and a peer silent for 20 s is dropped. `--transport udp --udp-local <port> --udp-peer <port>`
is a stand-in that relays over plain UDP, used by the test to run the whole chain on one PC.

### Two PCs over Steam: `scripts/`

- `scripts/setup-copy.ps1 -Letter A|B -PrivateDir <folder> [-Probe <dinput8.dll>]`: finds the game, checks
  the Dunia.dll hash, copies the install to `<drive>\fc2-coop\test-<letter>`, backs up and applies the three
  patches above (checks 17 changed bytes), installs the probe, and creates `Documents\My Games\FC2test_<letter>`
  from `-PrivateDir` (one `GamerProfile.xml` and the one `.sav` both PCs load). Installs FC2JackalFix and DXVK
  too when the folders below exist.
  Run again on an existing copy: only the probe is put in again, `GamerProfile.xml` is kept, and a save the game has
  changed is moved to `save-backups` and replaced by the shared one.
- `Setup.bat` / `scripts/setup.ps1 [-Role join|host] [-TestDir <copy>] [-NoDxvk] [-NoJackalFix]`: the tester's one step
  (in the kit, next to the other scripts and a `private` folder): checks for 64-bit Python (not when the kit has
  `fc2_relay.exe`; then it runs its `--check` instead), downloads what is missing,
  runs `setup-copy.ps1` (the copy from the settings file, else the default) and saves the copy, Python command and role in
  `%LOCALAPPDATA%\fc2-coop\settings.json`.
- `Play.bat` / `scripts/play.ps1 [-Join wait|friend]`: checks that the copy has the kit's probe (SHA256SUMS.txt) and runs
  `start-coop.ps1` with those settings, `-AiBody -TestSettings`.
- `scripts/get-jackalfix.ps1`: downloads FC2JackalFix V1 (borderless window etc.) into `scripts\jackalfix`, hashes checked.
  With it, its ASI loader is `bin\dinput8.dll` and the probe goes in as `bin\coop_probe.asi`.
- `scripts/get-dxvk.ps1`: downloads DXVK 3.1.1 into `scripts\dxvk`, hashes checked, and keeps the 32-bit
  `d3d10core.dll`, `d3d11.dll`, `dxgi.dll` (the game's D3D10 renderer) and `d3d9.dll` (its D3D9 renderer). Delete
  the four from `bin` to go back to the system's Direct3D.
- `scripts/get-steam-api.ps1`: downloads Steamworks.NET Standalone 2025.164.1 and checks the zip and
  `steam_api64.dll` hashes.
- `scripts/start-coop.ps1 -Role host|join -TestDir <copy> -SteamDll <steam_api64.dll> [-Python <python.exe>]
  [-Invite "<friend's Steam name>"] [-Join friend|wait] [-AiBody [-Buddy <archetype>]] [-TestSettings]`:
  checks the copy's patches and that its profile folder has a save (so Enter can never start a New Story),
  starts the copy (window title `A` / `B`, kept for the run), presses Enter until the save opens, waits for the
  player id, starts the relay (host: lobby; join: finds the friend's lobby, retrying for up to 10 min), runs
  `netstart`, prints the link state every 5 s, and on the join side samples the avatar's and the own player's
  position every 2 s. Kill switch on the real profile path; logs in `<copy>\coop-runs\<time>-<role>\`.
  With `-AiBody` the probe gets `aibody <-Buddy> remote` before `netstart` (the other player appears as an
  invincible buddy, spawned again after a death or reload; the 5 s status line adds its distance,
  teleports and spawns); `-TestSettings` switches on `ai_IgnorePlayer` and `cheat_GodMode`.

Tested:
- the whole chain through two udp relays with both copies on one PC (the two-copy test with `-Relay`);
- **two PCs on different networks, two Steam accounts that are friends (Steam 1.03 on both)**, 2026-10-25:
  the joining relay found the host's lobby through the friends list, the session was accepted, and the first
  message was decoded with the sender identity at offset 16 (x64). Probe packets went both ways at about 16/s
  for ~7 minutes with 0 dropped on both sides and no disconnect. `walk 3 20 2` on the host: on the joining PC the
  avatar (sampled every 2 s) went round about two circles of radius ~3 m centred on the expected point
  (2 m/s x 20 s = 40 m), while that PC's own player stayed within ~0.1 m. The owner saw the avatar move on both
  screens. When the joining PC stopped, the host marked the peer down after the 20 s timeout.
  Heading was not exercised in that run (`walk` does not turn the player);
- **heading across the two PCs**, 2026-10-25: the host's player turned with the mouse 171.3 → 81.3 → -171.3 →
  77.6 degrees; the joining PC's avatar (sampled every 2 s) showed the same four headings, 1-2 s later (the
  sample step), while staying in place;
- **Steam invite**, 2026-10-25: host `--invite <name>`, joining relay `--join wait`; when the invite was accepted
  on the second PC, the relay got callback 333, entered the lobby, and the link and avatar came up within 3 s;
- **Join Game** from the Steam friends list, 2026-10-25: host without an invite, joining relay `--join wait`;
  Join Game on the host in the second PC's friends list joined the host's new lobby, also through callback
  **333** (the host is in a joinable lobby; 337 with only a connect string was not observed, but is handled);
- in all these runs: 0 dropped, no crash lines, neither probe log named a real profile folder; when one side
  stopped, the other marked the peer down after the 20 s timeout.
- Not tested yet: Join Game or an invite while the relay is not running (Steam then starts the real Spacewar),
  longer sessions, packet loss.

## AI body: an animated peer

The puppet avatar is a script-spawned copy of the player pawn; it has none of the controller-side
locomotion data, so it can only slide. An AI pawn has its own: the engine's Domino `MoveTo` command
(`domino/system/moveto.lua` in `common.dat`:
`CDominoManager_GetInstance():SendCommandEventToEntity(pawn, "MoveTo", destinationEntity, speed, useCombat)`)
makes it walk or run to an entity with normal animation. The follower uses that:

- The pawn is a **spawned buddy**: `SpawnEntityFromArchetype('buddies.Buddies.<Name>_Unarmed', x, y, z, 0, 0, 0)`
  (12 names in `engine/gamemodes/gamemodesconfig.xml`, for example `Frank_Bilders`). Soldiers placed
  in the level also obey `MoveTo` but walk back to their post; briefing characters at a faction HQ
  ignore it (`MoveTo` runs an AI plan named in the pawn's own data, `MoveToPlanName`).
- The destination is a **pistol on the ground** (`weapons.Secondary.Makarov`), whose position the
  probe writes every tick. A spawned copy of the player as the destination is not neutral.
- `MoveTo` ends on arrival, so a pawn that stood for `aistall` ms while the target is further away
  gets a new order; a pawn more than `aitp` m behind is moved onto the marker with
  `TeleportEntity(pawn, marker)` and ordered again. Spawned buddies start within about 1 s of an
  order; level soldiers need 2-4 s, and re-ordering them sooner keeps them standing.
- `aibody` spawns the buddy itself, sends it `CFCXCountersComponent_SetInvincible` (the event the puppet
  uses) and spawns buddy and marker again when they are gone. A removed entity can still be found by id
  for a while with a readable object, so liveness is asked from the engine (`IsEntityValid`), not read
  from memory.
- Registering the `MoveToStarted/Finished/Failed` callbacks (`CScriptCallbackSystem`) from a plain
  Lua table crashed the game (null read in the Lua VM); the follower does not use them.

`scripts\fc2-coop-ai.ps1 -CopyA <test copy A> -CopyB <test copy B> [-Probe dinput8.dll]` starts a
two-copy session with an AI body in each copy (each copy's buddy follows the other copy's player) and
switches on the engine's `ai_IgnorePlayer` and `cheat_GodMode` (assigning these Lua globals goes
through the engine's setglobal hook into its console variables; `-NoTestSettings` leaves them off).

Measured with a spawned buddy (Steam 1.03, one PC, 2026-10-28):

```
real player in copy A, 3.4 min, 58 m path, top 3.8 m/s, B's buddy following (aistall 1000 1.5, ailead 0.7):
  distance mean 1.57 m, max 8.51 m, within 3 m 90 % of the time; 16 orders, 0 teleports, no crash lines
scripted circle r 4 m at 1.2 m/s, 25 s:  aistall 6000 3: mean ~6 m;  aistall 1000 1.5: mean 3.36 m, max 7.04 m
scripted circle r 6 m at 3.5 m/s, 25 s:  aistall 1000 1.5: mean 5.62-6.47 m, 3-4 teleports
ailead 0 / 0.7 / 1.2 on the circles: no measurable difference; aispeed 1 is slower, 5 no faster than 3
```

The person playing saw the buddy walk and run with normal animation. It faces where it walks, not
where the peer looks.

### Vehicles

Each packet also carries the vehicle the sender sits in (`CVehicle_GetCurrentVehicle`, asked once a
second), its position and its forward vector. Both copies load the same save, so the receiver has
that vehicle under the same id. When the peer gets in, the receiver switches physics off on its own
copy, seats the buddy with `SetupVehicleFollowPath` once `IsEntityLoaded` reports the pawn loaded,
and places the vehicle from the packets every frame (`SetEuler` / `SetPosition`, extrapolated by up
to 0.25 s). When the peer gets out, physics goes back on and the buddy is spawned again next to the
peer. A vehicle the receiver does not have falls back to `aiglide`; one the receiver's own player sits
in is left alone. Old builds drop the new packets (magic `FC22`), so both sides need this one.

Two copies, the player of A driving the save's Datsun for 25 s: the buddy in B got into the driver's
seat with its animation 1.0 s after A got in, and B's Datsun stayed a median 0.22 m (max 0.39 m while
driving) from A's.

**Driven by the peer's input (packet `FC2B`, `vdrive 1`, default).** Placed with its physics off, the car floated
and passed through things (owner). Now the receiver's car keeps its physics and Frank drives it with the peer's
controls:

- The game's vehicle input step (Dunia+0x4ac570, vtable entry +0xe67338, on the physics thread) reads the driver's
  input record (throttle, handbrake, reverse, steering) and writes Havok's `hkpVehicleDriverInputAnalogStatus` before
  each vehicle step. The probe hooks it: the sender keeps its own car's record; the receiver puts the peer's record
  into the controller of its copy just before the step (and the controller's back after).
- The Havok vehicle instance's chassis rigid body (instance + 0x18) holds its motion at +0xd0 (position +0x40,
  forward column +0x20, linear velocity +0xd0, angular +0xe0). The sender sends its car's velocity and yaw rate; in
  the same hook the receiver moves its car's velocity and yaw rate 15% of the way per step toward the peer's, plus
  1.5/s of the position error (at most 5 m/s) and 2/s of the heading error.
- Getting in, or more than 6 m off, the car is placed at the peer's (physics off, place, on, woken, given the
  peer's velocity through the physics component's set-velocity slot).

Two copies, the owner driving three times (slow, fast, turns, getting in and out): "that works great"; B's car
stayed a median 0.16 m from A's (p90 0.74, max 0.84 m, 1 s samples), snapped only on getting in. `vdrive 0` brings
back the placement. Test commands: `vbody`, `vpc <car> get|wake|set x y z`, `vin log|off|<instance> <throttle>
<steer> <handbrake> [reverse]`, `findvt <vtable rva>` (every object with that vtable).

**Seats (packet `FC2C`).** Each packet carries the sender's seat in its vehicle and the seat's type. A vehicle's
seats are found as `BindToVehicleRide` finds them: its `CAIComponent` (tag Dunia+0xfe4090) holds a seat manager
at +0x14, and the seats are `{ pointer, count }` at `*(*(manager + 0x210) + 0x10) + 0xc`, 0x50 bytes each (+0x10
type: 1 driver, 2 gunner, 3 passenger; +0x28 the occupant's entity id). Rover with a mounted gun: seat 0 driver,
1 passenger, 2 the gun; Datsun: 0 standing in the back, 1 driver. The peer driving: the body is seated with
`SetupVehicleFollowPath` and the car is driven with the peer's input as above. Any other seat: the body is put there
with `BindToVehicleRide(vehicle, body, seat)`, and the car moves the way the receiving world drives it (riding in
the local player's car is allowed now, unless both drive). A seat change seats the body again.

A vehicle spawned during play (patrols, test spawns) has another id in each world. The receiver then takes the
nearest entity with seats within 8 m of the peer's vehicle: a thread reads all memory for entities there (vtable
Dunia+0xe6b464, id at +8, position +0x60; about 1.5 s, `ReadProcessMemory` on our own process so a page freed
meanwhile only fails the read), and the main thread checks which has seats (`vmap x y z` runs it once and lists them).

Two copies with a spawned M2 Rover in each (different ids): the owner on A's gun put Frank on B's gun (placed, no
entry animation); A's driver seat put him in B's driver seat; B's player bound to B's gun by script while the owner
drove A's Rover: "Frank in B is driving the car and looks good". Not working yet: an AI body bound to a gunner or
passenger seat is not carried; when the car drove off, A's Frank was left behind and ran (players and AI drivers stay
attached). The way forward is the AI's own vehicle-use event, `CEventMercCommandUseVehicleSeat` (handler
FUN_1096cc90), which enemy crews use. Test commands: `vseats <vehicle>`, `vmap <x> <y> <z>`.

### Driven legs

Packet magic `FC2A`. The player's movement code ends by writing its desired movement into the control block (+0xb0/+0xb4/+0xb8, world m/s; FUN_10143de0), and the pawn's locomotion follows it with its own walk, run and sprint animations; an AI pawn does too (one copy, `ctl <Frank> move`: 1.5 m/s walked at ~1.5, 3.8 ran at ~3.6, 7.9 sprinted at ~7.4, stopping within 0.7 m). The sender sends its own; the body's copy, on foot, writes the peer's movement plus a pull toward the peer's position (1.2 per second, at most 4 m/s) into the body's block on each update, and holds MoveTo (`MoveToStopped` instead). The 10 m teleport, the glide and the vehicle seat stay; on foot the glide starts only above 12 m/s (a sprint's first burst is 8.9 m/s, above the default `aiglide` 8). The sprint flag is the write block's bit (set by the key) or the read block's (+0x2d0, kept by the movement code for the rest of the sprint). `aidrive 0` goes back to the MoveTo follower.

### Gun up, strafing

The player's pawn always aims (its control block +4 has AimAt, 0x01, on; +0x38/+0x40 are the camera's pitch and yaw). An AI pawn driven through +0xb0 turns to its movement, but while it aims (a `ShootAtTarget` that the probe ends after the first aim step) it keeps its facing and strafes or walks backwards with its own animations (copy A, owner watching: aimed north, 1.5 m/s east and then south, 9.6 m each, facing held). So the body keeps the peer's gun up along the peer's camera whenever it holds the peer's weapon (`aiready 1`, default), re-aimed when the camera turns 0.6 degrees or the body has moved 1 m, at most every 50 ms (`aiaim <ms>`), and lowers it while the peer sprints. `aiready 0` aims only while the peer aims.

### Facing

While the peer stands still (target speed under 0.3 m/s for 0.5 s) and the body is within 2.5 m of it, the body turns to the peer's facing at 270 degrees a second with `CEntity::SetEuler` (the packet's direction is the entity's local X axis, which is what SetEuler's yaw gives). The AI keeps a yaw set this way while the pawn stands; owner saw Frank turn with `vset`. Moving, the AI steers; aiming or burning, the aim order faces the body.

### Crouch, sprint and jumps

Packet magic `FC29`. The player's movement keys end in the pawn's control block (pawn data + 0x140; the input action dispatcher Dunia+0x144760): crouch toggles +0xa4, jump sets +4 bit 0x20, sprint +4 bit 0x40. The sender sends its crouch and sprint state as flags and counts its jumps (the jump bit coming on, seen in the pawn-update hook). The body's copy writes the same fields into the body's control block before and after each of its updates. A jump is queued with the peer's position and made when the body comes within 1.2 m of it (or 1.5 s late), at most one per 0.9 s so that it has landed; the sender counts at most one jump per 0.3 s. An AI pawn crouches and rises on a jump this way, but plays no jump animation. `ctl` does the same by hand on any pawn.

### Rockets and flame

Rockets count as shots: the sender counts a launch (the projectile strategy's Fire, slot 8), and the body calls the same slot on its launcher, which it holds raised along the peer's camera direction. Flame is a packet flag: the body's flame strategy gets StartShoot (slot 10) when the flag comes on and StopShoot (slot 11) when it goes off or the peer's weapon changes. An aim order makes a pawn draw the weapon it finds best (a rifle over a launcher), so the body carries only the peer's current weapon: the others are taken away on each switch. A weapon of its own it draws anyway (Frank_Bilders_Unarmed has one) is removed too.

Called by hand on one copy, a Frank fired 4 RPG-7 rockets and burned and stopped an LPO-50. Known gap: after its first rocket, none shows in his tube; the shots still go out.

### Weapons

Each packet also carries the archetype of the weapon in the sender's hand (`weapons.Primary.AK47`, empty
for none), read from the player's weapon manager and named with `GetEntityName` when it changes. The
receiver spawns that archetype, gives it to the buddy and selects its category (`hand_hand`,
`secondary`, `primary`, `special`, from the archetype's prefix), which draws it; it keeps one spawned
weapon per category and rechecks once a second. Nothing in the peer's hand holsters the buddy's
weapon. Only names of letters, digits, `.` and `_` are used, as they go into Lua code. The packet magic
is now `FC23`, so both sides need this build.

The game's Lua weapon functions only act on the player, so the probe takes the same steps on the buddy
natively (see the comment above `wpn_cmd` in `src/probe.c`).

Two copies, the player of A switching weapons 11 times (machete, Star45, G3KA4, LPO50 flamethrower):
the buddy in B drew each one within 1 s, and the owner saw him swap with them. Holstering is not
tested yet.

### Shooting

Each packet also carries how many bullets the sender's guns have fired (packet magic `FC24`). The
probe counts them in a detour on the engine's fire-one-bullet function (Dunia+0x113230, the
shooter's id is in its first argument). The receiver fires the new ones from the buddy's gun, at most
5 behind and 30 ms apart, while the buddy holds the peer's weapon. A gun fires on the `shootbullet`
event of its firing animation, so the probe hands the gun's bullet fire strategy such an event (see
the comment above `weapon_bullet`). Rockets and flame are synced too (see "Rockets and flame"); not grenades, melee or the mortar.

Each packet also carries the direction of the sender's last bullet (packet magic `FC25`). The engine
takes a bullet's origin and direction from one of two aim functions: the camera aim for the
player-controlled pawn (Dunia+0x112600) and the pawn's own aim for any other (Dunia+0x112080). The
probe keeps the direction from the first, and in the second replaces the buddy's direction with the
peer's. The bullet still starts at the buddy's muzzle, so it flies parallel to the peer's.

Two copies, the player of A firing single shots, bursts, at the ground and at a car with the G3: the
buddy in B fired along (41+ bullets), each with A's direction (for example -0.07 0.24 -0.97 for the
shots at the ground, where his own aim was -0.71 -0.70 -0.01), and the owner saw them go the same way.
The bullets damage enemies: an enemy soldier 6 m away took damage from 5 `wfire` shots (neither a
`MoveTo` to a marker nor `hold` keeps a test enemy in place). In the synced runs the owner heard the
buddy's shots. The buddy's arms do not follow the aim yet. `LookAtTarget` turns him but leaves
the barrel pointing at the ground, so it is not used.

Aiming is being worked on. `ShootAtTarget` (a Domino command) on a plain marker entity makes the
buddy turn and aim at it at once, with the game's own pose, but he then fires on his own and nothing
found so far ends that task (a stop signal, `MoveTo`, removing or killing the target, holstering).
A stack trace showed that the AI's gun fires through the weapon manager's fire-one-shot step
(Dunia+0x1305f0), the same path as the player's gun. `wmute` skips that step for one pawn: then it
aims, fires no bullet, makes no sound and does not reload, but its gun still jolts as if firing, and
the task still cannot be ended. Writing the aim fields of the pawn's control block directly (`waim`,
`waimat`, no shoot task) gives a broken pose, the gun pointing at the pawn's feet. The handoff notes
in the private workspace have the full map of what was tried.

### Buddy rescue

When the local player goes down and the game has a rescue buddy, it spawns him
(`GetBuddiesManager():GetRescueBuddyEntityId()`), he revives the player and fights until he has killed an
enemy. The probe leaves him alone during all of that (moving or teleporting him mid-rescue made his
model vanish) and removes its own buddy at once, so there are never two. When he has reached the player
and then stayed more than 8 m away for 5 seconds, or after `airescue` seconds (default 180), or when the
game removes him, the probe spawns its buddy where he stands and removes him (`RemoveEntity`;
`RemoveBuddyFromWorld` leaves him there). The game's rescue buddy ignores `MoveTo` afterwards, so he
cannot be the peer's buddy himself.

Two copies, B's player shot down with Frank set as the rescue buddy: Frank revived B's player, fought,
and was swapped for the peer's buddy 59 s later, only once the fight was over. The game then offers no
rescue for a while, so going down again soon is a real death.

The probe asks the engine once a second which vehicle the player is in; it now only does so while the
player exists, since during a reload `CVehicle_GetCurrentVehicle` crashed the game (Dunia+0xe7337).

### Aiming

Each packet also carries whether the sender's player aims and the camera's direction (a flag and a direction in the packet),
read from the player's control block (pawn data + 0x140): bit 0x08 of byte +4, pitch at +0x38, yaw at +0x40.

The buddy's pose comes from the game's `ShootAtTarget` command at a marker along that direction, 30 m ahead. That
command's shoot task never ends by itself, so the probe ends it on its first aim step, through its own burst stop
(FUN_10979990) and finished flag (+0x91), about 30 ms later. The buddy keeps the pose, without firing or jolting. His
own AI stays muted, and only the peer's bullets go out. He lowers his gun when the peer stops aiming, or 3 s after
a shot fired from the hip: his pawn attribute task (FUN_109a67c0) keeps setting the AimAt and LookAt bits every
frame, so it then runs with both settings off. A new direction goes out at most about 3 times a second.

Two copies on one PC (build `589b6996`), the owner aiming, turning and hip-firing in A: "all working and smooth".
Not tried over two PCs. Guns only.

### Peer death

Each packet also carries a flag that is set while the sender's game-over screen is up (packet magic
`FC26`). The probe sees that screen come and go by wrapping the enter and exit slots (5 and 6) of the
game-over state's vtable (`CFCXGRStateGameOver`, Dunia+0xeaace0).

`KillPawnAgent` sends a pawn 100000 damage. `Frank_Bilders_Unarmed`, the buddy the scripts spawn, ignores it,
invincible or not, while `Frank_Bilders` goes down. So when the peer's flag turns on, the receiver replaces the
buddy where he stands with the archetype without `_Unarmed` and kills that one once it has loaded, which shows
as a brief pop. When the flag turns off, it removes him and spawns a new buddy at the peer. He goes down when the
peer's game-over screen appears, not at the moment the peer is hit.

Two copies on one PC, A's player killed (build `3dac71db`): B replaced the buddy 62 ms after A's game-over
screen and killed the replacement 0.55 s later. The owner saw the pop, and after A's reload the buddy followed
as before. Not tried over two PCs.

### Teleports and following

A position jump above 60 m/s counts as a teleport, not a speed: a reload's position once threw each new buddy
so far ahead that the engine dropped him. A `MoveTo` sent while the pawn runs an older one did not replace it,
and a teleport did not end it: after a sprint the buddy kept running to an old goal while the peer stood still,
was teleported back at 10 m, and repeated that until the peer walked on. A teleport or the end of a glide now
sends `MoveToStopped`, and the new `MoveTo` 200 ms later. Sprinting in A with stops, same build: 1 teleport, 2
glides, median 0.93 m from A (129 samples); before the change 22 teleports in a similar run.

## Automated two-copy test

`tests/two_copy_walk.ps1` runs the two-copy setup above without a human: it starts both test
copies, presses Enter until each has loaded the save from its own profile, runs `netstart` on
both, walks copy A's player on a circle with `walk`, and checks from the logs and `pos`
readings that copy B's avatar follows A while B's own player stays put. Then it turns A's
player three times with relative mouse movement and checks that B's avatar faces the same
way (within 15 degrees). A kill switch stops
both copies if a log line names the real profile folder, and the real profile folder is
compared with a snapshot afterwards. Both copies must already be patched and have their
profiles; the script does not patch anything.

```
powershell -ExecutionPolicy Bypass -File tests\two_copy_walk.ps1 -CopyA <test copy A> -CopyB <test copy B> -OutDir <results folder>
```

With `-Relay` the probes talk to two `relay/fc2_relay.py` processes (udp transport) instead of to
each other, and two more checks require both relays to have carried traffic. With
`-PeerIp <this PC's LAN address>` both copies use the network mode, and a further check
sends 30 forged packets from `127.0.0.1` to copy B and requires them to be dropped. The windows
are titled `A` and `B` so they are easy to tell apart.

`-MoveBAside <m>` teleports B's own player `m` metres off A's circle before the walk (both players
start on the same spot and the circle runs through it, so the avatar can bump B's player and fail
`B player stays put`). `-AiBody` shows A in copy B as an AI body instead of the puppet: B runs
`aibody <-Buddy> remote` (default Frank) before the link; it checks `buddy spawned B` (invincible),
`no puppet avatar B`, `buddy follows A` (mean and max distance within
`-AiMeanLimit` 4 / `-AiMaxLimit` 8 m) and `buddy not teleported`, and skips the heading checks.
`-TestSettings` switches on `ai_IgnorePlayer` and `cheat_GodMode` in both copies, for saves next to
enemies. Last AI-body run (2026-10-28, `-AiBody -TestSettings -MoveBAside 5 -WalkRadius 6 -WalkSpeed 1.5
-WalkSeconds 25`):

```
RESULT: 21 checks, 1 failed -> FAIL
  FAIL buddy follows A: distance to A: mean 4.39 m, max 7.34 m (limits 4 / 8)
  link 15.6 / 16.0 packets/s; no puppet avatar B; 0 teleports; B player drift 0.00 m; no crash lines
```

The circle is harder to follow than real movement (the buddy cuts across a target that keeps
turning; a real player was followed at 1.57 m mean). `-WalkLegs "<speed> <pause> <dx1> <dy1> ..."`
walks A along straight legs relative to its start with a pause at each corner (probe command
`walklegs`) instead of the circle. A 10 m x 8 m rectangle west of the start, 2 s pauses:

```
-WalkLegs "1.5 2 -10 0 0 8 10 0 0 -8"  (walking, 32 s):  21 checks, 1 failed
  FAIL buddy follows A: mean 3.07 m, max 8.12 m (limits 4 / 8); 0 teleports
-WalkLegs "3.5 2 -10 0 0 8 10 0 0 -8"  (running, 18 s):  21 checks, 2 failed
  FAIL buddy follows A: mean 4.99 m, max 9.89 m;  FAIL buddy not teleported: 2 teleports
```

The buddy needs about a second to start after each order, so it falls furthest behind at the
start of each leg and when the peer runs. The walking run again with `-Relay` (both probes through
two `fc2_relay.py` processes, udp transport): 23 checks, 1 failed (`buddy follows A`: mean 2.98 m,
max 9.19 m, 0 teleports); the relays carried 759 / 703 packets each way with 0 dropped.

`-AiRespawn` (with `-AiBody`) then removes B's buddy and requires a new one near A, and kills B's
player (`cheat_GodMode = 0`, `Game:SetHealth(0)`), presses Enter on the death screen ("Load last
Save" is the first choice) and requires a new buddy near A in the reloaded world:

```
-AiBody -AiRespawn -TestSettings -MoveBAside 5 -WalkLegs "1.5 2 -10 0 0 8 10 0 0 -8":  25 checks, 0 failed -> PASS
  buddy follows A: mean 2.66 m, max 7.39 m; buddy spawned B: invincible ok
  buddy respawned B: within the 1 s liveness check after removal; 0.01 m from A 5 s later
  buddy back after death and reload B: 13 s, 2 Enter presses; 1.01 m from A 5 s later
```

The follow distance is close to its limit on this path (max 7.39 to 9.19 m over four runs), so that
check can still fail.

It presses Enter and moves the mouse in the game windows, so leave keyboard and mouse alone for
the ~2.5 minutes it runs. Exit code 0 = pass, 1 = a check failed, 2 = setup problem; `run.txt`,
`results.json`, `positions_b.csv` and both logs go to `<results folder>\<timestamp>\`. The check
`B player stays put` is the one that catches the avatar-binding bug (with the unfixed probe
B's own player was dragged 16 m); `avatar heading follows A` fails on a probe without the
heading code (the avatar stays at 0 degrees).

## How it works

- `DllMain` loads the real `dinput8.dll` and a watcher thread patches the import table
  of `Dunia.dll` (`bind`, `connect`, `sendto`, `recvfrom`, `WSASendTo`, `WSARecvFrom`,
  `getaddrinfo`, `gethostbyname`, `CreateFileA/W`, `PeekMessageA/W`, `PostQuitMessage`).
- An inline detour on `lua_gettop` captures the Lua state; the Lua entry points are
  `lua_dobuffer`, `lua_getglobal`, `lua_settop` and `luaL_check_lstr` at fixed RVAs.
- `PeekMessage` runs on the main thread when no script runs, so commands, the per-frame body
  writes and the transport run there.
- The physics body is found by scanning memory in 64 KiB chunks with `ReadProcessMemory`
  (a plain read can fault when another thread frees a page) for objects of one class and
  taking the one whose motion transform is nearest to the entity. Heap addresses change every
  session; the class does not.
- Heading: the body is an upright capsule whose rotation is always identity; where a pawn faces
  is the yaw in the entity matrix (`entity+0x30`, rows `(d0,d1,0)`, `(-d1,d0,0)`, `(0,0,1)`). For
  the local player the controller rewrites it every frame from mouse look; the avatar has no
  controller, so the rotation the probe writes there each tick stays (smoothed 25 % per tick).
  Writing the body's rotation, or calling `TeleportEntity` with angles, changes nothing;
  `SpawnEntityFromArchetype` only uses its last argument (yaw in degrees) at spawn.
- An exception filter and a vectored handler log access violations. Many at start-up come
  from system libraries and are harmless.

## Known limits

The same as in the write-up: no walking animation for the puppet avatar (the AI body has it, but
trails the peer by a few metres, takes its own route around obstacles and faces where it walks),
no weapons, health or mission state, direct UDP has no NAT traversal (use the Steam relay), exact build only, and positions written into a body bypass
collision (the avatar passes through scenery; it mirrors the peer's already-collided position).

## License

MIT, see [LICENSE](LICENSE).

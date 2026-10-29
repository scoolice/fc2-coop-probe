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
| `netstop` | Stop it and remove the avatar |
| `bodies` | Log every body of the class above with its position (AI pawns do not use this class) |
| `ents <id> <radius>` | Log every object of the same class as the entity's object within `radius` m, with its entity id (finds nearby pawns; `GetEntityName(id)` names them) |
| `aibody <archetype> remote\|local` | Spawn an AI pawn of `archetype` (for example `buddies.Buddies.Frank_Bilders_Unarmed`) at the target once there is one, make it invincible and follow the peer or the local player; the pawn and its marker are spawned again whenever the engine reports either as gone (`IsEntityValid`, checked once a second: a death or checkpoint reload removes both) or the pawn ignores five orders in a row |
| `aifollow <pawn> remote\|local\|circle [<r> <v>]` | Make an AI pawn follow the peer (`remote`), the local player (`local`, needs `findbody` first) or a test circle of radius `r` at `v` m/s (see [AI body](#ai-body-an-animated-peer)); with `remote` the puppet avatar is not spawned |
| `aistall <ms> <m>` / `ailead <s>` / `aispeed <n>` / `aitp <m>` | Follower tuning: re-order a pawn that stood `ms` while more than `m` away (default 6000 / 3); lead the marker by `s` s of the target's velocity (default 0); MoveTo speed value (default 3); teleport distance (default 10) |
| `aiglide <on> <off>` | Above `on` m/s of target speed (a vehicle; default 8) the pawn stops walking and is teleported onto the marker every frame, the marker extrapolated from the last packet by up to 0.25 s; below `off` m/s (default 5) it walks again. `aiglide 0 0` turns it off. A two-copy run with A moving at 10 m/s: glide on 0.5 s after the start, buddy 0.00-0.31 m from A, walking again 0.3 s after the stop |
| `vinfo <id>` / `vphys <id> 0\|1` / `vset <id> <x> <y> <z> <yaw>` | Log an entity's physics components; switch its physics off or on; place it with `CEntity::SetEuler` / `SetPosition` (yaw in radians). The way FC2JackalFix's noclip moves the player, and safe on vehicles, where `TeleportEntity` crashes (Dunia+0x9fcf02) |
| `winfo <pawn>` / `wadd <pawn> <weapon>` / `wsel <pawn> primary\|secondary\|hand_hand\|special` / `wdraw <pawn>` / `wholster <pawn>` | Weapons on any pawn: log its weapon manager (mode, state, weapon in hand); give it a spawned weapon entity; select a category (also draws); draw; holster |
| `sig <pawn> <name\|0xhash>` | Send a signal (crc32 of its name) to the pawn's state driver, as the player's input does (`config/inputactionmapcommon.xml`: `startshooting`, `reload`, `jump`, ...). On an AI pawn `startshooting` does nothing |
| `wbullet <pawn> [entity]` | Fire one bullet from the gun in the pawn's hand: a `shootbullet` animation event handed to its bullet fire strategy. Muzzle flash, no sound; the bullet follows the pawn's aim |
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

### Through Steam, without IP addresses: `relay/fc2_relay.py`

A relay next to each game copy carries the probe's packets, so friends don't need IPs or port
forwarding. The probe talks only to its local relay: `netstart <game port> 127.0.0.1 <relay port> <id>`.
Python 3, 64-bit; the Steam transport also needs the Steam client running and `steam_api64.dll`
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
  invincible buddy, spawned again after a death or reload; the 5 s status line adds its distance, orders,
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
the comment above `weapon_bullet`). Only bullet weapons so far, not flame, rockets, grenades or melee.

Each packet also carries the direction of the sender's last bullet (packet magic `FC25`). The engine
takes a bullet's origin and direction from one of two aim functions: the camera aim for the
player-controlled pawn (Dunia+0x112600) and the pawn's own aim for any other (Dunia+0x112080). The
probe keeps the direction from the first, and in the second replaces the buddy's direction with the
peer's. The bullet still starts at the buddy's muzzle, so it flies parallel to the peer's.

Two copies, the player of A firing single shots, bursts, at the ground and at a car with the G3: the
buddy in B fired along (41+ bullets), each with A's direction (for example -0.07 0.24 -0.97 for the
shots at the ground, where his own aim was -0.71 -0.70 -0.01), and the owner saw them go the same way.
The shots make no sound and the buddy's arms do not follow the aim. `LookAtTarget` turns him but leaves
the barrel pointing at the ground, so it is not used.

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

# Ultimate64 compatible REST API

VICE can serve the HTTP API of the 1541-Ultimate / Ultimate64 hardware, so that
launchers and tools written against a real device can drive the emulator without
being changed. The routes, parameter names, URL grammar and JSON keys follow the
firmware; where the hardware has no counterpart in an emulator, the route is
absent rather than faked.

## Which emulators have it

`x64`, `x64sc` and `xscpu64`. The API belongs to a specific piece of hardware:
the Ultimate64 is a C64 board, and the 1541-Ultimate II+ is a device on the C64
cartridge port. The other emulators are built without it, `vsid` included, so
its options and resources do not exist there rather than advertising a device
those machines never had.

`x128` is left out on purpose. The hardware does reach further than the C64 —
the firmware recognises C128 cartridges — but supporting the C128 here means
testing it there, and that has not been done.

## Enabling it

    x64sc -restapi                                   # localhost:8080
    x64sc -restapi -restapiaddress ip4://127.0.0.1:8464
    x64sc -restapi -restapipassword hunter2

| Resource               | Default                | Meaning                                    |
|------------------------|------------------------|--------------------------------------------|
| `RESTAPIServer`        | `0`                    | serve the API                              |
| `RESTAPIServerAddress` | `ip4://127.0.0.1:8080` | address to bind to                         |
| `RESTAPIPassword`      | empty                  | required in the `X-Password` request header |
| `RESTAPIHostname`      | empty                  | name reported to clients; empty = the host's own name |

The server is off by default and binds to localhost. Two reasons not to change
that lightly: the API takes host file names, so a client can ask VICE to read
any file the emulator itself can read, and the hardware's API has no
authentication beyond the optional password. The device is an appliance on a home
LAN; VICE is a program on your workstation.

The real device serves on port 80. VICE defaults to 8080 because binding a port
below 1024 needs privileges no emulator should ask for, so clients need to be
told the port.

## URL grammar

    /v1/<route>[/<path>][:<command>][?<parameters>]

The query string is separated first, then the command, then the path, which is
why a `:` inside a query string is not a command separator. A missing command is
the command `none`. `%XX` escapes and `+` are decoded. This matches
`parse_url_static()` in the firmware's HTTP core (GideonZ/MicroHttpServer).

Every response is a JSON object that ends in an `errors` array — empty when the
call succeeded:

    $ curl -X PUT 'http://127.0.0.1:8080/v1/runners:run_prg?file=/tmp/demo.prg'
    {"errors":[]}

`PUT` acts on a file that already exists on the host. `POST` takes the file with
the request as `multipart/form-data`, which is spooled to a temporary file and
then used exactly as `PUT` would use a host file.

## Implemented routes

| Method     | Route                     | Parameters             | Effect                              |
|------------|---------------------------|------------------------|-------------------------------------|
| GET        | `/v1/version`             |                        | API version (`0.1`, as on hardware) |
| GET        | `/v1/info`                |                        | product, VICE version, machine      |
| GET        | `/v1/help`                | `command`              | the device's placeholder page       |
| PUT        | `/v1/machine:reset`       |                        | CPU reset                           |
| PUT        | `/v1/machine:reboot`      |                        | power cycle                         |
| PUT        | `/v1/machine:pause`       |                        | pause emulation                     |
| PUT        | `/v1/machine:resume`      |                        | resume emulation                    |
| PUT        | `/v1/machine:poweroff`    |                        | quit the emulator                   |
| GET        | `/v1/machine:readmem`     | `address`, `length`    | memory as the CPU sees it, binary   |
| PUT        | `/v1/machine:writemem`    | `address`, `data`      | write 1 to 128 bytes given in hex   |
| POST       | `/v1/machine:writemem`    | `address`, upload      | write the uploaded bytes            |
| PUT        | `/v1/runners:run_prg`     | `file`                 | autostart a program                 |
| POST       | `/v1/runners:run_prg`     | uploaded file          | autostart an uploaded program       |
| PUT/POST   | `/v1/runners:load_prg`    | `file` / upload        | load without running                |
| PUT/POST   | `/v1/runners:run_crt`     | `file` / upload        | attach a cartridge and start it     |
| PUT        | `/v1/runners:sidplay`     | `file`, `songnr`       | play a SID file                     |
| POST       | `/v1/runners:sidplay`     | upload(s), `songnr`    | play an uploaded SID file           |
| GET        | `/v1/drives`              |                        | state of drives `a` and `b`         |
| PUT        | `/v1/drives/<d>:mount`    | `image`, `mode`        | attach a disk image                 |
| POST       | `/v1/drives/<d>:mount`    | upload, `mode`         | attach an uploaded disk image       |
| PUT        | `/v1/drives/<d>:remove`   |                        | detach                              |
| PUT        | `/v1/drives/<d>:reset`    |                        | reset the drive CPU                 |
| PUT        | `/v1/drives/<d>:on`       |                        | switch the drive on                 |
| PUT        | `/v1/drives/<d>:off`      |                        | switch the drive off                |
| PUT        | `/v1/drives/<d>:set_mode` | `mode`                 | make it a 1541, 1571 or 1581        |
| PUT/POST   | `/v1/drives/<d>:load_rom` | `file` / upload        | replace the drive's DOS ROM         |
| PUT        | `/v1/files/<path>:create_d64` | `tracks`, `diskname` | create a formatted D64 (35 or 40 tracks) |
| PUT        | `/v1/files/<path>:create_d71` | `diskname`         | create a formatted D71              |
| PUT        | `/v1/files/<path>:create_d81` | `diskname`         | create a formatted D81              |

`<d>` is `a` (unit 8) or `b` (unit 9); unit numbers `8` to `11` are accepted too.
Parameters are always read from the query string, including on `POST`, as they
are on the hardware — a `mode` sent as a form field is not seen.

`mode` is `readwrite` or `readonly`. The hardware's `unlinked` mode keeps writes
in the device's RAM and has no equivalent here, so it is rejected rather than
silently turned into a write to the user's image. Omitting `mode` leaves the
unit's `AttachDevice<n>d0Readonly` setting alone, where the hardware would
default to read/write: a unit its user has set read only should not be quietly
switched back by a mount.

An uploaded file outlives the request that brought it in, because the emulator
goes on using it: a mounted image is read for as long as it stays attached, and
`autostart_autodetect()` on a disk or tape image attaches it for the session. The
temporary file is dropped when the same slot receives another upload, when the
unit is emptied, or when the emulator exits. There is one slot per drive unit,
one for `runners`, and one for `run_crt`.

## Deliberate differences

`GET /v1/info` reports `product` as `VICE <machine>` and `firmware_version` as
VICE's version, and adds an `emulator` key. `hostname` is the name of the machine
VICE runs on, or `RESTAPIHostname` when that is set — clients use it as the
device's display name, so several emulators on one host want distinct names. The `fpga_version`, `core_version`
and `unique_id` keys of the hardware are absent — there is no FPGA to report on.

`machine:pause` does nothing in the headless build: that UI's pause loop is a
stub in VICE itself (`arch/headless/ui.c`), so emulation keeps running even
though the call reports success. It works in the GTK3 and SDL builds.

`help` answers exactly what firmware 1.1.0 does, which is a placeholder: a page
headed "This function provides some help!" saying "Help text.", whatever
`command` names, with the device's `Content-Type: text_html` (sic). Without
`command`, or with any other parameter, it fails the device's generic parameter
check in the device's words ("Function none requires parameter command").

`machine:readmem` and `machine:writemem` go through the CPU's view of memory,
as the device's DMA does: `$D020` is the VIC register while I/O is mapped in,
reading `$E000` gives the KERNAL ROM, and a write under a ROM lands in the RAM
below it. Reading is a peek, so looking at an I/O register does not acknowledge
an interrupt the way a real bus read would. The address is read the way firmware
1.1.0 reads it, with `strtol()` and a range check only: `12zz` is `$0012` and
`zz` is `$0000`. Later firmware for other Ultimate products rejects those; VICE
follows Commodore's release.

`machine:poweroff` quits VICE, after the response has gone out. A client that
switches the machine off expects it to be gone.

`drives:off` sets the unit's drive type to none and `drives:on` brings back the
type it had, since VICE has no separate power switch for a drive. `set_mode` on a
drive that is off changes the type it comes back with, as on the device.
`set_mode 1541` leaves a 1541-II alone: it already is a 1541 in the API's terms.

`drives:load_rom` replaces the DOS ROM resource of the drive's current type
(`DosName1541` and so on), so it applies to every unit of that type, where the
device loads it into one drive. The ROM must be 16K or 32K, and 32K for a 1570,
1571 or 1581. The original resource values come back when the emulator exits, so
saved settings never point at an uploaded ROM's temporary file.

The `files` routes take the rest of the URL as an absolute host path:
`/v1/files/tmp/new.d64:create_d64` creates `/tmp/new.d64`. Unlike the device,
they refuse to overwrite an existing file, since on a host that file can be
anything the user owns. `create_d64` takes 35 or 40 tracks, the sizes VICE
recognises, where the device takes 35 to 41. A `diskname` ending in `,XY` sets
the disk ID, as on the device; without one, the file name minus its extension
names the disk. The BAM covers the standard 683 sectors even on a 40 track image,
as the device's format does.

`runners:sidplay` plays a tune the way the device does, with the device's own
player: the "Ultimate SID Player" cartridge by Wilfred Bos, assembled from
firmware 1.1.0's sources (`restapi-sidcrt/`, rebuilt by `restapi-sidcrt.sh`) with
changes on top: the placement fix from the firmware's master (in 3.15a), and the
info screen as proposed in GideonZ/1541ultimate#949: a FOUND line with the
detected model for every SID the tune uses, and a WANT line with the model the
header asks for, the second and third numbered, the clock on the first line of
each block only. The model can be detected only for a SID in `$D400`-`$D4FF`; a
SID elsewhere shows as unknown. A SID whose model the header leaves open shows
the first SID's, as the SID file format defines it, and UNKNOWN when the first
is open too.
The tune's header is prepared as the firmware prepares it, the machine is reset
into the cartridge, and the tune is written to memory when the cartridge asks for
it. The cartridge then switches itself off through `$DFFF`, a switch of the
device's cartridge emulation that VICE's generic 16KiB cartridge has for this
purpose only, and is detached. It replaces any cartridge attached before. Song
lengths come from `SONGLENGTHS/<name>.ssl` next to the file, as on the device,
or from a second uploaded file on `POST`. The player is GPLv3, where VICE is GPLv2
or later, so this part cannot go upstream.

A file named `.mus` or `.str` is Compute's Sidplayer data, which the device
plays the same way with its MUS player cartridge, built from the same sources,
and COMPUTE!'s Sidplayer by Craig Chamberlain and Harry Bratt at `$E000`. The
header is made up as the firmware makes it: one song, three minutes long unless
a `.ssl` says otherwise, the file name as title, flags for an 8580 and NTSC. The
data goes to `$1000`. A stereo song, either embedded after the text or in the
`.str` file next to the `.mus`, gets a second SID at `$D500`; choosing the `.str`
plays the `.mus` with it. An upload is kept under the name it was sent with,
so an uploaded `.mus` plays as MUS too; on the device it lands in the `/Temp`
folder, where a `.str` of the same name would be found, while here it plays
without one.

The device maps its SIDs to the addresses the tune asks for, each with the model
the tune asks for; VICE sets `SidStereo`, `Sid2AddressStart`/`Sid3AddressStart`
and `SidModel`/`Sid2Model`/`Sid3Model` accordingly. A further SID whose model the
tune leaves open gets the first one's, as on the device. These settings stay as
the tune left them.

`GET /v1/drives` folds VICE's drive models into the API's vocabulary: 1540, 1541,
1541-II and 1551 report as `1541`, the 1570/1571 family as `1571`, the 1581 as
`1581`. A model with no counterpart (a CMD FD-2000, say) reports its VICE type
number instead, on the grounds that an unrecognised value is more useful to a
client than a wrong one.

## Not implemented

- `runners:modplay` — the MOD player drives the sample playback hardware of the
  Ultimate's FPGA, which VICE does not emulate.
- `runners:sidplay` for a SID file flagged as holding Compute's Sidplayer data:
  the device takes those through its SID path with the data at `$1000`, which
  is not reproduced here.
- `machine:debugreg`, `machine:measure` — they read FPGA internals.
- `machine:menu_button` — there is no device menu to open.
- `drives:unlink` — like the `unlinked` mount mode, it keeps writes in the
  device's RAM, which has no equivalent here.
- `configs/*` — VICE's resources and the device's configuration items are
  different sets of things; mapping them needs a decision about naming first.
- `files:info` — it answers from the device's own filesystem; here it would
  report on any host path, which wants limits designed in first.
- `files:create_dnp` — VICE knows CMD native partitions only inside D1M, D2M,
  D4M and DHD images, and cannot mount a bare `.dnp` file.
- `streams:start`, `streams:stop` — the device pushes VIC and audio streams over
  UDP; a worthwhile feature, and a separate one.
- Chunked request bodies. Clients of this API announce a `Content-Length`.

## Worth doing next: the Ident Service

The hardware is discoverable, and VICE is not. Since firmware 3.11 the device
runs an *Ultimate Ident Service* — `SocketDMA::identThread` in
`software/network/socket_dma.cc`: a UDP server on **port 64**, bound to
`INADDR_ANY`, answering unicast and broadcast, enabled by default. Send any
payload; if its first four bytes are exactly `json` the reply is a JSON object
with `product`, `firmware_version`, `fpga_version`, `core_version`, `hostname`,
`menu_header`, an echo of the request bytes after the first four, and
`password_protected` when a password is set. Anything else gets a CSV line,
`<request truncated to 32 bytes>,<hostname>,<menu_header>`.

This is how existing tools are meant to find a device, and answering it would
make VICE visible to all of them at once rather than requiring each client to
learn about emulators — which is otherwise the alternative, since a client that
identifies devices by the `product` string will not recognise `VICE <machine>`.
It is a small amount of code: a UDP socket polled from the same vsync hook, and
the same values `/v1/info` already reports. Left out of this patch to keep it to
one feature, and because answering a discovery broadcast is a decision about
visibility on the local network that deserves its own resource and its own
default rather than riding along on `RESTAPIServer`.

## Implementation notes

`src/restapi.c` owns the listening socket and the connections, and serves them
from the vsync hook — once per emulated frame, on the emulation thread, like the
remote monitor. Handlers therefore run at a point where machine state is
consistent and may call the machine API directly.

The pause loop of each UI (`ui_pause_loop_iteration()`) also serves the API.
Without that, a machine paused over REST could not be resumed over REST, because
the vsync hook does not run while paused.

`src/restapi_http.c` parses requests and builds responses; `src/restapi_routes.c`
is the route table. Requests are read without blocking: a request that has not
arrived in full is left for the next frame.

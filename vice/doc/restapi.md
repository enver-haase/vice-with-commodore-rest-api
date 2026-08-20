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
| PUT        | `/v1/machine:reset`       |                        | CPU reset                           |
| PUT        | `/v1/machine:reboot`      |                        | power cycle                         |
| PUT        | `/v1/machine:pause`       |                        | pause emulation                     |
| PUT        | `/v1/machine:resume`      |                        | resume emulation                    |
| PUT        | `/v1/runners:run_prg`     | `file`                 | autostart a program                 |
| POST       | `/v1/runners:run_prg`     | uploaded file          | autostart an uploaded program       |
| PUT/POST   | `/v1/runners:load_prg`    | `file` / upload        | load without running                |
| PUT/POST   | `/v1/runners:run_crt`     | `file` / upload        | attach a cartridge and start it     |
| GET        | `/v1/drives`              |                        | state of drives `a` and `b`         |
| PUT        | `/v1/drives/<d>:mount`    | `image`, `mode`        | attach a disk image                 |
| POST       | `/v1/drives/<d>:mount`    | upload, `mode`         | attach an uploaded disk image       |
| PUT        | `/v1/drives/<d>:remove`   |                        | detach                              |
| PUT        | `/v1/drives/<d>:reset`    |                        | reset the drive CPU                 |

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

`GET /v1/drives` folds VICE's drive models into the API's vocabulary: 1540, 1541,
1541-II and 1551 report as `1541`, the 1570/1571 family as `1571`, the 1581 as
`1581`. A model with no counterpart (a CMD FD-2000, say) reports its VICE type
number instead, on the grounds that an unrecognised value is more useful to a
client than a wrong one.

## Not implemented

- `runners:sidplay`, `runners:modplay` — the SID player belongs in `vsid`, and
  the MOD player is a REU program on the device.
- `machine:readmem`, `machine:writemem`, `machine:debugreg`, `machine:measure` —
  the binary monitor already covers memory access properly.
- `machine:menu_button` — there is no device menu to open.
- `configs/*` — VICE's resources and the device's configuration items are
  different sets of things; mapping them needs a decision about naming first.
- `files/*` — filesystem access over HTTP wants sandboxing designed in, not
  bolted on.
- `streams:start`, `streams:stop` — the device pushes VIC and audio streams over
  UDP; a worthwhile feature, and a separate one.
- Chunked request bodies. Clients of this API announce a `Content-Length`.

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

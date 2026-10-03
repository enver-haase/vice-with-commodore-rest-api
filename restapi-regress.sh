#!/bin/zsh
# Regression suite for the Ultimate64-compatible REST API in VICE.
#
# Needs three fixtures in $S; restapi-fixtures.sh generates them.
#
# The emulator is driven over REST on 8464 and inspected through the text
# monitor on 6510, so it needs no display.
S=${RESTAPI_FIXTURES:-$(cd "$(dirname "$0")" && pwd)/fixtures}
V=${VICE_TREE:-$(cd "$(dirname "$0")" && pwd)/vice}
B=http://127.0.0.1:8464/v1
M="nc -w 4 127.0.0.1 6510"
pass=0; fail=0
ok()   { print -- "  PASS  $1"; pass=$((pass+1)) }
bad()  { print -- "  FAIL  $1"; fail=$((fail+1)) }
check(){ if [[ "$2" == *"$3"* ]]; then ok "$1"; else bad "$1 (got: ${2:0:90})"; fi }
screen(){ printf 'm 0400 07e7\nx\n' | eval $M | grep -ic "$1" }
hex(){ od -An -tx1 | tr -d ' \n' }
# the text on the screen the VIC shows, wherever the player put it
infoscreen(){ python3 - "$B" <<'PY'
import sys, urllib.request
B = sys.argv[1]
rd = lambda a, n: urllib.request.urlopen(f"{B}/machine:readmem?address={a:04X}&length={n}").read()
scr = (3 - (rd(0xDD00, 1)[0] & 3)) * 0x4000 + (rd(0xD018, 1)[0] >> 4) * 0x400
print("".join(chr(0x40 + c) if 1 <= c <= 26 else (chr(c) if 32 <= c <= 90 else " ")
              for c in (x & 0x7f for x in rd(scr, 1000))))
PY
}
drive_a(){ curl -s -m 5 $B/drives | python3 -c "import json,sys; d=json.load(sys.stdin)['drives'][0]['a']; print(d['$1'])" }

# -9: an emulator stuck in a loop ignores SIGTERM and would keep the port
pkill -9 -f "x64sc -restapi" 2>/dev/null; sleep 1
cd $V/src
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy VICE_DATADIR=$V/data \
  ./x64sc -restapi -restapiaddress ip4://127.0.0.1:8464 \
          -remotemonitor -remotemonitoraddress ip4://127.0.0.1:6510 \
          -sounddev dummy -warp > $S/regress.log 2>&1 &
sleep 8

print "== basics =="
check "GET version"      "$(curl -s -m 5 $B/version)" '"version":"0.1"'
check "GET info"         "$(curl -s -m 5 $B/info)" '"product":"VICE C64SC"'
check "help needs command" "$(curl -s -m 5 $B/help)" "Function none requires parameter command"
check "help unknown param" "$(curl -s -m 5 "$B/help?command=x&foo=1")" "Function none does not have parameter foo"
check "help page"        "$(curl -s -m 5 "$B/help?command=machine")" "<h1>This function provides some help!</h1>"
check "help as text_html" "$(curl -s -m 5 -o /dev/null -w '%{content_type}' "$B/help?command=machine")" "text_html"
check "GET drives parses" "$(curl -s -m 5 $B/drives | python3 -c 'import json,sys; json.load(sys.stdin); print("valid-json")')" "valid-json"

print "== runners: host file =="
check "PUT run_prg"      "$(curl -s -m 5 -X PUT "$B/runners:run_prg?file=$S/test.prg")" '"errors":[]'
sleep 6
[[ $(screen "rest ok") -gt 0 ]] && ok "program ran from host path" || bad "program did not run from host path"

print "== runners: multipart upload =="
curl -s -m 5 -X PUT $B/machine:reboot > /dev/null; sleep 4
[[ $(screen "rest ok") -eq 0 ]] && ok "reboot cleared the screen" || bad "reboot did not clear the screen"
check "POST run_prg"     "$(curl -s -m 10 -X POST -F file=@$S/test.prg $B/runners:run_prg)" '"errors":[]'
sleep 8
[[ $(screen "rest ok") -gt 0 ]] && ok "uploaded program ran" || bad "uploaded program did not run"

print "== drives: uploaded image stays readable =="
curl -s -m 5 -X PUT $B/machine:reboot > /dev/null; sleep 4
check "POST mount"       "$(curl -s -m 10 -X POST -F image=@$S/test.d64 $B/drives/a:mount)" '"errors":[]'
IMG=$(curl -s -m 5 $B/drives | python3 -c "import json,sys; d=json.load(sys.stdin)['drives'][0]['a']; print(d['image_path']+d['image_file'])")
[[ -f "$IMG" ]] && ok "uploaded image survives its request" || bad "uploaded image was deleted"
printf 'keybuf load"hello",8,1\\nrun\\n\nx\n' | eval $M > /dev/null; sleep 10
[[ $(screen "rest ok") -gt 0 ]] && ok "drive loaded from the uploaded image" || bad "drive could not read the uploaded image"

print "== drives: mode handling =="
check "bogus mode"       "$(curl -s -m 5 -X PUT "$B/drives/a:mount?image=$S/test.d64&mode=sideways")" "Unsupported mode"
check "unlinked mode"    "$(curl -s -m 5 -X PUT "$B/drives/a:mount?image=$S/test.d64&mode=unlinked")" "Unsupported mode"
check "invalid drive"    "$(curl -s -m 5 -X PUT "$B/drives/z:mount?image=$S/test.d64")" "Invalid Drive"
check "missing image"    "$(curl -s -m 5 -X PUT $B/drives/a:mount)" "Missing required parameter"
check "PUT remove"       "$(curl -s -m 5 -X PUT $B/drives/a:remove)" '"errors":[]'
check "PUT drive reset"  "$(curl -s -m 5 -X PUT $B/drives/a:reset)" '"errors":[]'

print "== drives: power, mode and ROM =="
check "set_mode 1581"    "$(curl -s -m 5 -X PUT "$B/drives/a:set_mode?mode=1581")" '"mode":"1581"'
check "now a 1581"       "$(drive_a type)" "1581"
check "off"              "$(curl -s -m 5 -X PUT $B/drives/a:off)" '"errors":[]'
check "reported off"     "$(drive_a enabled)" "False"
check "set_mode while off" "$(curl -s -m 5 -X PUT "$B/drives/a:set_mode?mode=1571")" '"errors":[]'
check "stays off"        "$(drive_a enabled)" "False"
check "on"               "$(curl -s -m 5 -X PUT $B/drives/a:on)" '"errors":[]'
check "comes back as set" "$(drive_a type)" "1571"
check "unknown mode"     "$(curl -s -m 5 -X PUT "$B/drives/a:set_mode?mode=1551")" "Invalid Drive Type"
check "unknown drive"    "$(curl -s -m 5 -X PUT $B/drives/softiec:on)" "Invalid Drive"
head -c 16384 /dev/zero > $S/rom16k.bin
check "16K ROM in a 1571" "$(curl -s -m 5 -X PUT "$B/drives/a:load_rom?file=$S/rom16k.bin")" "Drive ROM is invalid"
check "back to 1541"     "$(curl -s -m 5 -X PUT "$B/drives/a:set_mode?mode=1541")" '"errors":[]'
ROM=$(ls $V/data/DRIVES/dos1541-325302-01+901229-05.bin)
check "POST load_rom"    "$(curl -s -m 10 -X POST -F file=@$ROM $B/drives/a:load_rom)" '"errors":[]'
[[ "$(drive_a rom)" == */vice.* ]] && ok "drive runs the uploaded ROM" || bad "drive ROM not replaced ($(drive_a rom))"
check "alive after ROM"  "$(curl -s -m 5 $B/version)" '"version":"0.1"'

print "== drives: repeated type changes =="
# drive_set_disk_drive_type() once set the drive clock back to 0 without
# resetting the disk rotation; x64sc then spun through an unsigned wrap of
# the cycle count and hung, within a dozen rounds of this in every run
hung=0
for round in $(seq 1 15); do
  for m in 1571 1541 1581 1541 1571 1581; do
    if [[ -z "$(curl -s -m 5 -X PUT "$B/drives/a:set_mode?mode=$m")" ]]; then
      hung=1; break 2
    fi
    sleep 0.$((RANDOM % 9))
  done
  curl -s -m 5 -X PUT $B/drives/a:off > /dev/null; curl -s -m 5 -X PUT $B/drives/a:on > /dev/null
done
(( hung == 0 )) && ok "90 type changes, emulator still answers" || bad "emulator hung in round $round changing to $m"
curl -s -m 5 -X PUT "$B/drives/a:set_mode?mode=1541" > /dev/null

print "== pause / resume =="
check "pause"            "$(curl -s -m 5 -X PUT $B/machine:pause)" '"errors":[]'
check "served while paused" "$(curl -s -m 5 $B/version)" '"version":"0.1"'
check "resume"           "$(curl -s -m 5 -X PUT $B/machine:resume)" '"errors":[]'
check "alive after resume" "$(curl -s -m 5 $B/version)" '"version":"0.1"'

print "== memory =="
check "PUT writemem"     "$(curl -s -m 5 -X PUT "$B/machine:writemem?address=d020&data=05")" '"address":"d020-d020"'
check "VIC register"     "$(curl -s -m 5 "$B/machine:readmem?address=d020&length=1" | hex)" "f5"
check "KERNAL ROM"       "$(curl -s -m 5 "$B/machine:readmem?address=e000&length=2" | hex)" "8556"
check "binary response"  "$(curl -s -m 5 -o /dev/null -w '%{content_type}' "$B/machine:readmem?address=0400")" "application/octet-stream"
printf '\001\002\003' > $S/three.bin
check "POST writemem"    "$(curl -s -m 10 -X POST -F file=@$S/three.bin "$B/machine:writemem?address=c000")" '"address":"c000-c002"'
check "read back"        "$(curl -s -m 5 "$B/machine:readmem?address=c000&length=3" | hex)" "010203"
check "address as 1.1.0" "$(curl -s -m 5 -X PUT "$B/machine:writemem?address=c0zz&data=00")" '"address":"00c0-00c0"'
check "address too high" "$(curl -s -m 5 "$B/machine:readmem?address=10000")" "Invalid address"
check "address signed"   "$(curl -s -m 5 "$B/machine:readmem?address=-1")" "Invalid address"
check "data not hex"     "$(curl -s -m 5 -X PUT "$B/machine:writemem?address=c000&data=0g")" "Invalid char 'g' at position 1."
check "write past FFFF"  "$(curl -s -m 5 -X PUT "$B/machine:writemem?address=ffff&data=0000")" 'exceeds location $FFFF'
check "read past FFFF"   "$(curl -s -m 5 "$B/machine:readmem?address=ff00&length=512")" 'exceeds location $FFFF'

print "== files: creating images =="
C=$S/created; rm -rf $C; mkdir -p $C
check "create_d64"       "$(curl -s -m 5 -X PUT "$B/files$C/a.d64:create_d64?diskname=hello%2Cab")" '"bytes_written":174848'
check "create_d64 40"    "$(curl -s -m 5 -X PUT "$B/files$C/b.d64:create_d64?tracks=40")" '"bytes_written":196608'
check "create_d71"       "$(curl -s -m 5 -X PUT "$B/files$C/c.d71:create_d71")" '"bytes_written":349696'
check "create_d81"       "$(curl -s -m 5 -X PUT "$B/files$C/d.d81:create_d81")" '"bytes_written":819200'
check "37 tracks"        "$(curl -s -m 5 -X PUT "$B/files$C/e.d64:create_d64?tracks=37")" "Track count should be 35 or 40."
check "no overwrite"     "$(curl -s -m 5 -X PUT "$B/files$C/a.d64:create_d64")" "File exists"
check "D64 name and ID"  "$($V/src/c1541 -attach $C/a.d64 -dir 2>/dev/null)" '"hello           " ab 2a'
check "D64 40 tracks"    "$($V/src/c1541 -attach $C/b.d64 -dir 2>&1)" "40 tracks"
check "D71 blocks free"  "$($V/src/c1541 -attach $C/c.d71 -dir 2>/dev/null)" "1328 blocks free"
check "D81 blocks free"  "$($V/src/c1541 -attach $C/d.d81 -dir 2>/dev/null)" "3160 blocks free"
check "mount created"    "$(curl -s -m 5 -X PUT "$B/drives/a:mount?image=$C/c.d71")" '"errors":[]'
curl -s -m 5 -X PUT $B/drives/a:remove > /dev/null

print "== malformed input =="
check "no /v1"           "$(curl -s -m 5 http://127.0.0.1:8464/version)" "Not a supported API endpoint"
check "unknown route"    "$(curl -s -m 5 $B/bogus)" "Unknown route"
check "unknown command"  "$(curl -s -m 5 -X PUT $B/machine:selfdestruct)" "Unknown command"
check "bad method"       "$(curl -s -m 5 -X DELETE $B/version)" "Unsupported method"
check "chunked"          "$(curl -s -m 5 -X POST -H 'Transfer-Encoding: chunked' --data-binary xx $B/runners:run_prg)" "Chunked transfer encoding"
check "colon in query"   "$(curl -s -m 5 "$B/version?note=a:b")" '"version":"0.1"'
check "garbage line"     "$(printf 'NOTHTTP\r\n\r\n' | nc -w 2 127.0.0.1 8464 | head -1)" "404"
check "survives all"     "$(curl -s -m 5 $B/version)" '"version":"0.1"'

print "== runners: sidplay =="
mem(){ curl -s -m 5 "$B/machine:readmem?address=$1&length=$2" | hex }
check "PUT sidplay"      "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test.sid")" '"errors":[]'
sleep 4
check "default song"     "$(mem 11f0 1)" "00"
A=$(mem 11f1 1); sleep 1; Z=$(mem 11f1 1)
[[ "$A" != "$Z" ]] && ok "tune plays ($A -> $Z)" || bad "tune does not play ($A -> $Z)"
check "cartridge gone"   "$(mem 8004 5)" "0000000000"
check "PUT song 3"       "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test.sid&songnr=3")" '"errors":[]'
sleep 4
check "song 3 started"   "$(mem 11f0 1)" "02"
check "POST song 2"      "$(curl -s -m 10 -X POST -F file=@$S/test.sid "$B/runners:sidplay?songnr=2")" '"errors":[]'
sleep 4
check "song 2 started"   "$(mem 11f0 1)" "01"
check "song too high"    "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test.sid&songnr=4")" "Invalid Song Number Requested"
check "song past 256"    "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test.sid&songnr=300")" "Undefined subsystem command"
check "not a SID"        "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test.prg")" "Error detected in file format"
check "no such file"     "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/nothing.sid")" "Cannot open file"
check "MUS data"         "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test-mus.sid")" "not supported on this architecture"
# Compute's Sidplayer files take the MUS player cartridge, which puts the data
# at $1000 and the player at $E000, in RAM under the KERNAL, where readmem
# sees the ROM; so the info screen it leaves is what is checked
check "PUT .mus"         "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/test.mus")" '"errors":[]'
sleep 4
check "MUS data at 1000" "$(mem 1000 8)" "020002000200014f"
check "MUS player"       "$(infoscreen)" 'ULTIMATE MUS PLAYER'
check "title from name"  "$(infoscreen)" 'TITLE : TEST'
check "one SID"          "$(infoscreen)" 'WANT  : $D400 : 8580 / NTSC (CIA)'
check "cartridge gone"   "$(mem 8004 5)" "0000000000"
# choosing the .str plays the .mus with it, and a second SID at $D500
check "PUT .str"         "$(curl -s -m 5 -X PUT "$B/runners:sidplay?file=$S/stereo.str")" '"errors":[]'
sleep 4
check "stereo title"     "$(infoscreen)" 'TITLE : STEREO'
check "second SID"       "$(infoscreen)" 'WANT 2: $D500 : 8580'
# an upload keeps the name it was sent with, so its extension still tells a
# .mus from a SID file; its .str is not next to it, and an uploaded .str finds
# no .mus
check "POST .mus"        "$(curl -s -m 10 -X POST -F file=@$S/stereo.mus $B/runners:sidplay)" '"errors":[]'
sleep 4
check "uploaded, mono"   "$(infoscreen)" 'WANT  : $D400 : 8580 / NTSC (CIA)'
[[ "$(infoscreen)" != *'WANT 2'* ]] && ok "no second SID for an upload" || bad "an uploaded .mus found its .str"
check "POST .str"        "$(curl -s -m 10 -X POST -F file=@$S/stereo.str $B/runners:sidplay)" "Cannot open file"
check "POST raw .mus"    "$(curl -s -m 10 -X POST --data-binary @$S/test.mus -H 'Content-Type: application/octet-stream' $B/runners:sidplay)" "Error detected in file format"

print "== runners: cartridge =="
check "PUT run_crt"      "$(curl -s -m 5 -X PUT "$B/runners:run_crt?file=$S/test.crt")" '"errors":[]'
sleep 5
[[ $(screen "crt ok") -gt 0 ]] && ok "cartridge cold-started" || bad "cartridge did not start"

# the test cartridge parks the CPU in a loop, so this runs after anything
# that needs the keyboard
print "== poweroff quits and removes retained uploads =="
T=${TMPDIR:-/tmp}
curl -s -m 10 -X POST -F image=@$S/test.d64 $B/drives/b:mount > /dev/null
BEFORE=$(ls $T/vice.* 2>/dev/null | wc -l | tr -d ' ')
check "poweroff answers" "$(curl -s -m 5 -X PUT $B/machine:poweroff)" '"errors":[]'
sleep 3
pgrep -f "x64sc -restapi" > /dev/null && bad "emulator still running after poweroff" || ok "emulator quit"
AFTER=$(ls $T/vice.* 2>/dev/null | wc -l | tr -d ' ')
[[ $AFTER -lt $BEFORE ]] && ok "temp files removed on exit ($BEFORE -> $AFTER)" || bad "temp files left behind ($BEFORE -> $AFTER)"

pkill -f "x64sc -restapi" 2>/dev/null
print ""
print "RESULT: $pass passed, $fail failed"
(( fail == 0 ))

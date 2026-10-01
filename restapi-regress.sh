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

pkill -f "x64sc -restapi" 2>/dev/null; sleep 1
cd $V/src
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy VICE_DATADIR=$V/data \
  ./x64sc -restapi -restapiaddress ip4://127.0.0.1:8464 \
          -remotemonitor -remotemonitoraddress ip4://127.0.0.1:6510 \
          -sounddev dummy -warp > $S/regress.log 2>&1 &
sleep 8

print "== basics =="
check "GET version"      "$(curl -s -m 5 $B/version)" '"version":"0.1"'
check "GET info"         "$(curl -s -m 5 $B/info)" '"product":"VICE C64SC"'
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

print "== pause / resume =="
check "pause"            "$(curl -s -m 5 -X PUT $B/machine:pause)" '"errors":[]'
check "served while paused" "$(curl -s -m 5 $B/version)" '"version":"0.1"'
check "resume"           "$(curl -s -m 5 -X PUT $B/machine:resume)" '"errors":[]'
check "alive after resume" "$(curl -s -m 5 $B/version)" '"version":"0.1"'

print "== malformed input =="
check "no /v1"           "$(curl -s -m 5 http://127.0.0.1:8464/version)" "Not a supported API endpoint"
check "unknown route"    "$(curl -s -m 5 $B/bogus)" "Unknown route"
check "unknown command"  "$(curl -s -m 5 -X PUT $B/machine:selfdestruct)" "Unknown command"
check "bad method"       "$(curl -s -m 5 -X DELETE $B/version)" "Unsupported method"
check "chunked"          "$(curl -s -m 5 -X POST -H 'Transfer-Encoding: chunked' --data-binary xx $B/runners:run_prg)" "Chunked transfer encoding"
check "colon in query"   "$(curl -s -m 5 "$B/version?note=a:b")" '"version":"0.1"'
check "garbage line"     "$(printf 'NOTHTTP\r\n\r\n' | nc -w 2 127.0.0.1 8464 | head -1)" "404"
check "survives all"     "$(curl -s -m 5 $B/version)" '"version":"0.1"'

print "== runners: cartridge =="
check "PUT run_crt"      "$(curl -s -m 5 -X PUT "$B/runners:run_crt?file=$S/test.crt")" '"errors":[]'
sleep 5
[[ $(screen "crt ok") -gt 0 ]] && ok "cartridge cold-started" || bad "cartridge did not start"

# the test cartridge parks the CPU in a loop, so this runs after anything
# that needs the keyboard
print "== clean shutdown removes retained uploads =="
T=${TMPDIR:-/tmp}
curl -s -m 10 -X POST -F image=@$S/test.d64 $B/drives/b:mount > /dev/null
BEFORE=$(ls $T/vice.* 2>/dev/null | wc -l | tr -d ' ')
printf 'quit\n' | eval $M > /dev/null 2>&1; sleep 3
AFTER=$(ls $T/vice.* 2>/dev/null | wc -l | tr -d ' ')
[[ $AFTER -lt $BEFORE ]] && ok "temp files removed on exit ($BEFORE -> $AFTER)" || bad "temp files left behind ($BEFORE -> $AFTER)"

pkill -f "x64sc -restapi" 2>/dev/null
print ""
print "RESULT: $pass passed, $fail failed"
(( fail == 0 ))

#!/bin/sh
# Cross-platform tests against raw FAT32 image files (no loop device required).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CDIR=$ROOT/tests
FD=$ROOT/fatdefrag
IMG=$CDIR/test.img
EMPTY=$CDIR/empty.img
STALE=$CDIR/stale.img
HOST=$CDIR/payload.bin

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

"$FD" --help >/dev/null || fail "help"
"$FD" -h >/dev/null || fail "help -h"

# --- fragmented check / defrag ---
"$CDIR/mkfat32" "$IMG"
out=$("$FD" check "$IMG" -v)
echo "$out"
echo "$out" | grep -q "FAT32" || fail "probe FAT32"
echo "$out" | grep -q "fragmented: 1" || fail "expected 1 fragmented file"
echo "$out" | grep -q "GAP.BIN" || fail "verbose lists GAP.BIN"
echo "$out" | grep -q "extents=5" || fail "GAP.BIN should have 5 extents"

"$FD" check "$IMG" -f /GAP.BIN | grep -q "extents=5" || fail "check -f"

"$FD" defrag "$IMG" -n -v | grep -q "dry-run" || fail "dry-run message"
"$FD" check "$IMG" -f GAP.BIN | grep -q "extents=5" || fail "dry-run must not change FS"

"$FD" defrag "$IMG" -v -f /GAP.BIN
out=$("$FD" check "$IMG" -v)
echo "$out"
echo "$out" | grep -q "fragmented: 0" || fail "after defrag, no fragmented files"
echo "$out" | grep GAP.BIN | grep -q "extents=1" || fail "GAP.BIN should be contiguous"

# content: 5 clusters of incrementing bytes
python3 - "$IMG" <<'PY' || fail "python hash verify"
import sys
path = sys.argv[1]
data = open(path, "rb").read()
bps, spc, rsv, nfats = 512, 8, 32, 2
fatsz = 64
first_data = rsv + nfats * fatsz
def cl_off(cl):
    return (first_data + (cl - 2) * spc) * bps
# GAP dirent at root cluster 2, second entry
root = cl_off(2)
ent = data[root+32:root+64]
cl = int.from_bytes(ent[26:28], "little") | (int.from_bytes(ent[20:22], "little") << 16)
fat = data[rsv*bps:]
def fatget(c):
    return int.from_bytes(fat[c*4:c*4+4], "little") & 0x0FFFFFFF
payload = bytearray()
n = 0
while 2 <= cl < 0x0FFFFFF8 and n < 16:
    payload += data[cl_off(cl):cl_off(cl)+4096]
    cl = fatget(cl)
    n += 1
expect = bytes(i & 0xFF for i in range(5*4096))
if payload != expect:
    sys.stderr.write("payload mismatch after defrag (len %d)\n" % len(payload))
    sys.exit(1)
print("content hash ok (%d bytes, %d clusters)" % (len(payload), n))
PY
pass "FAT32 check/defrag"

# --- copy ---
"$CDIR/mkfat32" --empty "$EMPTY"
dd if=/dev/urandom of="$HOST" bs=4096 count=6 2>/dev/null
"$FD" copy "$HOST" "$EMPTY:/COPIED.BIN" -v
out=$("$FD" check "$EMPTY" -v)
echo "$out"
echo "$out" | grep -q "COPIED.BIN" || fail "copy did not create file"
echo "$out" | grep COPIED.BIN | grep -q "extents=1" || fail "copied file must be contiguous"
python3 - "$EMPTY" "$HOST" <<'PY' || fail "copy content mismatch"
import sys, hashlib
img, host = sys.argv[1], sys.argv[2]
data = open(img, "rb").read()
want = open(host, "rb").read()
bps, spc, rsv, nfats, fatsz = 512, 8, 32, 2, 64
first_data = rsv + nfats * fatsz
def cl_off(cl):
    return (first_data + (cl - 2) * spc) * bps
root = data[cl_off(2):cl_off(2)+4096]
# find COPIED.BIN 8.3 (COPIED  BIN) or LFN
cl = None
size = None
i = 0
while i < 4096:
    ent = root[i:i+32]
    if not ent[0]:
        break
    if ent[0] == 0xE5 or ent[11] == 0x0F:
        i += 32
        continue
    name = ent[0:11]
    if name.startswith(b"COPIED"):
        cl = int.from_bytes(ent[26:28], "little") | (int.from_bytes(ent[20:22], "little") << 16)
        size = int.from_bytes(ent[28:32], "little")
        break
    i += 32
if cl is None:
    sys.exit("COPIED not found")
fat = data[rsv*bps:]
def fatget(c):
    return int.from_bytes(fat[c*4:c*4+4], "little") & 0x0FFFFFFF
payload = bytearray()
n = 0
while 2 <= cl < 0x0FFFFFF8 and n < 64:
    payload += data[cl_off(cl):cl_off(cl)+4096]
    cl = fatget(cl)
    n += 1
payload = payload[:size]
if payload != want:
    sys.exit("copy payload mismatch")
print("copy content ok")
PY
pass "FAT32 copy"

# --- journal recovery ---
"$CDIR/mkfat32" --stale "$STALE"
"$CDIR/plantjnl" "$STALE"
"$FD" check "$STALE" 2>&1 | grep -qi journal || fail "check should warn about journal"
"$FD" defrag "$STALE" -v
out=$("$FD" check "$STALE" -v)
echo "$out"
echo "$out" | grep -q "fragmented: 0" || fail "recovery should leave GAP contiguous"
pass "journal recovery"

# --- exFAT (self-contained image, no mkfs required) ---
EX=$CDIR/exfat.img
EXEMPTY=$CDIR/exempty.img
"$CDIR/mkexfat" "$EX"
out=$("$FD" check "$EX" -v)
echo "$out"
echo "$out" | grep -q "exFAT" || fail "exFAT probe"
echo "$out" | grep -q "GAP.BIN" || fail "exFAT lists GAP.BIN"
echo "$out" | grep GAP.BIN | grep -q "extents=3" || fail "exFAT GAP extents"
"$FD" defrag "$EX" -v -f /GAP.BIN
out=$("$FD" check "$EX" -v)
echo "$out"
echo "$out" | grep -q "fragmented: 0" || fail "exFAT after defrag"
echo "$out" | grep GAP.BIN | grep -q "extents=1" || fail "exFAT GAP contiguous"
pass "exFAT check/defrag"

"$CDIR/mkexfat" --empty "$EXEMPTY"
"$FD" copy "$HOST" "$EXEMPTY:/COPIED.BIN" -v
out=$("$FD" check "$EXEMPTY" -v)
echo "$out"
echo "$out" | grep -q "COPIED.BIN" || fail "exFAT copy did not create file"
echo "$out" | grep COPIED.BIN | grep -q "extents=1" || fail "exFAT copied file contiguous"
pass "exFAT copy"

echo "ALL TESTS PASSED"

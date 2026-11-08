#!/usr/bin/env bash
# Fetch the WyBot stock firmware images listed in manifest.tsv and verify their MD5.
# The binaries themselves are NOT committed to this repo (they are WyBot's proprietary
# images); they are served unauthenticated over plain HTTP from gateway.wybotpool.com.
set -euo pipefail
cd "$(dirname "$0")"
DEST="bin"
mkdir -p "$DEST"
pass=0; fail=0
# skip header + comment lines
while IFS=$'\t' read -r board role chip model hw sw url md5; do
  [[ "$board" == "board" || "$board" == \#* || -z "${url:-}" ]] && continue
  fname="$DEST/$(basename "$url")"
  echo ">> $board/$model  $(basename "$url")"
  if [[ ! -f "$fname" ]]; then
    curl -fsSL -o "$fname" "$url" || { echo "   DOWNLOAD FAILED"; ((fail++)); continue; }
  fi
  got=$(md5 -q "$fname" 2>/dev/null || md5sum "$fname" | awk '{print $1}')
  if [[ "$got" == "$md5" ]]; then echo "   md5 OK"; ((pass++)); else echo "   md5 MISMATCH got=$got want=$md5"; ((fail++)); fi
done < manifest.tsv
echo "---- $pass verified, $fail failed ----"

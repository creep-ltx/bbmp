#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build every replayer on macOS, one after another, and say which were built and which
# failed (the end of each log). Used by .github/workflows/mac.yml; a replayer that is
# already there (restored from the cache) is not built again.
set -uo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
LOGS="${LOGS:-$HERE/mac-logs}"
BUILT="${BBMP_OPT:-$HOME/.local/opt}/.mac-built"     # what was built, from what (cached with it)
mkdir -p "$LOGS" "$BUILT"
SCRIPTS=(build-out.sh build-sidplayfp.sh build-uade.sh build-zxtune.sh build-vgm.sh build-chip.sh build-psgplay.sh
         build-snsf.sh build-furnace.sh install-sunvox.sh build-xmp.sh build-retrovert.sh build-musicline.sh build-fac.sh
         build-psycle.sh build-iigs.sh build-nez.sh build-console.sh build-misc.sh
         build-pc.sh build-synth.sh build-amiga.sh build-retro.sh build-ay.sh build-adlib.sh
         build-darkwave.sh build-pcmusic.sh build-piece.sh build-chipnsfx.sh
         build-tiatracker.sh build-synder.sh build-t0ast.sh build-berotracker.sh build-axs.sh build-gme.sh)
ok=() failed=()
for s in "${SCRIPTS[@]}"; do
  name="${s%.sh}"
  [ -f "$HERE/tools/$s" ] || continue     # not in this copy of the source
  # built before from the same script and the same code: kept from the cache
  stamp="$(cat "$HERE/tools/$s" "$HERE/tools/sources.sh" \
           $(find "$HERE/csrc/${name#build-}" -type f 2>/dev/null | sort) | shasum -a 256 | cut -c1-16)"
  if [ "$(cat "$BUILT/$name" 2>/dev/null)" = "$stamp" ]; then ok+=("$name (cached)"); continue; fi
  start=$SECONDS
  if "$HERE/tools/$s" > "$LOGS/$name.log" 2>&1; then
    ok+=("$name ($((SECONDS - start)) s)")
    echo "$stamp" > "$BUILT/$name"
  else
    failed+=("$name")
    { echo "== $name failed after $((SECONDS - start)) s; the end of its log:"
      tail -25 "$LOGS/$name.log"; echo; } >> "$LOGS/failed.txt"
  fi
done
{ echo "Built (${#ok[@]}):"; printf '  %s\n' "${ok[@]}"
  echo "Failed (${#failed[@]}):"; printf '  %s\n' "${failed[@]}"; } | tee "$LOGS/summary.txt"
[ -f "$LOGS/failed.txt" ] && cat "$LOGS/failed.txt"
exit 0

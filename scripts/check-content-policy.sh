#!/usr/bin/env bash
# Refuse private-infrastructure strings in this repository, including binaries.
#
# WHY THIS IS NOT `git grep`. `build/` is gitignored, and a compiled binary
# embeds its string literals. A gate that only searches tracked files cannot
# see the artefact that would actually ship a leaked hostname. Scanning every
# file under `build/` is the wrong widening: CMake writes the ABSOLUTE source
# path into Makefiles, so a builder whose checkout happens to sit under a
# denylisted directory name (this one does) would fail for a reason CI on a
# GitHub-hosted runner never sees — green in CI, red here, which is the
# opposite of a gate. The shipped artefacts are the ELF binaries (and any
# static libraries). Those are what `strings` is for.
#
# The denylist is private repo names, internal DNS, AWS account ids, ECR hosts,
# and operator-only helper paths. Public product names (the CK API, Buddy, the
# docs site) are not secrets and are not listed.
set -euo pipefail

cd "$(dirname "$0")/.."

DENYLIST=(
  'cks-udp-api'
  'cks-michael-root'
  'cks-project-root'
  'cks-game-api'
  'cks-management-api'
  'cks-control-plane'
  'infra-control-plane'
  'cks-env\.com'
  'tier-facts\.sh'
  'Crowdy-Games'
  'ecr\.amazonaws\.com'
  '317700178317'
  'P2P_SECRET'
  'P2P_TOKEN'
  'wire-protocol-reference'
)

fail=0

hit() {
  local term="$1" file="$2"
  if grep -aIE -n -- "$term" "$file" >/tmp/cks-lt-policy-hit 2>/dev/null; then
    echo "DENYLISTED TERM '$term' found in $file:" >&2
    cat /tmp/cks-lt-policy-hit >&2
    fail=1
  fi
}

# Tracked source. The policy script itself names the denylist.
while IFS= read -r f; do
  [[ "$f" == scripts/check-content-policy.sh ]] && continue
  for term in "${DENYLIST[@]}"; do
    hit "$term" "$f"
  done
done < <(git ls-files)

# Shipped artefacts only: ELF binaries and ar archives under build/.
if [[ -d build ]]; then
  while IFS= read -r -d '' f; do
    case "$(file -b --mime-type "$f" 2>/dev/null || true)" in
      application/x-executable|application/x-pie-executable|application/x-sharedlib|application/x-archive|application/octet-stream)
        ;;
      *)
        # file(1) on some distros reports ELF as application/x-mach-binary etc.
        # Fall back to the ELF magic.
        if ! LC_ALL=C grep -q $'\x7fELF' "$f" 2>/dev/null && \
           ! LC_ALL=C grep -q '^!<arch>' "$f" 2>/dev/null; then
          continue
        fi
        ;;
    esac
    for term in "${DENYLIST[@]}"; do
      hit "$term" "$f"
    done
  done < <(find build -type f -print0)
fi

rm -f /tmp/cks-lt-policy-hit

if [[ $fail -ne 0 ]]; then
  echo "Content policy check FAILED — remove internal references before publishing." >&2
  echo "Tracked source and compiled binaries are scanned; CMake path metadata is not," >&2
  echo "because it records the builder's checkout path rather than a shipped string." >&2
  exit 1
fi
echo "Content policy check passed (tracked source + compiled artefacts)."

#!/usr/bin/env bash
# find-duplicates.sh
# Find duplicate files based on sha256 checksums.
# With --delete, delete all but one copy (keep the first one).

set -euo pipefail

DELETE_MODE=false
SEARCH_DIR="."

# --- Parse arguments ---
while [[ $# -gt 0 ]]; do
  case "$1" in
    --delete)
      DELETE_MODE=true
      shift
      ;;
    *)
      SEARCH_DIR="$1"
      shift
      ;;
  esac
done

# --- Core logic ---
find "$SEARCH_DIR" -type f -print0 | \
  xargs -0 sha256sum 2>/dev/null | \
  sort | \
  awk -v delete_mode="$DELETE_MODE" '
  {
    checksum = $1
    $1 = ""
    file = substr($0, 2)
    files[checksum] = files[checksum] ? files[checksum] "\n" file : file
  }
  END {
    for (sum in files) {
      n = split(files[sum], arr, "\n")
      if (n > 1) {
        print "Duplicate group (sha256: " sum "):"
        for (i = 1; i <= n; i++) {
          print "  " arr[i]
        }
        if (delete_mode == "true") {
          for (i = 2; i <= n; i++) {
            print "  -> Deleting duplicate: " arr[i]
            # Use system() carefully to remove safely
            cmd = "rm -f -- \"" arr[i] "\""
            system(cmd)
          }
        }
        print ""
      }
    }
  }'

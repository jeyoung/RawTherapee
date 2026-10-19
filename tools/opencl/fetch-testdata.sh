#!/usr/bin/env bash
#
#  This file is part of RawTherapee.
#
#  Download the samples listed in devnotes/opencl/testdata.manifest.
#  Existing files are verified; private/own-library entries are skipped.
#
set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
readonly MANIFEST="$ROOT/devnotes/opencl/testdata.manifest"
readonly TESTDATA="${RT_TESTDATA:-$HOME/rt-testdata}"

mkdir -p "$TESTDATA"

failures=0

while IFS=$'\t' read -r ext file variant sha source licence; do
    case "$ext" in
        ''|'#'*) continue ;;
    esac

    destination="$TESTDATA/$file"

    if [ -f "$destination" ]; then
        actual="$(sha256sum "$destination" | awk '{print $1}')"
        if [ "$actual" != "$sha" ]; then
            printf 'FAIL %s: checksum mismatch\n' "$file" >&2
            failures=$((failures + 1))
        else
            printf 'ok   %s\n' "$file"
        fi
        continue
    fi

    case "$source" in
        http://*|https://*)
            printf 'get  %s\n' "$file"
            if ! curl -fsSL --retry 3 -o "$destination.part" "$source"; then
                printf 'FAIL %s: download failed\n' "$file" >&2
                rm -f "$destination.part"
                failures=$((failures + 1))
                continue
            fi
            actual="$(sha256sum "$destination.part" | awk '{print $1}')"
            if [ "$actual" != "$sha" ]; then
                printf 'FAIL %s: checksum mismatch\n' "$file" >&2
                rm -f "$destination.part"
                failures=$((failures + 1))
                continue
            fi
            mv "$destination.part" "$destination"
            ;;
        *)
            printf 'skip %s: %s (add the file manually)\n' "$file" "$source"
            ;;
    esac
done < "$MANIFEST"

exit "$failures"

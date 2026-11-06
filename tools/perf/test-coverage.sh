#!/usr/bin/env bash
#
# Check the stage test-coverage manifest against the canonical contract.
#
# Usage:
#   tools/perf/test-coverage.sh [--manifest FILE]
#
# The checker reads the block ids, the five path names and the WorkItem names
# from rtengine/stage/stage_contract.cc, then proves that
# devnotes/perf/TEST_COVERAGE.md has one row per item, that every block row
# names a host test or profile and at least one path, and that every WorkItem
# and path row is present. It exits 1 with the missing rows listed.
#
# The manifest is data. Add a block to the contract and this check fails until
# the manifest explains how the block is tested.

set -euo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"
readonly CONTRACT="$ROOT/rtengine/stage/stage_contract.cc"
readonly DEFAULT_MANIFEST="$ROOT/devnotes/perf/TEST_COVERAGE.md"

manifest="$DEFAULT_MANIFEST"

usage() {
    awk 'NR > 1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --manifest)
            [ "$#" -ge 2 ] || die "--manifest needs a file"
            manifest="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

# Resolve a relative manifest against the repository root so the checker works
# from any directory, not only the documented invocation directory.
case "$manifest" in
    /*) ;;
    *) manifest="$ROOT/$manifest" ;;
esac

[ -f "$CONTRACT" ] || die "contract not found: $CONTRACT"
[ -f "$manifest" ] || die "manifest not found: $manifest"

python3 - "$CONTRACT" "$manifest" <<'PY'
import os
import re
import sys

contract_path, manifest_path = sys.argv[1], sys.argv[2]


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


try:
    with open(contract_path, encoding="utf-8") as handle:
        contract = handle.read()
except OSError as error:
    die(f"cannot read {contract_path}: {error}")

try:
    with open(manifest_path, encoding="utf-8") as handle:
        manifest = handle.read()
except OSError as error:
    die(f"cannot read {manifest_path}: {error}")

# The block table is the initializer of `blockTable()`. Restrict the search to
# that function so path node lists and edge pairs cannot be mistaken for block
# rows. A row is an id string, a display-name string and a reuse enumerator;
# the parameter lists also start with a brace and a string, so the reuse column
# is what distinguishes a block row from a field list.
block_table_start = contract.find("blockTable()")
if block_table_start < 0:
    die("cannot locate blockTable() in the contract")
table_marker = contract.find("static const std::vector<StageBlockContract> table = {",
                             block_table_start)
if table_marker < 0:
    die("cannot locate the canonical block table initializer")
table_end_marker = "\n    };\n    return table;"
table_end = contract.find(table_end_marker, table_marker)
if table_end < 0:
    die("cannot locate the end of the canonical block table")
table_body = contract[table_marker:table_end]

block_ids = re.findall(
    r'^\s*\{"([a-z][a-z0-9._]*)",\s*"[^"]*",\s*RT_STAGE_\w+',
    table_body, re.M)
if not block_ids:
    die("the canonical block table parsed no block ids")
if len(block_ids) != len(set(block_ids)):
    die("the canonical block table repeats a block id")

# Canonical diagnostic names, taken from the `workItemName()` switch. The Count
# sentinel returns "unknown"; stop before it so the sentinel is not a row.
work_item_names = []
work_item_function = re.search(
    r"const char\* workItemName\(WorkItem item\)\s*\{(.*?)\n\}", contract, re.S)
if work_item_function is None:
    die("cannot locate workItemName() in the contract")
for member, name in re.findall(
        r'case WorkItem::(\w+):\s*\n\s*return "([^"]+)";',
        work_item_function.group(1)):
    if member == "Count":
        break
    work_item_names.append(name)
if not work_item_names:
    die("workItemName() parsed no work-item names")

# The five path names are the diagnostic names returned by stagePathName().
path_names = re.findall(
    r'case StagePath::\w+:\s*\n\s*return "([^"]+)";', contract)
if len(path_names) != 5:
    die(f"stagePathName() parsed {len(path_names)} path names, expected 5")
if len(path_names) != len(set(path_names)):
    die("stagePathName() repeats a path name")

path_set = set(path_names)


def clean(cell):
    cell = cell.strip()
    if cell.startswith("`") and cell.endswith("`"):
        cell = cell[1:-1]
    return cell.strip()


def section_lines(heading):
    """Lines under `## <heading>`, stopping at the next level-two heading."""
    lines = manifest.splitlines()
    result = []
    inside = False
    target = heading.lower()
    for line in lines:
        if line.startswith("## "):
            title = line[3:].strip().lower()
            if inside:
                break
            if title.startswith(target):
                inside = True
            continue
        if inside:
            result.append(line)
    return result


def table_rows(lines):
    rows = []
    for line in lines:
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        cells = [cell.strip() for cell in stripped.strip("|").split("|")]
        if not cells:
            continue
        if all(set(cell) <= set("-: ") for cell in cells):
            continue
        rows.append(cells)
    return rows


def data_rows(lines, header):
    rows = []
    for cells in table_rows(lines):
        if clean(cells[0]).lower() == header:
            continue
        rows.append(cells)
    return rows


def is_blank(cell):
    return clean(cell) in {"", "-", "n/a", "N/A", "\u2014"}


cmake_path = os.path.join(os.path.dirname(contract_path), "..", "CMakeLists.txt")
with open(cmake_path, encoding="utf-8") as handle:
    cmake_text = handle.read()
known_targets = set(re.findall(r"add_executable\(([A-Za-z0-9_.+-]+)", cmake_text))
known_scripts = set()
repo_root = os.path.abspath(os.path.join(os.path.dirname(contract_path), "..", ".."))
for directory in ("tools/perf", "tools/opencl"):
    full = os.path.join(repo_root, directory)
    if os.path.isdir(full):
        for name in os.listdir(full):
            if name.endswith(".sh"):
                known_scripts.add(name)
known_scripts.add("verify.sh")

def resolvable_host_token(cell):
    tokens = re.findall(r"`([^`]+)`", cell)
    for token in tokens:
        token = token.split()[0]
        if token in known_targets or token in known_scripts:
            return True
        if os.path.basename(token) in known_targets or \
            os.path.basename(token) in known_scripts:
            return True
        if token.startswith("test-") and token.endswith(".sh"):
            return token in known_scripts
    return False

block_lines = section_lines("block coverage")
work_lines = section_lines("work item coverage")
path_lines = section_lines("path coverage")

missing = []
stale = []

if not block_lines:
    missing.append("section: ## Block coverage")
if not work_lines:
    missing.append("section: ## Work item coverage")
if not path_lines:
    missing.append("section: ## Path coverage")

# Block rows: one row per contract id, with a host test/profile, at least one
# path and a verify.sh cell. `n/a` is allowed only for the verify.sh cell,
# because every block has a host-side test or profile.
block_rows = {}
for cells in data_rows(block_lines, "block id"):
    if len(cells) < 4:
        missing.append(
            f"block row: malformed row with {len(cells)} columns: {cells[0]}")
        continue
    block_id = clean(cells[0])
    if block_id not in block_ids:
        stale.append(f"block row: unknown block id {block_id}")
        continue
    if block_id in block_rows:
        missing.append(f"block row: duplicate row for {block_id}")
        continue
    block_rows[block_id] = cells

for block_id in block_ids:
    cells = block_rows.get(block_id)
    if cells is None:
        missing.append(f"block row: {block_id}")
        continue
    if is_blank(cells[1]):
        missing.append(f"block row: {block_id} has no host test or profile")
    elif not resolvable_host_token(cells[1]):
        missing.append(
            f"block row: {block_id} names no resolvable host test or profile")
    paths_cell = clean(cells[2])
    if not paths_cell:
        missing.append(f"block row: {block_id} has no path")
    else:
        named = {name for name in path_set if re.search(
            rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])",
            paths_cell)}
        if not named:
            missing.append(
                f"block row: {block_id} names no canonical path")
    if is_blank(cells[3]):
        missing.append(f"block row: {block_id} has no verify.sh cell")

# Adapter registries: every path adapter and the shared template must be
# represented, and the template must point at the handler regression test.
adapter_lines = section_lines("adapter registries")
adapter_rows = data_rows(adapter_lines, "registry")
if len(adapter_rows) < 6:
    missing.append(
        f"adapter registry rows: found {len(adapter_rows)}, expected at least 6")
if "stage-handler-test" not in "\n".join(adapter_lines):
    missing.append("adapter registry rows: no stage-handler-test reference")
for cells in adapter_rows:
    if len(cells) < 3 or not resolvable_host_token(cells[2]):
        name = clean(cells[0]) if cells else "?"
        missing.append(
            f"adapter registry row: {name} names no resolvable coverage test")

# WorkItem rows: every contract name, with a host test/profile and the blocks
# and paths that schedule it.
work_rows = {}
for cells in data_rows(work_lines, "work item"):
    if len(cells) < 4:
        missing.append(
            f"work item row: malformed row with {len(cells)} columns: {cells[0]}")
        continue
    name = clean(cells[0])
    if name not in work_item_names:
        stale.append(f"work item row: unknown work item {name}")
        continue
    work_rows[name] = cells

for name in work_item_names:
    cells = work_rows.get(name)
    if cells is None:
        missing.append(f"work item row: {name}")
        continue
    if is_blank(cells[1]):
        missing.append(f"work item row: {name} has no host test or profile")
    elif not resolvable_host_token(cells[1]):
        missing.append(
            f"work item row: {name} names no resolvable host test or profile")
    if is_blank(cells[2]):
        missing.append(f"work item row: {name} names no block")
    if is_blank(cells[3]):
        missing.append(f"work item row: {name} names no path")

# Path rows: every canonical path, with a host test/harness and a verify.sh
# mode or an explicit n/a.
path_rows = {}
for cells in data_rows(path_lines, "path"):
    if len(cells) < 3:
        missing.append(
            f"path row: malformed row with {len(cells)} columns: {cells[0]}")
        continue
    name = clean(cells[0])
    if name not in path_set:
        stale.append(f"path row: unknown path {name}")
        continue
    path_rows[name] = cells

for name in path_names:
    cells = path_rows.get(name)
    if cells is None:
        missing.append(f"path row: {name}")
        continue
    if is_blank(cells[1]):
        missing.append(f"path row: {name} has no host test or harness")
    elif not resolvable_host_token(cells[1]):
        missing.append(
            f"path row: {name} names no resolvable host test or harness")
    if len(cells) < 3 or is_blank(cells[2]):
        missing.append(f"path row: {name} has no verify.sh cell")

if missing or stale:
    print("stage coverage manifest: FAIL", file=sys.stderr)
    for item in missing:
        print(f"  missing: {item}", file=sys.stderr)
    for item in stale:
        print(f"  stale:   {item}", file=sys.stderr)
    sys.exit(1)

print(
    f"stage coverage manifest: PASS "
    f"({len(block_ids)} blocks, {len(work_item_names)} work items, "
    f"{len(path_names)} paths)"
)
PY

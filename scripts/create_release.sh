#!/usr/bin/env bash
# Create the GitHub release for the current version.
#
# Release flow:
#   1. bump project(... VERSION) in CMakeLists.txt, commit (and push)
#   2. scripts/build_all.sh (or the three build scripts one by one)
#   3. scripts/create_release.sh
#
# The script reads the version from CMakeLists.txt, collects the artifacts of
# every supported architecture that are present in release/
# (sticky-notes-<version>-<arch>.AppImage, .tar.gz and .flatpak for x86_64 and
# aarch64), asks for the release notes (multi-line Markdown in $EDITOR),
# creates the tag on the remote at HEAD and uploads the files plus SHA256SUMS
# (which the app checks before installing an update). Missing architectures are
# a warning, not an error: a release can ship what was built. It never commits
# or pushes code on its own: if HEAD is not pushed yet it asks first.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # scripts/
root="$(cd "$here/.." && pwd)"                                 # repository root

dry_run=0
assume_yes=0
notes_file=""

usage() {
    cat <<'EOF'
Usage: scripts/create_release.sh [options]

Creates the GitHub release for the version in CMakeLists.txt, uploading every
artifact found in release/ (sticky-notes-<version>-<arch>.AppImage, .tar.gz and
.flatpak for x86_64 and aarch64) plus SHA256SUMS.

Options:
  -y, --yes              do not ask for confirmation
  -F, --notes-file FILE  read the release notes from FILE instead of $EDITOR
      --dry-run          only print the commands that would run
  -h, --help             show this help
EOF
}

log() { printf '[create_release] %s\n' "$*"; }
die() { printf 'create_release: %s\n' "$*" >&2; exit 1; }

while (($#)); do
    case "$1" in
        -y|--yes)        assume_yes=1 ;;
        --dry-run)       dry_run=1 ;;
        -F|--notes-file) notes_file="${2:?--notes-file needs a file}"; shift ;;
        -h|--help)       usage; exit 0 ;;
        *)               echo "Unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
    shift
done

# Run a command, or only print it in --dry-run mode.
run() {
    if ((dry_run)); then
        printf '+ '
        printf '%q ' "$@"
        printf '\n'
    else
        "$@"
    fi
}

confirm() { # question
    ((assume_yes)) && return 0
    local reply
    read -rp "$1 [y/N] " reply || return 1
    [[ "$reply" == [yY] || "$reply" == [yY][eE][sS] ]]
}

# --- version and assets -----------------------------------------------------

version="$(sed -n 's/^project([^)]*VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -1)"
[[ -n "$version" ]] || die "Could not read the version from CMakeLists.txt"
tag="v$version"

# Every artifact that build_all.sh can produce. Whatever is in release/ is
# uploaded; a missing architecture is reported but does not stop the release.
archs=(x86_64 aarch64)
assets=()
missing=()

add_asset() { # path
    if [[ -f "$1" ]]; then
        assets+=("$1")
    else
        missing+=("$(basename "$1")")
    fi
}

for arch in "${archs[@]}"; do
    add_asset "$root/release/sticky-notes-$version-$arch.AppImage"
    add_asset "$root/release/sticky-notes-$version-$arch.tar.gz"
    add_asset "$root/release/sticky-notes-$version-$arch.flatpak"
done

((${#assets[@]})) \
    || die "No release artifacts for $version in $root/release
Run scripts/build_all.sh (or the three build scripts) first."

if ((${#missing[@]})); then
    log "Not built, not uploaded: ${missing[*]}"
    log "Run scripts/build_all.sh to produce every architecture"
fi

# --- repository state -------------------------------------------------------

git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
    || die "Not a git repository: $root"
cd "$root"

if [[ -n "$(git status --porcelain)" ]]; then
    git status --short >&2
    die "Working tree is not clean: commit (or stash) the changes first"
fi

gh auth status >/dev/null 2>&1 || die "gh is not logged in (run: gh auth login)"

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    die "Tag $tag already exists locally"
fi
if git ls-remote --exit-code --tags origin "refs/tags/$tag" >/dev/null 2>&1; then
    die "Tag $tag already exists on origin"
fi
if gh release view "$tag" >/dev/null 2>&1; then
    die "Release $tag already exists"
fi

# The release tag is created on the remote at HEAD, so HEAD must be pushed.
branch="$(git symbolic-ref --short HEAD)"
upstream="$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' 2>/dev/null || true)"
[[ -n "$upstream" ]] || die "Branch '$branch' has no upstream: push it first"
if [[ "$(git rev-parse HEAD)" != "$(git rev-parse "$upstream")" ]]; then
    log "HEAD is not pushed to $upstream yet"
    confirm "Push $branch to origin now?" || die "Push it first, then run again"
    run git push origin "$branch"
fi

# --- release notes ----------------------------------------------------------

notes_tmp="$(mktemp)"
cleanup() { rm -f "$notes_tmp"; }
trap cleanup EXIT

if [[ -n "$notes_file" ]]; then
    [[ -f "$notes_file" ]] || die "Notes file not found: $notes_file"
    cp "$notes_file" "$notes_tmp"
else
    editor="${VISUAL:-${EDITOR:-}}"
    if [[ -z "$editor" ]]; then
        for candidate in nano vi vim; do
            if command -v "$candidate" >/dev/null 2>&1; then
                editor="$candidate"
                break
            fi
        done
    fi
    [[ -n "$editor" ]] || die "No editor found: set VISUAL or EDITOR"
    read -r -a editor_cmd <<<"$editor"
    command -v "${editor_cmd[0]}" >/dev/null 2>&1 \
        || die "Editor not found: ${editor_cmd[0]}"

    log "Write the release notes in the editor (multi-line Markdown)."
    log "Save and close it to continue; an empty text aborts."
    "${editor_cmd[@]}" "$notes_tmp" || die "Editor exited with an error"
fi

grep -q '[^[:space:]]' "$notes_tmp" || die "Empty release notes, aborted"

# Checksums: the app verifies the downloaded asset against this file before
# replacing itself, so every release must carry it. The updater looks the file
# up by name, so one SHA256SUMS covering all architectures is fine.
sums="$root/release/SHA256SUMS"
asset_names=()
for f in "${assets[@]}"; do
    asset_names+=("$(basename "$f")")
done

if ((dry_run)); then
    log "Would write $sums for ${#asset_names[@]} files"
else
    ( cd "$root/release" && sha256sum "${asset_names[@]}" > "$sums" ) \
        || die "Could not write $sums"
fi

printf '\n----- release notes for %s -----\n' "$tag"
cat "$notes_tmp"
printf -- '--------------------------------\n\n'
printf 'Assets:\n'
for f in "${assets[@]}"; do
    printf '  %s (%s)\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)"
done
printf '\n'

confirm "Create release $tag and upload these files?" || die "Aborted"

# --- tag and release --------------------------------------------------------

log "Creating release $tag from $(git rev-parse --short HEAD)"
run gh release create "$tag" \
    --title "$tag" \
    --notes-file "$notes_tmp" \
    --target "$(git rev-parse HEAD)" \
    "${assets[@]}" "$sums"

if ((dry_run)); then
    log "Dry run: nothing was created"
else
    # gh created the tag on the remote; mirror it locally.
    git tag "$tag"
    log "Done. Local tag $tag created"
    gh release view "$tag" --json url --jq '"Release: " + .url'
fi

#!/usr/bin/env bash
# The last step of the New Release workflow: makes the tag and the release, or
# adds to them if this version already has them.
#
#   VERSION=1.3 PUBLISH=false WITH_PS5=true tools/ci-release.sh <folder of files>
#
# Run from a checkout of the branch being released, with its tags. Needs gh with
# GH_TOKEN. Adding to a release: its files of the same name are replaced, its
# notes keep what a person wrote above "### Install", and the install steps and
# checksums below that are written again to cover every file.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
files="${1:?usage: ci-release.sh <folder of files>}"
version="${VERSION:?set VERSION}"
publish="${PUBLISH:-false}"
with_ps5="${WITH_PS5:-false}"
tag="v$version"
work="$(mktemp -d)"
param=platform/ps5/sce_sys/param.json

commit_as_bot() {
  git config user.name "github-actions[bot]"
  git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
  git add Makefile "$param"
  git diff --cached --quiet || git commit -q -m "Release $version"
}

# ---------- the tag ----------

if ! git ls-remote --exit-code --tags origin "refs/tags/$tag" > /dev/null; then
  # On a commit of its own, on the tag only, that has this version in it. The
  # contentVersion stays as it was without a PS5 file: the homebrew catalog reads
  # it at the tag, and must not be told of an update it can't download.
  INPUT_VERSION="$version" GITHUB_ENV="$work/env" GITHUB_OUTPUT="$work/out" bash "$root/tools/ci-version.sh"
  [[ "$with_ps5" != true ]] || python3 "$root/tools/content-version.py" --sync "$version" "$param"
  commit_as_bot
  git tag "$tag"
  git push -q origin "refs/tags/$tag"
elif [[ "$with_ps5" == true ]]; then
  # A PS5 file added later: the tag's contentVersion was left behind for the
  # reason above. Moved on by one commit that changes nothing else.
  git fetch -q origin "+refs/tags/$tag:refs/tags/$tag"
  git worktree add -q --detach "$work/tag" "refs/tags/$tag"
  (
    cd "$work/tag"
    if ! python3 "$root/tools/content-version.py" --check "$version" "$param" > /dev/null 2>&1; then
      python3 "$root/tools/content-version.py" --sync "$version" "$param"
      commit_as_bot
      git tag -f "$tag" > /dev/null
      git push -q --force origin "refs/tags/$tag"
    fi
  )
fi

# ---------- checksums and notes ----------

existing=false
gh release view "$tag" --json isDraft > /dev/null 2>&1 && existing=true

: > "$work/old-sums"
if [[ "$existing" == true ]]; then
  gh release download "$tag" --pattern SHA256SUMS --dir "$work/old" 2>/dev/null &&
    cp "$work/old/SHA256SUMS" "$work/old-sums" || true
fi
rm -f "$files/SHA256SUMS" "$files/RELEASE_NOTES.md"
(cd "$files" && sha256sum -- *) | sed 's/ \*/  /' > "$work/new-sums"
# The earlier files' lines, unless this run replaces them, and then this run's.
awk 'NR == FNR { replaced[$2] = 1; next } !($2 in replaced)' "$work/new-sums" "$work/old-sums" > "$work/sums"
cat "$work/new-sums" >> "$work/sums"
sort -k2 "$work/sums" > "$files/SHA256SUMS"

if [[ "$existing" == true ]]; then
  gh release view "$tag" --json body --jq .body > "$work/body.md"
  python3 "$root/tools/release-notes.py" --intro "$work/body.md" "$root/CHANGELOG.md" "$version" \
    "$files/RELEASE_NOTES.md" "$files/SHA256SUMS"
else
  python3 "$root/tools/release-notes.py" --allow-missing "$root/CHANGELOG.md" "$version" \
    "$files/RELEASE_NOTES.md" "$files/SHA256SUMS"
fi

# ---------- GitHub's "latest" ----------

# Whatever reads "latest" (download links, the PS5 homebrew catalog) expects
# every platform there, so it needs them all, and no newer release to be there.
has() { grep -qE "$1" "$files/SHA256SUMS"; }
complete=false
if has 'chiral-ftp-ps5-.*\.zip$' && has 'chiral-ftp\.nro$' && has '\.apk$' && has '(\.dmg|\.app\.zip)$' &&
   has 'windows-x64\.exe$' && has 'x86_64\.AppImage$' && has 'aarch64\.AppImage$'; then
  complete=true
fi
current="$(gh release view --json tagName --jq .tagName 2>/dev/null || true)"
current="${current#v}"
newest=true
if [[ -n "$current" && "$current" != "$version" ]]; then
  [[ "$(printf '%s\n%s\n' "$current" "$version" | sort -V | tail -1)" == "$version" ]] || newest=false
fi
latest=false
[[ "$complete" == true && "$newest" == true ]] && latest=true

# ---------- the release ----------

if [[ "$existing" == true ]]; then
  draft="$(gh release view "$tag" --json isDraft --jq .isDraft)"
  gh release upload "$tag" "$files"/* --clobber
  gh release edit "$tag" --notes-file "$files/RELEASE_NOTES.md"
  if [[ "$draft" == true && "$publish" == true ]]; then
    gh release edit "$tag" --draft=false --latest="$latest"
    draft=false
  elif [[ "$draft" != true && "$latest" == true ]]; then
    gh release edit "$tag" --latest
  fi
else
  args=(--verify-tag --title "Chiral-FTP $version" --notes-file "$files/RELEASE_NOTES.md")
  if [[ "$publish" == true ]]; then
    args+=(--latest="$latest")
    draft=false
  else
    args+=(--draft)
    draft=true
  fi
  gh release create "$tag" "${args[@]}" "$files"/*
fi

# ---------- what happened, on the run's page ----------

{
  echo "## Chiral-FTP $version"
  [[ "$existing" != true ]] || echo "Files added to the existing release."
  if [[ "$draft" == true ]]; then
    if [[ "$latest" == true ]]; then
      echo "A draft is waiting on the Releases page. When you publish it, leave **Set as the latest release** ticked."
    else
      echo "A draft is waiting on the Releases page. When you publish it, **untick Set as the latest release**: it doesn't have every platform, or a newer release exists."
    fi
  else
    echo "Published, and live now."
    if [[ "$latest" == true ]]; then
      echo "It is GitHub's latest release."
    else
      echo "It isn't GitHub's latest release: it doesn't have every platform yet, or a newer release exists."
    fi
  fi
} >> "${GITHUB_STEP_SUMMARY:-/dev/null}"
rm -rf "$work"

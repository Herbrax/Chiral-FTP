#!/usr/bin/env bash
# For the GitHub workflows: attaches built files to the release of the tag
# being built, creating that release if this is the first platform to finish.
#
#   tools/ci-attach-release.sh <file>...
#
# It is created as a prerelease, for two reasons. Publishing one fails if the
# tag already has one, so when several platforms finish at once only the first
# creates it (drafts don't refuse: each would get its own). And a prerelease
# isn't "latest": nobody is offered a half-finished release, and
# latest_version.json ignores it, until a person turns it into a full release
# once every platform has arrived.
#
# Needs gh, GH_TOKEN, GH_REPO and GITHUB_REF_NAME (the workflows set them).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
tag="${GITHUB_REF_NAME:?set GITHUB_REF_NAME}"
version="$(sed -n 's/^VERSION := //p' "$root/Makefile")"

# The files are named after VERSION: a tag that disagrees is a forgotten bump.
[[ "$tag" == "v$version" ]] || { echo "tag $tag doesn't match VERSION $version in the Makefile" >&2; exit 1; }

if ! gh release view "$tag" >/dev/null 2>&1; then
  gh release create "$tag" --verify-tag --prerelease --title "Chiral-FTP $version" \
    --notes "Being built: the files arrive as each platform finishes." ||
    gh release view "$tag" >/dev/null  # another platform got there first
fi
gh release upload "$tag" "$@" --clobber

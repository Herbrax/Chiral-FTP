#!/usr/bin/env bash
# For the GitHub workflows: settles the version this run builds, and hands it
# to the later steps as $VERSION (and the step output "version").
#
#   INPUT_VERSION=1.2.1 tools/ci-version.sh
#
# INPUT_VERSION is what was typed when the workflow was started by hand; empty,
# the Makefile's VERSION stands. A typed one replaces that line, in this run's
# checkout only and never committed: everything takes the version from there
# -- the About screens, the servers' banners, the PS5's contentVersion, Android's
# versionCode, the Windows file properties, the file names -- so nothing can be
# left showing the old one.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
version="${INPUT_VERSION:-}"
version="${version#v}"

if [[ -n "$version" ]]; then
  # The PS5's contentVersion and Android's versionCode are made of these
  # numbers. A test build adds -alpha, -beta or -dev (1.2-beta), which the
  # numbers leave out and which keeps it from asking for updates (app/update.h).
  [[ "$version" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?(-(alpha|beta|dev)[0-9A-Za-z.]*)?$ ]] || {
    echo "::error::Version '$version' isn't usable: write MAJOR.MINOR or MAJOR.MINOR.PATCH, such as 1.2.1, and for a test build -beta after it (1.2-beta)"
    exit 1
  }
  sed -i.orig "s/^VERSION := .*/VERSION := $version/" "$root/Makefile"
  rm -f "$root/Makefile.orig"
else
  version="$(sed -n 's/^VERSION := //p' "$root/Makefile")"
fi

echo "VERSION=$version" >> "$GITHUB_ENV"
[[ -z "${GITHUB_OUTPUT:-}" ]] || echo "version=$version" >> "$GITHUB_OUTPUT"
echo "Building version $version"

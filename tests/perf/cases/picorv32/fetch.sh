#!/bin/sh
# Fetch pinned picorv32 sources into ./src (gitignored). Shallow fetch of the
# exact SHA; GitHub allows fetching arbitrary SHAs (allow-any-sha1-in-want).
set -eu

URL=https://github.com/YosysHQ/picorv32
SHA=87c89acc18994c8cf9a2311e871818e87d304568

cd "$(dirname "$0")"
rm -rf src
git init -q src
git -C src remote add origin "$URL"
git -C src fetch -q --depth 1 origin "$SHA"
git -C src checkout -q FETCH_HEAD

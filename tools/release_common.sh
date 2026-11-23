# Copyright (C) 2026 Cartlink contributors. GNU GPL v3 or later (see COPYING).
# Shared parts of tools/build_windows.sh and tools/build_linux.sh
# (sourced, not run).
#
# A release is built only from a commit: the sources are `git archive
# HEAD`, packed as cartlink-<version>-source.tar.gz next to the binary
# archive, and the build is made from that same tarball. A tree with
# uncommitted or untracked files is refused; --dirty builds it anyway
# for local testing, from the files as they are on disk, and every
# file name gets -DIRTY so it can't pass for a release.
#
# Version: the CARTLINK_VERSION file. Repo link: CARTLINK_REPO_URL.
# While that is still the OWNER placeholder a clean release build is
# refused; --allow-placeholder builds anyway and adds -PLACEHOLDER to
# the names (a local test build with a dead source link).

CARTLINK_REPO_URL=${CARTLINK_REPO_URL:-https://github.com/OWNER/cartlink}

LOW="nice -n 19"
command -v ionice >/dev/null 2>&1 && LOW="$LOW ionice -c3"

# release_init <repo root> [--dirty] [--allow-placeholder]
# Sets VERSION, COMMIT, SHORT, DIRTY (0/1), NAME
# (cartlink-<v>[-DIRTY][-PLACEHOLDER]).
release_init() {
   local root=$1; shift
   local allow_dirty=0 allow_placeholder=0
   for a in "$@"; do
      case $a in
         --dirty) allow_dirty=1 ;;
         --allow-placeholder) allow_placeholder=1 ;;
         *) echo "unknown option: $a (--dirty, --allow-placeholder)" >&2; exit 2 ;;
      esac
   done
   VERSION=$(tr -d ' \t\r\n' < "$root/CARTLINK_VERSION")
   if ! printf '%s' "$VERSION" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+$'; then
      echo "CARTLINK_VERSION must be like 0.1.0, not '$VERSION'" >&2; exit 1
   fi
   COMMIT=$(git -C "$root" rev-parse HEAD)
   SHORT=$(git -C "$root" rev-parse --short HEAD)
   DIRTY=0
   if [ -n "$(git -C "$root" status --porcelain --untracked-files=normal)" ]; then
      if [ "$allow_dirty" = 1 ]; then
         DIRTY=1
         echo "== WARNING: uncommitted changes are built in; names get -DIRTY (not for release)"
      else
         echo "The tree has uncommitted or untracked files:" >&2
         git -C "$root" status --short >&2
         echo "Commit them first (a release must match a commit), or use --dirty for a local test build." >&2
         exit 1
      fi
   fi
   NAME=cartlink-$VERSION
   [ "$DIRTY" = 1 ] && NAME=$NAME-DIRTY
   case $CARTLINK_REPO_URL in
      */OWNER/*)
         if [ "$allow_placeholder" = 1 ]; then
            NAME=$NAME-PLACEHOLDER
            echo "== WARNING: source link is the placeholder $CARTLINK_REPO_URL; names get -PLACEHOLDER (not for release)"
         elif [ "$DIRTY" = 0 ]; then
            echo "The source link is still the placeholder $CARTLINK_REPO_URL." >&2
            echo "Set CARTLINK_REPO_URL to the public repo, or use --allow-placeholder for a local test build." >&2
            exit 1
         fi ;;
   esac
   return 0
}

# release_source_tarball <repo root> <out .tar.gz>
# Clean: exactly `git archive HEAD`. Dirty: tracked + untracked,
# not-ignored files as they are on disk now (deleted files skipped).
release_source_tarball() {
   local root=$1 out=$2 prefix=$NAME-source/
   rm -f "$out"
   if [ "$DIRTY" = 0 ]; then
      git -C "$root" archive --format=tar --prefix="$prefix" HEAD | gzip -n -9 > "$out"
   else
      (cd "$root" && git ls-files -z --cached --others --exclude-standard \
         | while IFS= read -r -d '' f; do
              if [ -e "$f" ] || [ -L "$f" ]; then printf '%s\0' "$f"; fi
           done \
         | tar --null --no-recursion --transform "s|^|$prefix|" \
              --owner=0 --group=0 --numeric-owner -cf - -T -) | gzip -n -9 > "$out"
   fi
}

# release_unpack <source .tar.gz> <dir>: a fresh tree to build in.
release_unpack() {
   local tarball=$1 dir=$2
   rm -rf "$dir"
   mkdir -p "$dir"
   tar -xzf "$tarball" -C "$dir" --strip-components=1
}

# release_source_txt <out file> <platform words> <extra paragraph>
# [<licence file name in the archive>, default COPYING]
release_source_txt() {
   local out=$1 platform=$2 extra=$3 copying=${4:-COPYING}
   {
      echo "Cartlink $VERSION, $platform"
      echo
      echo "Built from commit $COMMIT"
      if [ "$DIRTY" = 1 ]; then
         echo "WITH UNCOMMITTED CHANGES: a local test build, not a release."
         echo "Its source is only in $NAME-source.tar.gz."
      fi
      echo "Built on $(date -u +%Y-%m-%d)"
      echo
      echo "Cartlink is free software under the GNU GPL v3 or later (see"
      echo "$copying). The complete source is at"
      echo "  $CARTLINK_REPO_URL (commit $COMMIT)"
      echo "and in $NAME-source.tar.gz, published with this download."
      echo "Build it with the steps in that source's README.md."
      echo
      printf '%s\n' "$extra"
      echo
      echo "Third-party code in this program and its licences:"
      echo "THIRD-PARTY-NOTICES.txt."
   } > "$out"
}

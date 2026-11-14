#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Install or uninstall Permute.
#
#   make            # build first
#   sudo ./install.sh              # /usr/local
#   ./install.sh --prefix "$HOME/.local"
#   sudo make uninstall            # zero residue
#
# The installed tree under <prefix>/lib/permute is the CLI's PERMUTE_HOME: it
# holds the Python package, the ABI headers, the guest archive and the two
# host binaries. Uninstall reads the manifest written here, so it removes
# exactly what install created and nothing else.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$here"

prefix=/usr/local
destdir=
mode=install
dry_run=0

usage() {
    cat <<'EOF'
usage: install.sh [options]

  --prefix DIR     installation prefix (default /usr/local)
  --destdir DIR    staging root, prepended to the prefix (packaging)
  --uninstall      remove the files listed in the install manifest
  --dry-run        print what would happen, change nothing
  -h, --help       this message
EOF
}

die() {
    echo "install.sh: $*" >&2
    exit 2
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            [ "$#" -ge 2 ] || die "--prefix needs a value"
            prefix=$2
            shift 2
            ;;
        --prefix=*)
            prefix=${1#*=}
            shift
            ;;
        --destdir)
            [ "$#" -ge 2 ] || die "--destdir needs a value"
            destdir=$2
            shift 2
            ;;
        --destdir=*)
            destdir=${1#*=}
            shift
            ;;
        --uninstall)
            mode=uninstall
            shift
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument: $1"
            ;;
    esac
done

case "$prefix" in
    /*) ;;
    *) die "prefix must be absolute: $prefix" ;;
esac
destdir=${destdir%/}
root=$destdir$prefix
lib=$root/lib/permute
bindir=$root/bin
manifest=$lib/install-manifest.txt

# Single-quote a value for the generated launcher.
sh_quote() {
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

say() {
    if [ "$dry_run" -eq 1 ]; then
        echo "would $*"
    else
        echo "$*"
    fi
}

do_mkdir() {
    [ "$dry_run" -eq 1 ] || mkdir -p "$1"
}

do_copy() {
    # $1 source, $2 destination, $3 mode
    if [ "$dry_run" -eq 1 ]; then
        echo "would install $2"
    else
        cp "$1" "$2"
        chmod "$3" "$2"
    fi
}

record() {
    [ "$dry_run" -eq 1 ] || printf '%s\n' "$1" >>"$manifest"
}

# ---------------------------------------------------------------------------
# uninstall

if [ "$mode" = uninstall ]; then
    if [ ! -f "$manifest" ]; then
        echo "install.sh: nothing installed at $root (no manifest)"
        exit 0
    fi

    missing=0
    while IFS= read -r rel; do
        case "$rel" in
            ""|\#*) continue ;;
            /*|..|../*|*/../*) die "refusing unsafe manifest path: $rel" ;;
        esac
        target=$root/$rel
        if [ -e "$target" ] || [ -L "$target" ]; then
            if [ "$dry_run" -eq 1 ]; then
                echo "would remove $target"
            else
                rm -f "$target"
            fi
        else
            missing=$((missing + 1))
        fi
    done <"$manifest"

    if [ "$dry_run" -eq 1 ]; then
        echo "would remove $manifest"
        echo "would prune empty directories under $lib"
        exit 0
    fi

    rm -f "$manifest"
    # Runtime bytecode is suppressed by the launcher; clean any tree that
    # predates this install or was produced by an older launcher.
    find "$lib" -type f -name '*.pyc' -delete 2>/dev/null || true
    find "$lib" -depth -type d -exec rmdir {} \; 2>/dev/null || true
    rmdir "$lib" 2>/dev/null || true
    rmdir "$root/lib" 2>/dev/null || true
    rmdir "$bindir" 2>/dev/null || true
    rmdir "$root" 2>/dev/null || true

    if [ "$missing" -gt 0 ]; then
        echo "install.sh: removed the installation ($missing file(s) already gone)"
    else
        echo "install.sh: removed $prefix (zero residue under $root)"
    fi
    exit 0
fi

# ---------------------------------------------------------------------------
# install

for required in cli/permute/__main__.py abi/permute_iface.def \
                guest/permute_guest.ld tools/make_guest.py \
                build/libpermute_guest.a build/permute-run \
                build/permute-worker; do
    [ -e "$required" ] || die "missing $required (run make first)"
done

sha=unknown
if [ -d .git ]; then
    sha=$(git rev-parse --short HEAD 2>/dev/null) || sha=unknown
fi
if [ "$sha" = unknown ] && [ -f permute-version.txt ]; then
    # A source tarball has no .git; git archive substituted $Format:%h$.
    sha=$(sed -n '/^#/d; /^[[:space:]]*$/d; p; q' permute-version.txt)
fi
case "$sha" in
    *Format*) sha=unknown ;;
esac
[ -n "$sha" ] || sha=unknown

do_mkdir "$lib"
do_mkdir "$bindir"

if [ "$dry_run" -eq 0 ]; then
    : >"$manifest"
    printf '# permute %s install manifest\n' "$sha" >>"$manifest"
fi

# Python package. Every module the CLI imports must be present; __pycache__
# is excluded because it is regenerated and would break residue checks.
for source in $(find cli/permute -name '*.py' -type f | sort); do
    rel=$source
    do_mkdir "$lib/$(dirname "$rel")"
    record "lib/permute/$rel"
    do_copy "$source" "$lib/$rel" 0644
done

# ABI surface and linker script: `permute build` compiles guests with
# -I $PERMUTE_HOME and resolves abi/permute_iface.h and guest/*.ld.
for source in abi/*.h abi/permute_iface.def guest/permute_guest.ld; do
    rel=$source
    do_mkdir "$lib/$(dirname "$rel")"
    record "lib/permute/$rel"
    do_copy "$source" "$lib/$rel" 0644
done

# Host binaries and the guest archive.
for source in build/libpermute_guest.a build/permute-run build/permute-worker; do
    rel=${source#build/}
    do_mkdir "$lib/build"
    record "lib/permute/build/$rel"
    do_copy "$source" "$lib/build/$rel" 0755
done

# Tools the CLI shells out to.
for source in tools/make_guest.py tools/symcheck.py tools/header_gap.py; do
    rel=${source#tools/}
    do_mkdir "$lib/tools"
    record "lib/permute/tools/$rel"
    do_copy "$source" "$lib/tools/$rel" 0644
done

# Licence and reader documents.
for source in LICENSE NOTICE README.md QUICKSTART.md CHANGELOG.md; do
    [ -f "$source" ] || continue
    do_mkdir "$lib/doc"
    record "lib/permute/doc/$source"
    do_copy "$source" "$lib/doc/$source" 0644
done

launcher=$bindir/permute
if [ "$dry_run" -eq 1 ]; then
    echo "would write $launcher"
else
    {
        echo '#!/bin/sh'
        echo '# Generated by install.sh; removed by make uninstall.'
        echo "PERMUTE_HOME=$(sh_quote "$prefix/lib/permute")"
        echo "PERMUTE_GIT_SHA=$(sh_quote "$sha")"
        echo 'PYTHONDONTWRITEBYTECODE=1'
        echo 'PYTHONPATH="$PERMUTE_HOME${PYTHONPATH:+:$PYTHONPATH}"'
        echo 'export PERMUTE_HOME PERMUTE_GIT_SHA PYTHONDONTWRITEBYTECODE PYTHONPATH'
        echo 'exec python3 -m cli.permute "$@"'
    } >"$launcher"
    chmod 0755 "$launcher"
fi
record "bin/permute"

say "installed permute $sha to $prefix"
say "run: $bindir/permute --version"

#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Headless QNX SDP 8.0 bootstrap for a Linux host.
#
# Follows the QNX "Unattended installation on Linux or macOS" guide plus the
# QSTI-for-QEMU package. The myQNX password is read from the terminal, passed
# only to qnxsoftwarecenter_clt (which requires it on the command line), and
# never written to disk by this script.
#
# Prerequisite, done in a browser by the account holder:
#   1. create a myQNX account
#   2. request the free non-commercial SDP 8.0 licence
#   3. deploy it in myQNX License Manager (Status must read "Deployed")
#
# Usage:  tools/qnx_bootstrap.sh
# Result: ~/qnxinstall/qnxsoftwarecenter, ~/qnx800 with qcc and the QEMU image.

set -euo pipefail

QSC_ID=79441
QSC_FILE=qnx-setup-2.0.4-202501021438-linux.run
QSC_DIR=$HOME/qnxinstall
SDP_DIR=$HOME/qnx800
QSC=$QSC_DIR/qnxsoftwarecenter/qnxsoftwarecenter_clt

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
note() { printf '\n== %s\n' "$*"; }

[ "$(uname -s)" = Linux ] || die "this script targets Linux hosts"

# Downloads are ~5-7 GB: QSC installer ~0.4 GB, SDP 8.0 ~3-4 GB, QEMU image
# ~1-2 GB. They are kept and expanded under the same tree, so reserve 12 GB.
free_gb=$(( $(df -Pk "$HOME" | awk 'NR==2 {print $4}') / 1048576 ))
[ "$free_gb" -ge 12 ] || die "need ~12 GB free under $HOME, found ${free_gb} GB"
echo "free space under HOME: ${free_gb} GB (downloads are ~5-7 GB)"
[ "$free_gb" -ge 20 ] || echo "note: under 20 GB free; the SDP tree and the boot image share this space"

mkdir -p "$HOME/.qnx"
read -r -p "myQNX email: " MYQNX_USER
read -r -s -p "myQNX password: " MYQNX_PASSWORD
echo

note "signing in"
curl -fsS --cookie-jar "$HOME/.qnx/myqnxcookies.auth" \
  --form "userlogin=$MYQNX_USER" \
  --form "password=$MYQNX_PASSWORD" \
  -X POST https://www.qnx.com/account/login.html \
  -o "$HOME/.qnx/login.html" || die "login request failed"
grep -qi 'logout' "$HOME/.qnx/login.html" \
  || echo "warning: could not confirm the login page; continuing"

note "downloading QNX Software Center 2.0.4"
if [ ! -f "$HOME/$QSC_FILE" ]; then
  curl -fL --cookie "$HOME/.qnx/myqnxcookies.auth" \
    "https://www.qnx.com/download/download/$QSC_ID/$QSC_FILE" \
    -o "$HOME/$QSC_FILE" \
    || die "download failed: is the licence deployed to $MYQNX_USER?"
  chmod +x "$HOME/$QSC_FILE"
fi
# The installer is a Makeself self-extracting archive: a shell script with an
# embedded payload, not an ELF binary. HTML from an expired session starts with
# '<', so a '#!' shebang is the reliable test.
magic=$(head -c 2 "$HOME/$QSC_FILE")
[ "$magic" = '#!' ] || die "downloaded file is not a Makeself installer; sign in again"

note "installing QNX Software Center (accepts the QNX EULA)"
if [ -x "$QSC" ]; then
  echo "already installed at $QSC"
else
  "$HOME/$QSC_FILE" force-override disable-auto-start agree-to-license-terms "$QSC_DIR"
fi

note "activating the deployed licence"
echo "note: qnxsoftwarecenter_clt takes the password as an argument, so it is"
echo "      briefly visible in this machine's process list while it runs."
"$QSC" -syncLicenseKeys \
  -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD" \
  -activateAll -listLicenseKeys

note "installing SDP 8.0 into $SDP_DIR (several GB)"
if [ -f "$SDP_DIR/qnxsdp-env.sh" ]; then
  echo "already installed at $SDP_DIR; skipping the clean install"
else
  "$QSC" -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD" \
    -mirrorBaseline qnx800 \
    -cleanInstall -destination "$SDP_DIR" \
    -installBaseline com.qnx.qnx800
fi

note "installing the QSTI for QEMU"
if ! "$QSC" -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD" \
  -destination "$SDP_DIR" \
  -installPackage com.qnx.qnx800.quickstart.qemu; then
  echo
  echo "package install failed. The accessible quickstart packages are:"
  "$QSC" -myqnx.user="$MYQNX_USER" -myqnx.password="$MYQNX_PASSWORD" \
    -listAccessible 2>/dev/null | grep -i quickstart || true
  die "retry with -installPackage <versioned id> from the list above"
fi

note "unpacking the QEMU image"
if [ -d "$SDP_DIR/images/qemu/qemu" ]; then
  echo "already unpacked"
else
  ( cd "$SDP_DIR/images/qemu" && ./unpack_qemu_image.sh )
fi

note "verifying the toolchain"
set +u
# shellcheck disable=SC1091
. "$SDP_DIR/qnxsdp-env.sh"
set -u
qcc -Vgcc_ntox86_64 --version
ls "$SDP_DIR/images/qemu/qemu"

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  note "qemu-system-x86_64 is missing"
  if command -v apt-get >/dev/null 2>&1; then
    echo "install it with: sudo apt-get install -y qemu-system-x86"
  elif command -v pacman >/dev/null 2>&1; then
    echo "install it with: sudo pacman -S --needed qemu-full"
  fi
fi

cat <<EOF

done.
  toolchain:  source $SDP_DIR/qnxsdp-env.sh
  cross-compile: qcc -Vgcc_ntox86_64 -o hello hello.c
  boot QNX:   cd $SDP_DIR/images/qemu && mkqnximage --run

Record these for docs/QNX-EVIDENCE.md:
  uname -a, the image SHA-256, the mkqnximage invocation, and a serial excerpt.
EOF

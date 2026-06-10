#!/usr/bin/env bash
# Build a static apt + dnf repository tree (for GitHub Pages) from a
# directory of .deb and .rpm packages.
#
#   build-pages.sh <input-pkg-dir> <output-public-dir>
#
# Environment:
#   PAGES_BASE_URL   Public URL the tree will be served from
#                    (default: https://thecleaners.github.io/qiftop).
#   GPG_KEY_ID       If set, sign the apt Release (-> InRelease + Release.gpg)
#                    and the dnf repomd.xml (-> repomd.xml.asc). The key must
#                    already be importable by the active gpg keyring
#                    (GNUPGHOME). If empty, the repo is published UNSIGNED.
#
# Trust model (matches what apt/dnf actually verify):
#   * apt  — the signed `Release` file carries SHA256 of `Packages`, which
#            carries SHA256 of every .deb. Signing Release transitively
#            authenticates the packages; individual .debs are not signed
#            (this is the standard apt model).
#   * dnf  — `repo_gpgcheck=1` verifies `repomd.xml.asc`; repomd carries the
#            checksum of `primary.xml`, which carries the checksum of every
#            .rpm. So a signed repomd transitively authenticates the
#            packages, exactly like apt's signed Release. Package-level
#            `gpgcheck` (rpm --addsign) is a separate, additive step left as
#            a post-stable TODO; we ship `gpgcheck=0 repo_gpgcheck=1`.
set -euo pipefail

IN_DIR="${1:?usage: build-pages.sh <input-pkg-dir> <output-public-dir>}"
OUT_DIR="${2:?usage: build-pages.sh <input-pkg-dir> <output-public-dir>}"
BASE_URL="${PAGES_BASE_URL:-https://thecleaners.github.io/qiftop}"
KEY_ID="${GPG_KEY_ID:-}"

command -v apt-ftparchive >/dev/null || { echo "need apt-ftparchive (apt-utils)" >&2; exit 1; }
command -v createrepo_c   >/dev/null || { echo "need createrepo_c" >&2; exit 1; }

IN_DIR="$(cd "$IN_DIR" && pwd)"
mkdir -p "$OUT_DIR"
OUT_DIR="$(cd "$OUT_DIR" && pwd)"

echo "==> output:   $OUT_DIR"
echo "==> base url: $BASE_URL"
echo "==> signing:  ${KEY_ID:-<unsigned>}"

# Public key (committed at dist/repo/qiftop-archive-keyring.asc) goes at the
# tree root so the .repo file + apt signed-by can fetch it.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "$SCRIPT_DIR/qiftop-archive-keyring.asc" ]]; then
    cp "$SCRIPT_DIR/qiftop-archive-keyring.asc" "$OUT_DIR/qiftop-archive-keyring.asc"
    # dearmored copy for apt's signed-by= (apt accepts either, but a .gpg
    # keyring is the conventional drop-in for /usr/share/keyrings).
    gpg --dearmor < "$SCRIPT_DIR/qiftop-archive-keyring.asc" \
        > "$OUT_DIR/qiftop-archive-keyring.gpg" 2>/dev/null || true
fi

# ----------------------------------------------------------------------------
# APT repository:  deb/  ->  dists/stable/main/binary-amd64 + pool/
# ----------------------------------------------------------------------------
echo "==> building apt repo"
APT="$OUT_DIR/deb"
POOL="$APT/pool/main/q/qiftop"
BINDIR="$APT/dists/stable/main/binary-amd64"
RELDIR="$APT/dists/stable"
rm -rf "$APT"
mkdir -p "$POOL" "$BINDIR"

shopt -s nullglob
debs=("$IN_DIR"/*.deb)
shopt -u nullglob
[[ ${#debs[@]} -gt 0 ]] || { echo "no .deb files in $IN_DIR" >&2; exit 1; }
cp "${debs[@]}" "$POOL/"

# Packages index (Filename is relative to the apt root = $APT).
( cd "$APT" && apt-ftparchive packages pool > dists/stable/main/binary-amd64/Packages )
gzip -9c "$BINDIR/Packages" > "$BINDIR/Packages.gz"

relconf="$(mktemp)"
cat > "$relconf" <<EOF
APT::FTPArchive::Release::Origin "qiftop";
APT::FTPArchive::Release::Label "qiftop";
APT::FTPArchive::Release::Suite "stable";
APT::FTPArchive::Release::Codename "stable";
APT::FTPArchive::Release::Architectures "amd64";
APT::FTPArchive::Release::Components "main";
APT::FTPArchive::Release::Description "qiftop apt repository";
EOF
( cd "$RELDIR" && apt-ftparchive -c="$relconf" release . > Release )
rm -f "$relconf"

if [[ -n "$KEY_ID" ]]; then
    ( cd "$RELDIR" \
        && gpg --batch --yes --default-key "$KEY_ID" -abs -o Release.gpg Release \
        && gpg --batch --yes --default-key "$KEY_ID" --clearsign -o InRelease Release )
fi

# ----------------------------------------------------------------------------
# DNF repository:  rpm/  ->  flat rpms + repodata/
# ----------------------------------------------------------------------------
echo "==> building dnf repo"
RPM="$OUT_DIR/rpm"
rm -rf "$RPM"
mkdir -p "$RPM"

shopt -s nullglob
rpms=("$IN_DIR"/*.rpm)
shopt -u nullglob
[[ ${#rpms[@]} -gt 0 ]] || { echo "no .rpm files in $IN_DIR" >&2; exit 1; }
cp "${rpms[@]}" "$RPM/"

createrepo_c --quiet "$RPM"
if [[ -n "$KEY_ID" ]]; then
    gpg --batch --yes --default-key "$KEY_ID" --detach-sign --armor "$RPM/repodata/repomd.xml"
fi

# Drop-in .repo file for /etc/yum.repos.d/. repo_gpgcheck verifies the signed
# repomd; gpgcheck (per-package) is off because the rpms aren't individually
# signed yet (the signed repomd already authenticates them via checksums).
repo_gpgcheck=0
[[ -n "$KEY_ID" ]] && repo_gpgcheck=1
cat > "$RPM/qiftop.repo" <<EOF
[qiftop]
name=qiftop
baseurl=$BASE_URL/rpm
enabled=1
gpgcheck=0
repo_gpgcheck=$repo_gpgcheck
gpgkey=$BASE_URL/qiftop-archive-keyring.asc
EOF

# ----------------------------------------------------------------------------
# Landing page
# ----------------------------------------------------------------------------
cat > "$OUT_DIR/index.html" <<EOF
<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>qiftop package repository</title>
<style>body{font-family:system-ui,sans-serif;max-width:48rem;margin:2rem auto;
padding:0 1rem;line-height:1.5}code,pre{background:#f4f4f4;border-radius:4px}
pre{padding:.8rem;overflow-x:auto}code{padding:.1rem .3rem}h2{margin-top:2rem}</style>
</head><body>
<h1>qiftop package repository</h1>
<p>APT (Debian/Ubuntu) and DNF (Fedora) repositories for
<a href="https://github.com/TheCleaners/qiftop">qiftop</a>, a Qt6 iftop-style
network monitor. Signed with GPG key
<code>${KEY_ID:-unsigned}</code>.</p>
<h2>Debian / Ubuntu (apt)</h2>
<pre>curl -fsSL $BASE_URL/qiftop-archive-keyring.asc \\
  | sudo gpg --dearmor -o /usr/share/keyrings/qiftop-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/qiftop-archive-keyring.gpg] $BASE_URL/deb stable main" \\
  | sudo tee /etc/apt/sources.list.d/qiftop.list
sudo apt update &amp;&amp; sudo apt install qiftop qiftop-agent</pre>
<h2>Fedora (dnf)</h2>
<pre>sudo curl -fsSL $BASE_URL/rpm/qiftop.repo -o /etc/yum.repos.d/qiftop.repo
sudo rpm --import $BASE_URL/qiftop-archive-keyring.asc
sudo dnf install qiftop qiftop-agent</pre>
<p>Or grab a <code>.deb</code>/<code>.rpm</code> directly from the
<a href="https://github.com/TheCleaners/qiftop/releases">releases page</a>.</p>
</body></html>
EOF

echo "==> done. tree:"
find "$OUT_DIR" -maxdepth 3 -type f | sort | sed "s#$OUT_DIR#  .#"

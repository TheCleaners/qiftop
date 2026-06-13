# Fedora COPR spec for qiftop.
# This is a second, source-RPM packaging path kept in sync with the CPack RPM
# package names: qiftop, qiftop-agent, qiftop-libs, qiftop-devel, nqiftop,
# and qiftop-monitoring-plugin. Keep every %files list in sync with CMake
# install() path changes; COPR builds an SRPM from this spec plus Source0.
#
# Verified in a fedora:44 container: rpmbuild -ba builds the SRPM + all six
# subpackages (no unpackaged/missing-file errors), dnf installs them, and every
# artifact lands (binaries, libqiftop.so, freedesktop .desktop + AppStream
# metainfo + icons, systemd unit, shell completions, netdev group). rpmlint is
# clean apart from spelling false-positives (ncurses/Nagios/libqiftop/perfdata).

Name:           qiftop
Version:        0.3.0
Release:        1%{?dist}
Summary:        Qt6 iftop-style network monitor

License:        GPL-2.0-or-later
URL:            https://github.com/TheCleaners/qiftop
Source0:        https://github.com/TheCleaners/qiftop/archive/refs/tags/v%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  systemd-rpm-macros
BuildRequires:  qt6-qtbase-devel
BuildRequires:  libnl3-devel
BuildRequires:  libnetfilter_conntrack-devel
BuildRequires:  ncurses-devel

Requires:       qiftop-libs%{?_isa} = %{version}-%{release}
Recommends:     qiftop-agent = %{version}-%{release}

%description
qiftop is a Qt 6 desktop application that displays live per-interface
traffic counters and per-connection flows. It cooperates with the
qiftop-agent system service for privileged data access but can also
fall back to running with elevated privileges on demand.


%package agent
Summary:        Privileged DBus agent for qiftop
Requires:       qiftop-libs%{?_isa} = %{version}-%{release}
Requires:       dbus
Requires(post): shadow-utils
Requires(post): systemd
Requires(preun): systemd
Requires(postun): systemd
Recommends:     nftables
%{?systemd_requires}

%description agent
qiftop-agent is a small system-bus daemon that publishes per-interface
and per-connection network statistics on the well-known DBus name
org.qiftop.NetworkAgent1. It is activated on demand and runs as root
with a bounded capability set.


%package -n qiftop-libs
Summary:        Qt6 network-monitoring data library (runtime)

%description -n qiftop-libs
libqiftop carries the qiftop wire DTOs, the DBus client proxies for the
qiftop-agent, the per-interface and per-connection aggregators, the filter
mini-language, IEC unit formatters, and JSON/CSV export. It has no Qt
Widgets dependency, so it runs headless.


%package -n qiftop-devel
Summary:        Development files for qiftop-libs
Requires:       qiftop-libs%{?_isa} = %{version}-%{release}
Requires:       qt6-qtbase-devel

%description -n qiftop-devel
Public headers, the CMake package config (find_package(qiftop) ->
qiftop::qiftop), and the pkg-config file for building against libqiftop.


%package -n nqiftop
Summary:        ncurses network monitor (qiftop, headless)
Requires:       qiftop-libs%{?_isa} = %{version}-%{release}
Recommends:     qiftop-agent = %{version}-%{release}

%description -n nqiftop
nqiftop is a terminal UI for live per-interface and per-connection traffic,
built on libqiftop. It streams from the qiftop-agent over DBus, or captures
in-process when run as root; no X/Wayland is needed, so it works over SSH.


%package -n qiftop-monitoring-plugin
Summary:        Nagios/Icinga/Zabbix monitoring plugin for qiftop
Requires:       qiftop-libs%{?_isa} = %{version}-%{release}
Recommends:     qiftop-agent = %{version}-%{release}

%description -n qiftop-monitoring-plugin
check_qiftop is a one-shot threshold checker built on libqiftop. It streams
from qiftop-agent over DBus, waits for enough samples to compute rates, then
emits standard Nagios plugin status and perfdata.


%prep
%autosetup -n %{name}-%{version}


%build
%cmake \
    -DQIFTOP_BUILD_TESTS=OFF \
    -DQIFTOP_AUTO_PACKAGE=OFF
%cmake_build


%install
%cmake_install


%post agent
# Ensure the netdev group exists; the DBus policy gates agent access on it.
if ! getent group netdev >/dev/null 2>&1; then
    groupadd -r netdev || :
fi

# Best-effort enrollment for installs launched via sudo or pkexec.
target_user=""
if [ -n "${SUDO_USER:-}" ] && [ "${SUDO_USER}" != "root" ]; then
    target_user="${SUDO_USER}"
elif [ -n "${PKEXEC_UID:-}" ]; then
    target_user="$(getent passwd "${PKEXEC_UID}" 2>/dev/null | cut -d: -f1 || :)"
fi
if [ -n "${target_user}" ] && getent passwd "${target_user}" >/dev/null 2>&1; then
    if ! id -nG "${target_user}" 2>/dev/null | tr ' ' '\n' | grep -qx netdev; then
        usermod -a -G netdev "${target_user}" || :
        echo "qiftop-agent: added user '${target_user}' to group 'netdev'." >&2
        echo "qiftop-agent: log out and back in (or run 'newgrp netdev') for it to take effect." >&2
    fi
fi
%systemd_post qiftop-agent.service

%preun agent
%systemd_preun qiftop-agent.service

%postun agent
%systemd_postun_with_restart qiftop-agent.service

%post -n qiftop-libs -p /sbin/ldconfig
%postun -n qiftop-libs -p /sbin/ldconfig


%files
%{_bindir}/qiftop
%{_mandir}/man1/qiftop.1*
%{_datadir}/applications/io.github.thecleaners.qiftop.desktop
%{_metainfodir}/io.github.thecleaners.qiftop.metainfo.xml
%{_datadir}/icons/hicolor/scalable/apps/io.github.thecleaners.qiftop.svg
%{_datadir}/icons/hicolor/*/apps/io.github.thecleaners.qiftop.png
%{_datadir}/bash-completion/completions/qiftop
%{_datadir}/zsh/site-functions/_qiftop
%{_datadir}/fish/vendor_completions.d/qiftop.fish
%doc %{_docdir}/qiftop/README.md
%license %{_docdir}/qiftop/LICENSE

%files agent
%{_bindir}/qiftop-agent
%{_mandir}/man5/qiftop-agent.conf.5*
%{_mandir}/man8/qiftop-agent.8*
%{_datadir}/dbus-1/system.d/org.qiftop.NetworkAgent1.conf
%{_datadir}/dbus-1/system-services/org.qiftop.NetworkAgent1.service
/lib/systemd/system/qiftop-agent.service
%{_datadir}/bash-completion/completions/qiftop-agent
%{_datadir}/zsh/site-functions/_qiftop-agent
%{_datadir}/fish/vendor_completions.d/qiftop-agent.fish
%dir %{_sysconfdir}/qiftop
%config(noreplace) %{_sysconfdir}/qiftop/agent.conf
%dir %{_datadir}/qiftop
%{_datadir}/qiftop/qiftop-conntrack.nft
%license %{_docdir}/qiftop-agent/LICENSE

%files -n qiftop-libs
%license LICENSE
%{_libdir}/libqiftop.so.0*

%files -n qiftop-devel
%license LICENSE
%{_includedir}/qiftop/
%{_libdir}/libqiftop.so
%{_libdir}/cmake/qiftop/
%{_libdir}/pkgconfig/qiftop.pc

%files -n nqiftop
%license LICENSE
%{_bindir}/nqiftop
%{_mandir}/man1/nqiftop.1*
%{_datadir}/applications/io.github.thecleaners.qiftop.nqiftop.desktop
%{_datadir}/bash-completion/completions/nqiftop
%{_datadir}/zsh/site-functions/_nqiftop
%{_datadir}/fish/vendor_completions.d/nqiftop.fish

%files -n qiftop-monitoring-plugin
%license LICENSE
%dir %{_libexecdir}/qiftop
%{_libexecdir}/qiftop/check_qiftop
%{_mandir}/man1/check_qiftop.1*
%{_datadir}/bash-completion/completions/check_qiftop
%{_datadir}/zsh/site-functions/_check_qiftop
%{_datadir}/fish/vendor_completions.d/check_qiftop.fish


%changelog
* Fri Jun 12 2026 qiftop maintainers <noreply@example.com> - 0.3.0-1
- BSD client support (FreeBSD/NetBSD), attribution reason taxonomy
  (forwarded/orphaned/nosocket, contract Version 0.6), v4-mapped IPv6
  attribution fix, root-owned-config fix, new-client/old-agent wire
  tolerance, and nqiftop UX (group collapse, Ctrl-F/Ctrl-B, W export).
* Wed Jun 11 2026 qiftop maintainers <noreply@example.com> - 0.2.5-1
- Performance: aggregator signal coalescing, single-pass netns scan,
  delegate allocation reuse, bounded LRU route cache.
* Wed Jun 11 2026 qiftop maintainers <noreply@example.com> - 0.2.4-1
- Rounded-out distribution: freedesktop metainfo + launchers, shell
  completions, broader-distro packaging recipes.
* Wed Jun 10 2026 qiftop maintainers <noreply@example.com> - 0.2.3-1
- Add Fedora COPR source-RPM packaging path.

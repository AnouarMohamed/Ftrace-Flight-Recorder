Summary:        Flight recorder for Linux kernel trace events
Name:           fdr
Version:        1.4.0
Release:        1%{?dist}
License:        UPL-1.0
Source0:        %{name}-%{version}.tar.xz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  curl
BuildRequires:  systemd-rpm-macros
Requires:       kmod
Requires:       logrotate
%{?systemd_requires}

%description
FDR creates isolated ftrace instances, enables configured kernel tracepoints,
and continuously saves their events with disk-space protection, rotation,
health checks, and Prometheus metrics.

%prep
%autosetup

%build
%make_build VERSION=%{version} CFLAGS="%{optflags}"

%check
%make_build check VERSION=%{version} CFLAGS="%{optflags}"

%install
%make_install VERSION=%{version} PREFIX=%{_prefix} \
    UNITDIR=%{_unitdir} SYSCONFDIR=%{_sysconfdir}

%post
%systemd_post fdr.service

%preun
%systemd_preun fdr.service

%postun
%systemd_postun_with_restart fdr.service

%files
%{_sbindir}/fdrd
%{_unitdir}/fdr.service
%dir %{_sysconfdir}/fdr.d
%{_datadir}/fdr
%{_mandir}/man8/fdrd.8*
%doc README.md CONTRIBUTING.md SECURITY.md
%license LICENSE.txt

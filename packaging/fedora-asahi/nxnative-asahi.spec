%global _lto_cflags %{nil}
# Draft Fedora Asahi Remix packaging. Not a release artifact yet.
Name:           nxnative-asahi
Version:        0.1.0
Release:        1%{?dist}
Summary:        Native Nintendo Switch runtime for Asahi Linux
License:        MIT
URL:            https://github.com/seedds/nxnative
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  cmake
BuildRequires:  ninja-build
BuildRequires:  gcc
BuildRequires:  vulkan-loader-devel
BuildRequires:  vulkan-headers
Requires:       vulkan-loader
Requires:       mesa-vulkan-drivers
Requires:       sdl3
%description
nxnative-asahi runs a Nintendo Switch game's own AArch64 code on Apple Silicon
under Asahi Linux. It answers Horizon's kernel SVCs and services and renders
with Vulkan through Mesa's Honeykrisp driver. Users must supply their own game
files and keys; no Nintendo material is included.
%prep
%setup -q
%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXNATIVE_BUILD_TESTS=OFF -DNXNATIVE_BUILD_HOST=%{?game:ON}%{!?game:OFF} -DNXNATIVE_GAME=%{?game}
%cmake_build
%install
%cmake_install
%files
%{_bindir}/nxnative-asahi
%{_bindir}/nxnative-launcher
%{_datadir}/applications/nxnative-asahi.desktop
%{_docdir}/nxnative-asahi
%changelog
* Mon Nov 16 2026 nxnative-asahi contributors - 0.1.0-1
- Initial Asahi Linux foundation: native execution core, 16 KiB memory model,
  native Vulkan device and Vulkan guest API layer.

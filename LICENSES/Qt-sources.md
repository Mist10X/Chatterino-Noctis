# Qt runtime (Windows 6.9.3)

The Windows Noctis packages contain dynamically linked Qt 6.9.3 libraries and plugins from Qt's official distribution. Qt is not part of Noctis's own MIT license. Qt Base, Qt SVG and Qt Image Formats are used under LGPL-3.0, subject to the respective component and third-party license notices. The LGPL terms and the referenced GPLv3 terms are included in this distribution. Qt Charts is not used or bundled by this build.

The corresponding sources are available at no charge in the release asset `Qt-6.9.3-corresponding-source.zip`, alongside the Windows installer and portable binaries. It contains the following original upstream archives; their SHA-256 hashes are verified against Qt's published metadata:

- Qt Base: https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtbase-everywhere-src-6.9.3.tar.xz — SHA-256 `c5a1a2f660356ec081febfa782998ae5ddbc5925117e64f50e4be9cd45b8dc6e`
- Qt SVG: https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtsvg-everywhere-src-6.9.3.tar.xz — SHA-256 `db76aa3358cbbe6fce7da576ff4669cb9801920188c750d3b12783bbe97026e2`
- Qt Image Formats: https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtimageformats-everywhere-src-6.9.3.tar.xz — SHA-256 `4fb26bdbfbd4b8e480087896514e11c33aba7b6b39246547355ea340c4572ffe`

Qt source and third-party license information: https://doc.qt.io/qt-6.9/licensing.html . These modules are used without Noctis-specific modifications. The upstream source archives include their build scripts, license notices and bundled third-party source. Other separately distributed dependencies retain their own licenses.

The Qt runtime is not statically incorporated into Noctis. You may modify the Qt libraries, reverse engineer the application to debug such modifications, and replace the Qt DLLs and plugins with compatible builds. Close the application, back up its runtime files, and replace the matching Qt 6.9.3 files in the application directory with your compatible build; the application does not check their signatures. The Noctis source and build instructions are available in this repository.

If the Windows Qt runtime version changes, update this document and review the exact module/third-party source and license inventory before distribution.

# Qt runtime (Windows 6.9.3)

The Windows Noctis packages contain dynamically linked Qt 6.9.3 libraries and plugins from Qt's official distribution. Qt is not part of Noctis's own MIT license. Qt Base, Qt SVG and Qt Image Formats are used under LGPL-3.0, subject to the respective component and third-party license notices. The LGPL terms and the referenced GPLv3 terms are included in this distribution. Qt Charts is not used or bundled by this build.

Corresponding upstream source archives for the included Qt modules:

- Qt Base: https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtbase-everywhere-src-6.9.3.tar.xz
- Qt SVG: https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtsvg-everywhere-src-6.9.3.tar.xz
- Qt Image Formats: https://download.qt.io/archive/qt/6.9/6.9.3/submodules/qtimageformats-everywhere-src-6.9.3.tar.xz

Qt source and third-party license information: https://doc.qt.io/qt-6.9/licensing.html . These modules are used without Noctis-specific modifications. The upstream source archives include their build scripts, license notices and bundled third-party source. Other separately distributed dependencies retain their own licenses.

The Qt runtime is not statically incorporated into Noctis. You may modify the Qt libraries, reverse engineer the application to debug such modifications, and replace the Qt DLLs and plugins with compatible builds. Close the application, back up its runtime files, and replace the matching Qt 6.9.3 files in the application directory with your compatible build; the application does not check their signatures. The Noctis source and build instructions are available in this repository.

If the Windows Qt runtime version changes, update this document and review the exact module/third-party source and license inventory before distribution.

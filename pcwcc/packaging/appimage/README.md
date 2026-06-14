# AppImage packaging

Directory for Linux `AppImage` packaging assets for the desktop client.

## How it is used
[`../../../scripts/build_appimage.sh`](../../../scripts/build_appimage.sh):
- creates a dedicated build virtualenv;
- installs `pcwcc` dependencies;
- builds the application with `PyInstaller`;
- creates the `AppDir`;
- bundles `OpenSSL 3`;
- downloads `appimagetool` and produces the final `.AppImage` in `dist/`.

## Directory contents
This directory can store Linux package-specific assets:
- a `${APP_ID}.png` icon for the desktop entry;
- additional manifests or templates if packaging grows.

If the icon is missing, the build script injects a minimal PNG automatically so the release build does not fail on an empty asset directory.

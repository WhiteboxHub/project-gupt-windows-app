# Cross-Platform Testing

## Built On This Windows Machine

Existing native Windows app:

```text
build/Release/Gupt.exe
```

New Tauri shell Windows build:

```text
CrossPlatform/apps/gupt-desktop/src-tauri/target/release/gupt-desktop.exe
CrossPlatform/apps/gupt-desktop/src-tauri/target/release/bundle/nsis/Gupt_0.1.0_x64-setup.exe
CrossPlatform/apps/gupt-desktop/src-tauri/target/release/bundle/msi/Gupt_0.1.0_x64_en-US.msi
```

## What Can Be Tested Now

### Windows Host -> Windows Client

Use the existing native app on both machines:

```text
build/Release/Gupt.exe
```

### Windows Host -> macOS Client Preview

Build/run the Tauri shell on macOS, then use Client mode with the Windows host
session ID.

The frontend speaks the existing Gupt relay protocol and can send mouse/keyboard
events back to the Windows host. The Windows host performs the actual input
injection.

macOS build command:

```bash
cd CrossPlatform/apps/gupt-desktop
npm install
npm run tauri -- build
```

Expected macOS outputs:

```text
src-tauri/target/release/bundle/macos/Gupt.app
src-tauri/target/release/bundle/dmg/*.dmg
```

## What Still Needs Native macOS Work

### macOS Host -> Windows Client

Requires native macOS host implementation:

- ScreenCaptureKit capture
- Accessibility/Quartz input injection
- Screen Recording permission UX
- Accessibility permission UX

### macOS Host -> macOS Client

Requires the same macOS host implementation above. The viewer shell is already
scaffolded, but full macOS host control is not complete yet.

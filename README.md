# Dyslexia Reader — Desktop

A free, native desktop reading assistant that turns text captured from the screen into a customizable reading window, with local OCR and text-to-speech.

**Repository:** [DyslexiaApp](https://github.com/Dyslexia-Project/DyslexiaApp). The dashboard and backend are maintained in [DyslexiaPlatform](https://github.com/Dyslexia-Project/DyslexiaPlatform).

**Status:** the C++ reader scaffold, bundled fonts, live typography/theme controls, visual reading aids, offline Windows speech controls, bimodal playback highlighting, and a provisional cloud speech client/cache are implemented. The Windows-specific targets still require validation with the Windows SDK.

## Building the Windows prototype

Use Visual Studio's C++ desktop workload with a Windows 10 SDK, then run from
PowerShell or a Developer Command Prompt:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Debug
.\build\Debug\dyslexia_reader_demo.exe
```

The in-memory font loader uses `IDWriteFactory5` and requires Windows 10 build
15063 or later. The demo displays deterministic mock OCR content; it does not
perform screen capture or OCR.

The reader toolbar supports font family, font size, letter spacing, line height,
Bionic emphasis, the interactive Reading Ruler, semantic color themes, local
voice selection, bimodal highlighting, Play/Pause, Stop, and speech speed.
Local speech is synthesized off the UI thread; reliable Windows timed speech
cues drive sentence tint and DirectWrite-bound word outlines. The highlighting
control is independent of audio playback.
The WinRT API does not expose a `SpeechBookmarkReached` event, so the adapter
uses the documented word/sentence boundary metadata instead. Changes redraw the
existing mock-fed text without repeating OCR. Native Windows compilation and
visual behavior have not yet been validated on this Linux development host.
The cloud client requires an HTTPS service base URL and an injected optional
bearer-token provider. Its provisional request/response schema is
`docs/contracts/cloud-speech-v1.schema.json`; the linked Spring Boot repository
does not yet implement that endpoint, so server compatibility is unverified.
The cloud client/cache are not yet wired to a reader-toolbar opt-in or playback
path; cloud UI integration remains a follow-up.

Windows is the first target. macOS support is planned after the Windows workflow is validated.

## Free and independent

The desktop app must remain useful without an account, subscription, or connection to our platform. Screen capture, local OCR, reading customization, local speech, and local settings belong to the free app.

The platform provides settings synchronization, account management, and cloud services. A platform outage, expired subscription, or sign-out must leave local functionality available.

Free pricing and open-source licensing are separate decisions. The source license is still to be selected.

## Reading workflow

1. Run the app in the Windows system tray.
2. Press a configurable global keyboard shortcut.
3. Select a rectangular area of the screen.
4. Extract text locally using OCR.
5. Open the text in a movable, resizable reading window above the current application.
6. Adjust its appearance or listen using a local or explicitly selected cloud voice.
7. Close the reader and return focus to the original application.

The reader defaults to a non-modal floating window. A focus reading mode provides a full-screen reading surface for longer passages or captured book pages. Both modes display a reformatted copy of captured text; they do not change another application's fonts or document.

## Reading modes

- **Floating:** a movable, resizable reader above the current application, with other applications available for interaction.
- **Focus (blocking):** a full-screen reader on the selected display that covers the background and prevents clicks from passing through to applications underneath. Provide scrolling, reading customization, and speech controls for sustained reading.

Switch modes from the tray menu or reader controls, and choose the default in local settings. Keep a visible exit control and an Escape shortcut to leave focus mode and restore the previous reader position. Normal system shortcuts remain available; focus mode does not lock the desktop or prevent switching applications.

Book reading initially uses captured text. Importing complete books and automatic page turning are outside the first MVP.

## Reader toolbar

Both reading modes include a toolbar inside the reader, so text appearance and speech can be controlled without opening the main settings window.

Keep Play/Pause, Stop, speech speed, text size, and the appearance-panel button visible. Place detailed typography and color controls in an expandable panel to preserve reading space. Keep controls outside the text area, with labels, keyboard access, and visible focus.

| Tool group | Controls |
| --- | --- |
| Text appearance | Font family, size, weight, letter spacing, word spacing, line spacing, and line width. |
| Colors | Text and background colors, contrast presets, and reset to the active profile. |
| Speech playback | Play/Pause, Stop, speed, volume, voice selection, and a clear local/cloud provider label. |
| Reading selection | Read all text or a selected passage; copy selected text. |
| Profiles | Choose a saved reading profile or save the current appearance and voice preferences. |
| Window | Switch floating/focus mode and close or exit focus mode. |

Appearance changes update the existing text immediately without repeating capture or OCR, and must not restart speech. Session adjustments remain temporary until saved to a reading profile; saving follows the profile synchronization setting.

Speech starts when the user presses Play. If text is selected, play that selection; otherwise read the full passage. Show loading, playing, paused, and error states clearly, and allow Stop to cancel pending generation as well as playback. If the selected provider cannot resume paused playback, explain the behavior instead of silently restarting the passage.

Keep local voices available when disconnected. Selecting a cloud voice must make the text-sharing behavior clear before playback. Later reading aids can include sentence navigation and synchronized highlighting where the speech provider exposes usable timing information.

## Planned features

| Area | Behavior |
| --- | --- |
| Tray | Capture, show reader, switch floating/focus mode, pause/stop speech, settings, account status when connected, quit. |
| Capture | Configurable shortcut, selection cancellation, multiple monitors, mixed display scaling. |
| Local OCR | On-device text recognition, language selection, clear handling of missing language packs or unreadable captures. |
| Reader | Font family and size, letter/word/line spacing, text and background colors, line width, reusable reading profiles. |
| Reader toolbar | Live text appearance controls, voice selection, Play/Pause, Stop, speed, volume, and profile controls in both modes. |
| Focus reading | Full-screen blocking surface on the selected display, scrolling, speech controls, visible exit, Escape to leave. |
| Local speech | Available Windows voices, voice selection, speed, playback controls. |
| Accessibility | Keyboard operation, visible focus, accessible controls, high-contrast support. |
| Cloud integration | Account connection, reading-profile sync, subscription voices through the platform API. |

Reading preferences vary between people. Provide adjustable profiles and validate them with users who have dyslexia rather than assuming one font or color scheme suits everyone.

## Technology direction

Keep the desktop runtime and underlying engine in C++. Use native Windows integrations for the tray, hotkeys, capture, and reading window. The browser dashboard lives in the separate platform repository.

| Component | Proposed approach |
| --- | --- |
| Desktop shell and local settings | C++ with Win32 windows and controls. |
| Text layout and rendering | DirectWrite with Direct2D. |
| Tray and shortcut | Windows notification-area and global-hotkey APIs. |
| Capture | Native capture adapter; prototype and measure the capture path before selecting the final API. |
| OCR | Windows OCR through C++/WinRT for the first prototype. |
| Local speech | Windows speech synthesis through a provider adapter. |
| Preferences | Versioned local configuration; no hosted database required. |
| Platform connection | HTTPS/JSON client behind a separate integration interface. |

Windows exposes [tray icons](https://learn.microsoft.com/en-us/windows/win32/shell/notification-area), [global hotkeys](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerhotkey), and [DirectWrite text layout](https://learn.microsoft.com/en-us/windows/win32/directwrite/direct-write-portal) to native desktop applications.

The proposed Windows OCR and WinRT speech APIs require package identity according to Microsoft's [desktop API support documentation](https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/winrt-api-desktop-app-support). Validate MSIX deployment early. OCR also requires [available language packs](https://learn.microsoft.com/en-us/uwp/api/windows.media.ocr.ocrengine.availablerecognizerlanguages). Detect missing resources and explain how to install them; installed resources must work offline.

For future macOS support, reuse portable engine logic and replace the platform-specific shell, capture, OCR, speech, and credential adapters.

## Engine boundaries and versions

Keep the engine separate from the tray UI within this repository. It should expose capture-processing orchestration, OCR and speech provider interfaces, and reading-profile logic without depending on the web platform.

The initial portable interfaces, domain contracts, mock providers, and implementation sequence are documented in [the desktop engine architecture](docs/architecture/desktop-engine-architecture.md).
The bundled font assets and their redistribution notices are listed in [third-party font notices](docs/third-party-fonts.md).

Start with one bundled engine and display separate versions in Settings → About and diagnostic reports:

```text
Desktop app:       0.1.0
Reading engine:    0.1.0
Platform API:      v1 (when connected)
```

These are illustrative versions, not existing releases. App and engine versions may advance independently. Track the upstream OCR/speech provider identity separately; an OS-provided engine is updated by the OS rather than by this app.

The MVP bundles a tested app/engine combination. Engine downloads, version selection, and rollback are later features. Independent version labels do not require a runtime manager, network service, or third repository.

## Local settings and synchronization

All reading and device settings must be editable inside the app without signing in.

Reading profiles can synchronize through the platform when enabled. Shortcuts, startup behavior, default reading mode, window placement, installed voices, and device-specific OCR resources remain local. If a synced profile references an unavailable font or voice, use a local fallback and explain the substitution.

Apply local changes immediately and queue eligible changes while offline. Use a versioned settings format and server revision checks so concurrent edits can be detected and resolved. A sync error must never block capture or reading.

## Data handling

- Process screenshots and OCR locally by default; keep captures transient unless the user explicitly saves them.
- Do not upload captured text merely because an account is connected or settings sync is enabled.
- Cloud speech sends the selected text to the chosen service only when the user enables or requests it; explain that behavior at selection time.
- Keep account tokens in OS-protected credential storage and provider secrets on the platform server.
- Keep captured text, screenshots, and tokens out of diagnostic logs.

## Compatibility and performance

The product goal is quick access and broad compatibility across ordinary desktop applications. Treat the competitive advantage as a hypothesis to validate through user trials and comparison measurements.

An [always-on-top window](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowpos) can stay above ordinary windows, but that does not guarantee visibility over every desktop surface. Secure desktops, capture-protected content, exclusive fullscreen applications, and competing topmost windows require explicit limits and testing. Windows capture APIs also impose [protected-content and desktop-access restrictions](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api).

Measure shortcut-to-selector time, selection-to-readable-text time, time to first speech, idle CPU, memory, and OCR accuracy on documented reference hardware. Keep OCR and network requests off the UI thread. Cloud calls must never delay the local reading path.

## Delivery plan

1. **Technical prototype:** shortcut → snip → local OCR → reader; verify installation, fonts, display scaling, and window behavior.
2. **Free desktop MVP:** tray, floating and focus reading modes, reader toolbar, persistent settings, reading profiles, local speech, keyboard accessibility, offline operation.
3. **Platform integration:** browser sign-in, profile sync, cloud voice access, graceful disconnection.
4. **Later:** additional providers, engine switching, macOS, and more advanced reading assistance based on user feedback.

Release gates include operation with networking disabled, no account, and an unavailable platform; mixed-DPI monitor checks; accessible reader controls; focus-mode exit and focus restoration; and a documented compatibility matrix.

## Repository boundary

`DyslexiaApp` owns the tray app, C++ engine, native adapters, installer, desktop tests, and desktop releases.

`DyslexiaPlatform` owns the React dashboard, Java Spring Boot backend, account services, PostgreSQL schema, API contracts, subscriptions, and cloud integrations. Shared contracts are versioned and consumed by both repositories; neither repository imports the other's private implementation.

Build and installation instructions will be added when an executable prototype exists.

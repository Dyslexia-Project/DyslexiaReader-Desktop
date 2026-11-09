# Desktop Reader Engine Architecture

## Status and scope

This is the architecture baseline for the Windows-first desktop reader. Portable domain types and dependency boundaries live in `include/dyslexia_reader/`; Windows adapters are isolated under `src/windows/` and `include/dyslexia_reader/windows/`.

The Windows app owns the tray, reader windows, toolbar, settings, and platform-specific adapters. A portable engine coordinates reading documents, profiles, speech events, and reading aids through interfaces. Future macOS support can replace shell, capture, OCR, speech, and rendering adapters without changing those domain contracts.

## Requirements basis

- `apor_tasks.txt`: Direct2D/DirectWrite canvas; in-memory bundled OpenDyslexic, Lexend, and Atkinson Hyperlegible fonts; live typography/theme controls; bionic emphasis; reading ruler; offline WinRT speech and bookmarks; cloud synthesis and temporary audio caching; synchronized sentence/word highlights.
- `README.md`: offline-first local OCR and speech; cloud speech is explicit; no screenshot/text upload by default; local features remain useful without an account or service; requests stay off the UI thread; package identity for Windows OCR and speech must be validated early.
- `docs/contracts/reading-profile.schema.json` and `color-themes.json`: profile defaults, bounded typography values, aid toggles, stable theme identifiers, and semantic colors.
- `docs/design/desktop-reader-wireframes.md`: floating and focus modes, immediate settings preview, preserved reading context, and no OCR rerun for appearance changes.
- `docs/design/dyslexia-aids-visual-parameters.md`: 40% initial bionic emphasis (ceiling, minimum eligible word length 3), line-layout-based ruler, reliable timing requirements, and visual priority.
- `docs/design/keyboard-navigation-shortcuts.md`: keyboard-only operation, focus restoration, Escape behavior, and shortcut-conflict checks.
- `docs/design/accessibility-checklist-first-demo.md`: understandable empty/loading/error states, keyboard and screen-reader access, and no misleading timing highlights.

The task's 40–50% bionic emphasis range is represented by the design document's 40% initial value. It is an implementation default, not a new persisted preference. OCR language-pack prompts and capture cancellation are adapter/workflow responsibilities; no capture data should be logged or sent to the cloud by default.

## Boundaries and dependency direction

```text
Win32 app shell / reader controls
  ├─ Reading workflow coordinator
  │    ├─ IScreenCapturer → IOcrEngine → IParagraphRebuilder
  │    ├─ IReadingProfileStore
  │    └─ ITextRenderer ← IReadingAid implementations
  ├─ ISpeechEngine (WinRT local or cloud-backed)
  │    ├─ ICloudSpeechClient → Spring Boot API adapter
  │    ├─ IAudioPlayer
  │    └─ IAudioCache (%TEMP% adapter)
  └─ IFontLoader → DirectWrite in-memory font registration adapter
```

High-level orchestration depends on the interfaces in `interfaces.hpp`, not concrete platform APIs. The Spring Boot route, authentication, request/response serialization, and transport remain behind `ICloudSpeechClient`. The desktop must request cloud speech only after explicit user intent and make text sharing clear. Keep local OCR, profile editing, and local speech independent of network/account state.

## Contract rules

- Canonical reading text is UTF-16 (`std::u16string`) to map cleanly to DirectWrite and WinRT. `TextRange` is a half-open range measured in UTF-16 code units. Unicode-aware grapheme segmentation is required for bionic emphasis; byte counts are never character counts.
- OCR bounding boxes are in captured-image pixels. `CapturedImage` carries desktop bounds and X/Y scale so mixed-DPI adapters can map physical pixels correctly. Cancellation is represented by an empty `optional`; capture/OCR/provider failures are explicit errors, not empty successful documents.
- Contract operations report failures with `ReaderError` and a `ReaderErrorCode`; cancellation is not an error. UI/workflow code translates these codes into actionable, accessible status messages without logging captured text or credentials.
- `IParagraphRebuilder` returns document text plus paragraph, sentence, and word ranges. It owns hyphen-wrap joining and line-to-paragraph decisions; rendering never reconstructs OCR text.
- Profile values mirror schema version 1. The schema remains the persistence authority; validate/clamp or reject invalid persisted values in profile loading, and keep theme colors in the shared JSON contract rather than duplicating hex values in controls.
- `ITextRenderer::set_profile` updates layout/rendering without invoking capture or OCR. Playback-highlight updates only affect visuals and invalidate the affected drawing; they must not recreate text layout or cause reflow. The Direct2D adapter owns the HWND and calls `InvalidateRect`.
- `IReadingAid` produces semantic style ranges and overlays from the current layout snapshot. Bionic styling, ruler geometry, and playback highlighting stay separate so combined effects do not overwrite one another.
- Speech marks reference ranges in the exact text submitted for synthesis and carry millisecond offsets. Providers report capability/reliability; do not infer word-level synchronization when it is unavailable. Stop clears highlights; pause preserves them.
- Blocking capture/OCR/cloud work is scheduled away from the UI thread by the workflow coordinator. Adapter APIs are synchronous at this contract layer to keep cancellation, threading, and dispatch policies out of domain interfaces; callbacks are used for speech events.
- `IAudioCache` is a replaceable boundary. `CachingCloudSpeechClient` keys results by service namespace, text, voice, rate, and selection. `WindowsAudioCache` stores versioned audio/mark records under `%TEMP%` using a SHA-256 filename; it does not persist the text key. Cache errors must be surfaced and must not silently report successful synthesis.

## Header contracts and mocks

- `domain.hpp`: shared profile, OCR, document, geometry, layout, highlight, session, and speech value types.
- `interfaces.hpp`: focused capture, OCR, paragraph, font, rendering, reading-aid, speech, cloud, cache, and profile-store abstractions.
- `mocks.hpp`: deterministic capture/OCR/rebuilder fixtures (including `compre-` + `hension`), plus basic renderer, font, local speech, and cloud-client test doubles.

Mocks are fixtures only; the mock font and audio bytes are not valid production media. They must be injectable through the same interfaces as platform implementations and must not require caller-side type checks.

The `Direct2DTextRenderer` and `dyslexia_reader_demo` provide the Windows HWND/render-target shell. The demo obtains its sample document through `MockScreenCapturer`, `MockOcrEngine`, and `MockParagraphRebuilder`; it performs no real screen capture or OCR. `FontResourceLoader` reads embedded Windows resources, and `InMemoryFontCollection` registers the bundled font faces through `IDWriteFactory5::CreateInMemoryFontFileLoader`. Font license notices are kept with the assets and summarized in `docs/third-party-fonts.md`. The renderer includes keyboard-accessible typography/theme controls, independent Bionic emphasis, Reading Ruler, and bimodal-highlighting toggles, a line-layout-driven ruler movable with the mouse or `Alt+Up` / `Alt+Down`, and controls for local voice selection and playback. Sentence tint and high-contrast word outlines use DirectWrite hit-test bounds and only appear when the corresponding timing capability is reliable. `WindowsSpeechEngine` synthesizes on a worker thread and maps documented word/sentence timed metadata cues to UTF-16 document ranges. `WindowsCloudSpeechClient`, `CachingCloudSpeechClient`, and `WindowsAudioCache` implement the cloud transport and cache boundary against the provisional v1 schema in `docs/contracts/cloud-speech-v1.schema.json`. The cloud client/cache are not yet connected to a reader-toolbar opt-in or playback path. The linked platform backend does not yet implement this speech route, so end-to-end service compatibility remains unverified. The Windows target remains uncompiled on the current Linux host.

The in-memory font collection uses `dwrite_3.h` and requires the Windows 10 SDK. `IDWriteFactory5::CreateInMemoryFontFileLoader` requires Windows 10 build 15063 or later at runtime.

## Sequential implementation plan

1. **Reader window and render target** — complete. Create the Win32 HWND host, Direct2D HWND render target, DirectWrite factory/layout, resize/DPI handling, and deterministic mock-fed sample window.
2. **Bundled in-memory fonts** — complete. Embed licensed Lexend, Atkinson Hyperlegible, and OpenDyslexic font files as resources and register a DirectWrite custom collection without installing fonts system-wide.
3. **Live typography and themes** — complete. Native controls update font family, size, letter spacing, line height, and the semantic palette from `color-themes.json`; schema-v1 bounds are validated and changes redraw without capture/OCR.
4. **Visual reading aids** — implemented Bionic emphasis (40%, ceiling, alphabetic words of at least three graphemes), with UTF-16 grapheme-safe ranges, plus a theme-colored, layout-driven ruler with 25% surrounding-line dimming and mouse/keyboard movement.
5. **Offline speech** — complete. Implement WinRT synthesis on a worker thread, local voice selection, Play/Pause, Stop, rate controls, and word/sentence timed-cue mapping to document ranges. `SpeechSynthesizer` does not expose the requested `SpeechBookmarkReached` event; the adapter uses the documented `IncludeWordBoundaryMetadata` / `IncludeSentenceBoundaryMetadata` options and `TimedMetadataTrack.CueEntered` events instead.
6. **Cloud speech and cache** — implemented against a provisional v1 contract because no speech controller/schema exists in the linked Spring Boot repository. `WindowsCloudSpeechClient` POSTs HTTPS JSON to `/api/v1/speech/synthesize`, requests audio and speech marks, validates UTF-16 ranges, and accepts an injected bearer-token provider. `CachingCloudSpeechClient` and `WindowsAudioCache` persist result chunks and marks under `%TEMP%` without storing text in filenames or cache keys on disk. Align the contract with the backend before end-to-end deployment. The explicit reader-toolbar opt-in and cloud audio playback path remain follow-ups.
7. **Bimodal highlighting** — implemented reliable sentence tint and DirectWrite-bounds word outlines, with an independent toolbar toggle; timing pauses with playback and clears on stop/completion.
8. **Acceptance validation** — validate the assigned features, font loading, Windows SDK build, mock-fed workflow, accessibility, offline behavior, and no-admin font operation. Real capture and OCR are out of scope for this implementation.

The cloud request/response schema, exact font asset licenses/packaging, and final keyboard shortcut mappings are explicit integration decisions to confirm during their corresponding tasks; no undocumented API or shortcut is assumed here.

## Contract and window-shell verification

The portable smoke tests check capture/OCR/paragraph/renderer mock substitution and cache hit/miss behavior, including rate, selection, voice, and service-namespace keying. The Windows-only reading-aids smoke test covers emphasis ranges, ruler overlays, and bimodal highlight composition. The portable Linux build and smoke tests pass. The Windows target, WinHTTP/WinRT transport, DirectWrite hit-testing, and `%TEMP%` cache require a Windows SDK and have not been compiled or run on this host. The cloud endpoint remains provisional until implemented by the platform backend; no claim is made here that real capture/OCR or cloud UI/playback integration is implemented.

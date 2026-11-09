# Dyslexia Reader — Accessibility Checklist for First Demo

## 1. Overview

This document provides an accessibility and usability validation checklist for the first demonstration of the Dyslexia Reader desktop application.

Its purpose is to help the development team evaluate whether the implemented interface follows the proposed UX specifications and supports a predictable, readable and keyboard-accessible experience.

The checklist covers:

- Reading profile customization.
- Color themes and visual contrast.
- Keyboard navigation and focus management.
- Floating Window and Focus Mode.
- Dyslexia assistance features.
- OCR and text-to-speech interaction.
- UI states and error handling.
- Overall demonstration readiness.

This checklist is intended for an early project demonstration. It does not represent a formal accessibility certification or a complete WCAG conformance audit.

## 2. Validation Scope

The first demo may contain only a subset of the planned application features.

Each checklist item should be evaluated according to its implementation status.

### 2.1 Status Definitions

| Status          | Meaning                                                      |
| --------------- | ------------------------------------------------------------ |
| PASS            | Implemented and successfully verified                        |
| FAIL            | Implemented but does not meet the expected behavior          |
| NOT IMPLEMENTED | Planned feature not yet available in the demo                |
| NOT APPLICABLE  | Requirement does not apply to the demonstrated functionality |
| NOT TESTED      | Feature or behavior has not yet been evaluated               |

A feature must not be marked as PASS solely because it is documented or visually represented in a mockup.

### 2.2 Validation Information

Complete the following information before running the checklist:

| Field                     | Value |
| ------------------------- | ----- |
| Demo version / commit     | TBD   |
| Test date                 | TBD   |
| Tester                    | TBD   |
| Operating system          | TBD   |
| Application build         | TBD   |
| Implemented demo features | TBD   |
| Known limitations         | TBD   |

## 3. Reading Profile Accessibility

### 3.1 Typography and Customization

- [ ] RP-01: The application displays text using the selected reading font.
- [ ] RP-02: The user can select among the supported font families: Lexend, Atkinson Hyperlegible and OpenDyslexic.
- [ ] RP-03: Font size can be adjusted within the supported range of 14–48 pt.
- [ ] RP-04: Letter spacing can be adjusted according to the reading profile.
- [ ] RP-05: Word spacing can be adjusted according to the reading profile.
- [ ] RP-06: Line height can be adjusted according to the reading profile.
- [ ] RP-07: Maximum reading line width is respected.
- [ ] RP-08: Typography changes are reflected immediately in the reading interface.
- [ ] RP-09: Text remains readable after changing typography settings.
- [ ] RP-10: Typography changes do not cause unexpected loss of reading content.

### 3.2 Profile Persistence

- [ ] RP-11: Reading preferences remain consistent between Floating Window and Focus Mode.
- [ ] RP-12: Reading preferences are saved through the application's persistence mechanism.
- [ ] RP-13: Previously saved preferences are restored when the application reloads them.
- [ ] RP-14: Invalid or unsupported profile values are handled safely.
- [ ] RP-15: The application uses the documented default values when no valid saved profile is available.

**Expected result:** Users can customize the reading experience without losing content or encountering unexpected interface behavior.

## 4. Color Themes and Visual Contrast

### 4.1 Theme Selection

- [ ] CT-01: Warm Cream is available.
- [ ] CT-02: Sepia is available.
- [ ] CT-03: Dark High-Contrast is available.
- [ ] CT-04: Switching themes updates the reading interface immediately.
- [ ] CT-05: Theme selection remains consistent between reading modes.
- [ ] CT-06: Text and interface controls remain distinguishable in all supported themes.

### 4.2 Contrast and Readability

- [ ] CT-07: Normal text meets the applicable WCAG contrast target of at least 4.5:1.
- [ ] CT-08: Large text meets the applicable WCAG contrast target of at least 3:1.
- [ ] CT-09: Essential non-text UI components and visual states meet the applicable 3:1 contrast requirement.
- [ ] CT-10: Keyboard focus indicators remain clearly visible in every theme.
- [ ] CT-11: Highlighted text remains readable.
- [ ] CT-12: Reading Ruler effects do not make surrounding text unreadable.
- [ ] CT-13: Information is not communicated exclusively through color.

Contrast should be evaluated using the actual rendered colors, including relevant highlighting and overlay combinations.

**Expected result:** All supported themes maintain readable text and distinguishable interactive controls.

## 5. Keyboard Accessibility

### 5.1 General Navigation

- [ ] KB-01: All essential interactive controls can be reached without a mouse.
- [ ] KB-02: `Tab` moves focus forward through interactive controls.
- [ ] KB-03: `Shift + Tab` moves focus backward.
- [ ] KB-04: Focus order follows a logical and predictable sequence.
- [ ] KB-05: Focus indicators are visible.
- [ ] KB-06: Buttons can be activated using standard keyboard controls.
- [ ] KB-07: The Reading Area supports keyboard scrolling.
- [ ] KB-08: No unintended keyboard traps occur.
- [ ] KB-09: Disabled controls behave according to native platform conventions.

### 5.2 Focus Management

- [ ] KB-10: Opening Reading Settings moves focus to an appropriate control.
- [ ] KB-11: Closing Reading Settings restores focus appropriately.
- [ ] KB-12: Entering Focus Mode places focus predictably.
- [ ] KB-13: Exiting Focus Mode restores focus to a logical location.
- [ ] KB-14: Theme changes do not unexpectedly move focus.
- [ ] KB-15: Font size changes do not unexpectedly move focus.
- [ ] KB-16: Playback highlighting does not interfere with keyboard focus.

### 5.3 Keyboard Shortcuts

- [ ] KB-17: Implemented reader shortcuts perform their documented actions.
- [ ] KB-18: Global shortcuts are checked for conflicts where supported.
- [ ] KB-19: Shortcuts do not unnecessarily override operating system commands.
- [ ] KB-20: Shortcuts do not interfere with editable text fields.
- [ ] KB-21: Essential actions remain accessible without memorizing shortcuts.
- [ ] KB-22: Available shortcuts are discoverable or documented.

**Expected result:** The demonstrated application can be operated using keyboard-only interaction.

## 6. Desktop Reading Modes

### 6.1 Floating Window

- [ ] FW-01: Floating Window displays OCR-extracted text.
- [ ] FW-02: Essential playback controls are accessible.
- [ ] FW-03: The reading content supports vertical scrolling.
- [ ] FW-04: The window can be resized where supported.
- [ ] FW-05: Controls remain accessible at the minimum supported window size.
- [ ] FW-06: Font and theme controls are available.
- [ ] FW-07: Enter Focus Mode is clearly identifiable.
- [ ] FW-08: The reading area adapts appropriately when the window is resized.

### 6.2 Focus Mode

- [ ] FM-01: Focus Mode provides an expanded reading environment.
- [ ] FM-02: The reading column remains centered and the text left-aligned.
- [ ] FM-03: The configured maximum line width is respected.
- [ ] FM-04: Essential playback controls remain accessible.
- [ ] FM-05: Reading Settings remain accessible.
- [ ] FM-06: Exit Focus Mode is clearly identifiable.
- [ ] FM-07: `Escape` exits Focus Mode when no higher-priority overlay is active.
- [ ] FM-08: Reading preferences are preserved when switching modes.
- [ ] FM-09: Reading position is preserved where technically feasible.
- [ ] FM-10: TTS playback state is preserved where supported.
- [ ] FM-11: Returning to Floating Window restores its previous state where feasible.

**Expected result:** Users can switch between reading modes without unnecessary disruption.

## 7. Dyslexia Assistance Features

### 7.1 Bionic Reading

- [ ] BR-01: Bionic Reading can be enabled and disabled independently.
- [ ] BR-02: The beginning of eligible words is visually emphasized.
- [ ] BR-03: The original text content remains unchanged.
- [ ] BR-04: Punctuation and word order are preserved.
- [ ] BR-05: Font-weight emphasis follows the documented initial parameters.
- [ ] BR-06: Disabling the feature restores normal text rendering.
- [ ] BR-07: The feature does not introduce unnecessary layout shifts.
- [ ] BR-08: Text remains accessible to assistive technologies.

### 7.2 Interactive Reading Ruler

- [ ] RR-01: Reading Ruler can be enabled and disabled independently.
- [ ] RR-02: The ruler highlights the selected rendered visual line.
- [ ] RR-03: Surrounding text remains readable.
- [ ] RR-04: The ruler can be moved using keyboard controls.
- [ ] RR-05: The ruler adapts when text wrapping changes.
- [ ] RR-06: The ruler adapts when font size or spacing changes.
- [ ] RR-07: Ruler movement does not cause excessive scrolling.
- [ ] RR-08: Disabling the ruler restores the normal reading presentation.

### 7.3 Bimodal Highlighting

- [ ] BH-01: Bimodal Highlighting can be enabled and disabled independently.
- [ ] BH-02: Sentence highlighting follows reliable timing information when available.
- [ ] BH-03: Word highlighting follows reliable word-level timing when available.
- [ ] BH-04: Word-level highlighting is disabled when reliable timing is unavailable.
- [ ] BH-05: Highlighting pauses when TTS playback is paused.
- [ ] BH-06: Highlighting resumes appropriately with playback.
- [ ] BH-07: Active playback highlighting clears when playback stops.
- [ ] BH-08: Highlighting does not cause text reflow.
- [ ] BH-09: Active text remains readable across all color themes.
- [ ] BH-10: Audio playback remains functional when highlighting is disabled.

### 7.4 Combined Feature Behavior

- [ ] DA-01: The three features can be enabled independently.
- [ ] DA-02: Multiple active features do not obscure text.
- [ ] DA-03: Bionic Reading emphasis remains visible during playback highlighting.
- [ ] DA-04: Reading Ruler and sentence highlighting remain visually distinguishable.
- [ ] DA-05: The current spoken word remains identifiable when multiple effects are active.
- [ ] DA-06: Feature settings remain consistent across reading modes.

**Expected result:** Implemented assistance features remain optional, predictable and readable when used individually or together.

## 8. OCR and Text-to-Speech Accessibility

### 8.1 OCR Workflow

- [ ] OCR-01: The user can initiate the supported text capture workflow.
- [ ] OCR-02: Screen area selection provides understandable visual feedback.
- [ ] OCR-03: The capture workflow can be canceled without losing previous reading content.
- [ ] OCR-04: Successfully extracted text appears in the reading interface.
- [ ] OCR-05: OCR processing status is communicated clearly.
- [ ] OCR-06: OCR failures produce understandable feedback.
- [ ] OCR-07: The user can retry after an OCR failure.

### 8.2 TTS Workflow

- [ ] TTS-01: Play/Pause is implemented as a single state-dependent control.
- [ ] TTS-02: Playback can be started using the keyboard.
- [ ] TTS-03: Playback can be paused where supported.
- [ ] TTS-04: Playback can be resumed where supported.
- [ ] TTS-05: Playback can be stopped.
- [ ] TTS-06: The current playback state is visually identifiable.
- [ ] TTS-07: Available voices can be selected using the keyboard.
- [ ] TTS-08: Playback controls expose understandable accessible labels.
- [ ] TTS-09: Opening Reading Settings does not unexpectedly stop playback.
- [ ] TTS-10: Unavailable TTS functionality is communicated clearly.

**Expected result:** Users can extract text and interact with supported speech playback controls without unnecessary barriers.

## 9. UI States and Error Handling

### 9.1 Empty States

- [ ] UI-01: The application displays an informative message when no text is available.
- [ ] UI-02: Playback controls requiring text are disabled appropriately.
- [ ] UI-03: The user can initiate or repeat text extraction.

### 9.2 Loading States

- [ ] UI-04: OCR processing displays an understandable loading indicator.
- [ ] UI-05: TTS initialization displays appropriate status feedback when necessary.
- [ ] UI-06: Duplicate operations are prevented where appropriate.
- [ ] UI-07: Unrelated controls remain accessible where possible.

### 9.3 Error States

- [ ] UI-08: OCR errors are explained in understandable language.
- [ ] UI-09: TTS errors are communicated clearly.
- [ ] UI-10: Previously available text is preserved when appropriate.
- [ ] UI-11: Users can recover from supported error states.
- [ ] UI-12: The application does not display misleading synchronization when timing information is unavailable.

**Expected result:** Users can understand the current application state and recover from common failures.

## 10. Screen Reader and Assistive Technology Considerations

Where supported by the selected desktop framework and operating system:

- [ ] AT-01: Interactive controls expose meaningful accessible names.
- [ ] AT-02: Control roles are correctly exposed.
- [ ] AT-03: Toggle states are exposed to assistive technologies.
- [ ] AT-04: The Reading Area exposes the original text content.
- [ ] AT-05: Focus changes are communicated appropriately.
- [ ] AT-06: Important status messages can be identified without relying exclusively on visual feedback.
- [ ] AT-07: Decorative visual effects do not create duplicate or misleading text content.
- [ ] AT-08: Standard screen reader navigation is not unnecessarily overridden.

Screen reader testing should use tools appropriate to the target operating system, such as NVDA on Windows or VoiceOver on macOS.

**Expected result:** The application exposes sufficient semantic information for assistive technologies to interact with implemented features.

## 11. First Demo Test Scenarios

The following scenarios provide a practical sequence for evaluating the first demo.

### Scenario 1 — Capture and Read Text

**Steps:**

1. Launch Dyslexia Reader.
2. Initiate the supported screen capture workflow.
3. Select a region containing readable text.
4. Wait for OCR processing.
5. Verify that extracted text appears in the Floating Window.
6. Start TTS playback if available.

**Expected result:** The user can capture text, view the extracted content and initiate supported playback without encountering an accessibility barrier.

### Scenario 2 — Customize Reading Preferences

**Steps:**

1. Open Reading Settings.
2. Change the font family.
3. Increase the font size.
4. Adjust line spacing.
5. Switch between Warm Cream, Sepia and Dark High-Contrast.
6. Close Reading Settings.

**Expected result:** Changes apply immediately, text remains readable and focus is restored appropriately.

### Scenario 3 — Switch Reading Modes

**Steps:**

1. Open the Floating Window with text available.
2. Enter Focus Mode.
3. Verify the centered reading layout.
4. Navigate using the keyboard.
5. Exit Focus Mode.
6. Verify that the previous reading context is preserved where supported.

**Expected result:** Mode switching is predictable and does not unnecessarily disrupt reading.

### Scenario 4 — Test Dyslexia Assistance Features

**Steps:**

1. Enable Bionic Reading.
2. Verify that eligible word beginnings are emphasized.
3. Enable Reading Ruler.
4. Move the ruler using the keyboard.
5. Enable Bimodal Highlighting.
6. Start TTS playback if synchronization is supported.
7. Verify that active highlights follow available timing information.

**Expected result:** Implemented assistance features behave consistently and do not obscure the text.

### Scenario 5 — Keyboard-Only Interaction

**Steps:**

1. Place the mouse aside.
2. Navigate through the Floating Window using `Tab` and `Shift + Tab`.
3. Activate playback controls using the keyboard.
4. Open and close Reading Settings.
5. Enter and exit Focus Mode.
6. Verify that keyboard focus remains visible.

**Expected result:** Essential implemented functionality remains accessible without a mouse.

### Scenario 6 — Error Recovery

**Steps:**

1. Attempt a capture operation that produces no readable text or an OCR failure.
2. Verify that the application communicates the result.
3. Retry the operation.
4. Test the unavailable-TTS state if it can be reproduced safely.
5. Verify that the interface remains usable.

**Expected result:** Error states are understandable and users can recover without restarting the application unnecessarily.

## 12. Issue Reporting Template

When a checklist item fails, record the issue using the following structure:

| Field              | Description                                    |
| ------------------ | ---------------------------------------------- |
| Issue ID           | Unique issue identifier                        |
| Checklist ID       | Related checklist item, e.g. KB-05             |
| Description        | Short explanation of the problem               |
| Steps to Reproduce | Actions required to reproduce the issue        |
| Expected Behavior  | Behavior defined by the specification          |
| Actual Behavior    | Observed behavior                              |
| Severity           | Critical / High / Medium / Low                 |
| Platform           | Operating system and version                   |
| Evidence           | Screenshot, recording or logs where available |
| Status             | Open / In Progress / Resolved                  |

Issues should be tracked in the team's GitHub repository.

## 13. Demo Readiness Criteria

The first demo can be considered ready for presentation when:

- The implemented demo scope is clearly documented.
- The primary demonstration workflow can be completed successfully.
- Essential implemented controls are keyboard-accessible.
- Reading text remains readable in the demonstrated themes.
- No known critical accessibility issue blocks the primary workflow.
- Error states are handled appropriately for the implemented features.
- Unimplemented features and known limitations are documented.
- The team can explain which accessibility requirements have been verified and which remain pending.

Passing this checklist does not imply that all planned features are implemented or that the application is fully WCAG conformant.

## 14. Validation Summary

Complete this section after testing.

| Category               | Passed | Failed | Not Implemented | Not Applicable | Not Tested |
| ---------------------- | ------ | ------ | --------------- | -------------- | ---------- |
| Reading Profile        | TBD    | TBD    | TBD             | TBD            | TBD        |
| Color Themes           | TBD    | TBD    | TBD             | TBD            | TBD        |
| Keyboard Accessibility | TBD    | TBD    | TBD             | TBD            | TBD        |
| Desktop Reading Modes  | TBD    | TBD    | TBD             | TBD            | TBD        |
| Dyslexia Assistance    | TBD    | TBD    | TBD             | TBD            | TBD        |
| OCR & TTS              | TBD    | TBD    | TBD             | TBD            | TBD        |
| UI States              | TBD    | TBD    | TBD             | TBD            | TBD        |
| Assistive Technology   | TBD    | TBD    | TBD             | TBD            | TBD        |

### Overall Demo Status

**Status:** NOT YET EVALUATED

**Critical Issues:** TBD

**Known Limitations:** TBD

**Next Actions:** TBD

## 15. Related Documents

- `docs/contracts/reading-profile.schema.json`
- `docs/contracts/reading-profile.example.json`
- `docs/contracts/color-themes.json`
- `docs/design/color-themes.md`
- `docs/design/desktop-reader-wireframes.md`
- `docs/design/dyslexia-aids-visual-parameters.md`
- `docs/design/keyboard-navigation-shortcuts.md`

## 16. Final Notes

This checklist is intended to support iterative accessibility validation throughout development.

It should be updated as new functionality becomes available and as usability testing identifies additional requirements.

The team should prioritize removing barriers that prevent users from completing essential reading tasks.

Accessibility validation should be treated as an ongoing development activity rather than a one-time task performed only before release.

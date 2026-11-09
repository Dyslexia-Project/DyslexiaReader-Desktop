# Dyslexia Reader — Keyboard Navigation & Shortcuts

## 1. Overview

This document defines the proposed keyboard navigation behavior, shortcut mappings and focus management requirements for the Dyslexia Reader desktop application.

The goal is to ensure that users can operate the desktop reader without relying on a mouse, while maintaining predictable behavior and compatibility with operating system and assistive technology conventions.

The specification covers:

- Global application shortcuts.
- Keyboard navigation in Floating Window.
- Keyboard navigation in Focus Mode.
- Text-to-speech playback controls.
- Interactive Reading Ruler navigation.
- Reading Settings interaction.
- Focus management and accessibility requirements.
- Platform-specific considerations for Windows and macOS.

The mappings described in this document are initial UX proposals. They must be validated against operating system restrictions, assistive technology shortcuts and the capabilities of the native desktop implementation.

## 2. Design Principles

Keyboard interaction should follow these principles:

- **Full keyboard accessibility:** All essential functionality must be available without a mouse.
- **Predictable navigation:** Keyboard focus should follow a logical and consistent order.
- **Visible focus:** The currently focused interactive element must have a clear visual indicator.
- **Standard conventions:** Common keyboard interaction patterns should be preserved.
- **Minimal conflicts:** Shortcuts should avoid interfering with operating system or assistive technology commands.
- **Consistency:** Shared controls should behave consistently in Floating Window and Focus Mode.
- **Recoverability:** Users should be able to close overlays and return to their previous context.
- **Discoverability:** Keyboard shortcuts should be documented and discoverable within the interface.

The application should not require users to memorize shortcuts in order to access essential functionality.

## 3. Standard Keyboard Navigation

The following keyboard interactions should be supported throughout the desktop application.

| Key | Expected Behavior |
|---|---|
| `Tab` | Move focus to the next interactive element |
| `Shift + Tab` | Move focus to the previous interactive element |
| `Enter` | Activate the focused control where appropriate |
| `Space` | Activate the focused button or toggle where appropriate |
| `Escape` | Close the active dismissible overlay or exit the current temporary mode |
| `Arrow Keys` | Navigate within controls that support directional interaction |
| `Home` / `End` | Navigate to the beginning or end within supported controls |
| `Page Up` / `Page Down` | Scroll the reading area when it has keyboard focus |

Native controls should retain their expected platform-specific keyboard behavior.

The application must not override standard text navigation, text selection or assistive technology commands unnecessarily.

## 4. Global Application Shortcuts

### 4.1 Purpose

Global shortcuts allow users to activate Dyslexia Reader while working in another application.

These shortcuts may be registered at the operating system level and should remain available when the Dyslexia Reader window is not focused.

Global shortcuts must be treated separately from shortcuts that only work inside the reader.

### 4.2 Proposed Global Shortcuts

| Action | Windows | macOS |
|---|---|---|
| Start screen area capture | `Ctrl + Shift + R` | `Cmd + Shift + R` |
| Open or bring the reader to the foreground | `Ctrl + Shift + D` | `Cmd + Shift + D` |

These mappings are proposed defaults and are not guaranteed to be available on every system.

The final implementation must check for conflicts with existing system-wide shortcuts and other applications.

### 4.3 Global Shortcut Behavior

When the screen capture shortcut is activated:

1. The application initiates the screen area selection workflow.
2. The user selects the desired screen region.
3. The selected region is processed by the OCR component.
4. The extracted text is displayed in the Floating Window when processing succeeds.
5. The user can begin reading or text-to-speech playback.

If the capture operation is canceled, the application should return to its previous state without losing existing reading content.

When the reader activation shortcut is triggered:

- If the reader is hidden or minimized, it should be brought to the foreground.
- If the reader is already visible, its existing content should be preserved.
- The application should avoid creating duplicate reader windows unnecessarily.

### 4.4 Global Shortcut Failure Handling

If a global shortcut cannot be registered:

- Inform the user that the shortcut is unavailable.
- Avoid silently overriding another application's shortcut.
- Keep the same functionality accessible through the application interface.
- Allow an alternative shortcut to be configured if the implementation supports customization.

Global shortcut registration and permissions must be handled according to the operating system.

## 5. Floating Window Keyboard Navigation

### 5.1 Focus Order

The Floating Window contains three main interface areas:

1. Header / Playback Toolbar.
2. Reading Area.
3. Bottom Toolbar.

The proposed Tab navigation order is:

| Order | Control |
|---|---|
| 1 | Play/Pause |
| 2 | Stop |
| 3 | Voice Selector |
| 4 | Reading Settings |
| 5 | Reading Area |
| 6 | Font Selector |
| 7 | Decrease Font Size |
| 8 | Increase Font Size |
| 9 | Theme Selector |
| 10 | Enter Focus Mode |

The exact order may be adjusted to match the final visual layout, but it must remain logical and predictable.

Controls that are disabled should follow the native platform's expected keyboard behavior.

### 5.2 Playback Controls

**Play/Pause**

- Receives focus through normal keyboard navigation.
- Can be activated using `Enter` or `Space`.
- Changes its accessible label according to the playback state.
- Does not unexpectedly move keyboard focus when its state changes.

**Stop**

- Can be activated using `Enter` or `Space`.
- Stops playback without unexpectedly changing the currently focused control.

**Voice Selector**

- Can be opened using the standard keyboard interaction for the selected native control.
- Supports navigating the available voices using keyboard controls.
- Announces the currently selected voice through accessible control properties.

**Reading Settings**

- Can be activated using `Enter` or `Space`.
- Opens the Reading Settings panel.
- Moves keyboard focus into the settings interface.

### 5.3 Reading Area

The Reading Area should:

- Be reachable using keyboard navigation.
- Support vertical scrolling without a mouse.
- Preserve standard text navigation and selection behavior where supported.
- Avoid trapping keyboard focus.
- Remain accessible to screen readers through the platform accessibility API.

When the Reading Area is focused, standard scrolling keys should behave according to the native control's conventions.

The application must not intercept these keys in ways that prevent normal reading or text selection.

### 5.4 Bottom Toolbar

The bottom toolbar should provide keyboard access to:

- Font family selection.
- Font size adjustment.
- Theme selection.
- Enter Focus Mode.

Font size buttons should update the reading interface immediately without unexpectedly moving focus.

Theme changes should preserve the current focus and reading position where feasible.

## 6. Focus Mode Keyboard Navigation

### 6.1 Purpose

Focus Mode provides a distraction-reduced reading environment while preserving keyboard accessibility.

The interface should contain fewer permanently visible controls than Floating Window, without making essential functionality inaccessible.

### 6.2 Proposed Focus Order

| Order | Control |
|---|---|
| 1 | Exit Focus Mode |
| 2 | Play/Pause |
| 3 | Stop |
| 4 | Voice Selector |
| 5 | Reading Settings |
| 6 | Main Reading Area |

Secondary controls may be accessed through Reading Settings.

### 6.3 Entering Focus Mode

When Focus Mode is activated:

- Preserve the current reading profile.
- Preserve the extracted text.
- Preserve the reading position where feasible.
- Preserve TTS playback state when supported.
- Move keyboard focus to an appropriate element in the Focus Mode interface.

The initial focus target should be predictable and should not trigger an action automatically.

The proposed initial focus target is the **Exit Focus Mode** control.

### 6.4 Exiting Focus Mode

The user should be able to exit Focus Mode by:

- Activating the Exit Focus Mode control.
- Pressing `Escape` when no higher-priority dismissible overlay is active.

After exiting:

- Restore the Floating Window.
- Restore its previous dimensions and position where supported.
- Preserve the reading context.
- Return keyboard focus to the Enter Focus Mode control or another appropriate equivalent if that control is unavailable.

## 7. Text-to-Speech Keyboard Shortcuts

### 7.1 Proposed Reader Shortcuts

The following shortcuts are local to the Dyslexia Reader interface.

| Action | Windows | macOS |
|---|---|---|
| Play/Pause TTS | `Ctrl + Space` | `Cmd + Space` (requires replacement) |
| Stop TTS | `Ctrl + Shift + S` | `Cmd + Shift + S` |
| Increase font size | `Ctrl + +` | `Cmd + +` |
| Decrease font size | `Ctrl + -` | `Cmd + -` |
| Open Reading Settings | `Ctrl + ,` | `Cmd + ,` |

**Important:** `Cmd + Space` is commonly reserved for Spotlight on macOS and must not be used as the final default.

For consistency, the recommended cross-platform alternative for Play/Pause is:

`Ctrl + Shift + Space`

This shortcut is proposed for both Windows and macOS, subject to conflict testing.

### 7.2 Playback Shortcut Behavior

**Play/Pause**

- Starts playback if no audio is currently playing.
- Pauses active playback.
- Resumes paused playback when supported.
- Preserves the reading position when possible.

**Stop**

- Stops playback.
- Clears active playback highlighting.
- Resets the playback position according to the selected TTS implementation.

Playback shortcuts must not trigger while the user is entering text into an editable field if doing so would interfere with normal input.

### 7.3 Shortcut Discoverability

The interface should provide a discoverable way to view keyboard shortcuts.

Possible approaches include:

- Tooltips displaying the assigned shortcut.
- A Keyboard Shortcuts section inside Reading Settings.
- Accessible labels or descriptions associated with relevant controls.

The displayed shortcut must match the actual platform-specific mapping.

## 8. Interactive Reading Ruler Keyboard Navigation

### 8.1 Purpose

The Interactive Reading Ruler must be operable without a mouse.

Users should be able to move the ruler between rendered visual lines while maintaining their current reading context.

### 8.2 Proposed Shortcuts

| Action | Proposed Shortcut |
|---|---|
| Move ruler to previous visual line | `Alt + Up Arrow` |
| Move ruler to next visual line | `Alt + Down Arrow` |

These shortcuts are proposed for use only when Reading Ruler is enabled and the reading interface is active.

They must be tested for conflicts with operating system, screen reader, and native text navigation shortcuts before being finalized.

### 8.3 Ruler Movement Behavior

When moving the ruler:

- Move to the previous or next rendered visual line.
- Preserve the selected reading profile.
- Keep the selected line visible where feasible.
- Avoid abrupt or excessive scrolling.
- Update the ruler position after text wrapping or font changes.

The ruler should follow the rendered layout rather than a fixed number of characters.

### 8.4 Ruler Focus Behavior

Ruler movement should not unexpectedly transfer keyboard focus away from the Reading Area.

If the ruler is disabled, ruler-specific shortcuts should not intercept normal keyboard input.

The ruler must not prevent standard text selection or screen reader navigation.

## 9. Reading Settings Keyboard Navigation

### 9.1 Opening Settings

Reading Settings may be opened through:

- The Reading Settings button.
- The proposed application shortcut.
- An accessible application menu entry, if available.

When the panel opens:

- Keyboard focus should move to the first appropriate interactive control.
- The previously focused control should be remembered.
- The user should be able to navigate all settings without a mouse.

### 9.2 Settings Focus Order

The proposed logical order is:

1. Font Family.
2. Font Size.
3. Letter Spacing.
4. Word Spacing.
5. Line Height.
6. Line Width.
7. Color Theme.
8. Bionic Reading.
9. Reading Ruler.
10. Bimodal Highlighting.
11. Close Settings.

The exact order may be adjusted to match the final settings layout.

### 9.3 Settings Interaction

Settings should follow standard keyboard conventions:

- `Tab` and `Shift + Tab` move between interactive controls.
- `Space` toggles checkboxes or switches when appropriate.
- Arrow keys adjust values within supported sliders and selection controls.
- `Enter` activates the focused button where appropriate.
- `Escape` closes the panel when it is dismissible.

Reading preference changes should be applied immediately.

A separate Apply button is not required for standard reading preference changes.

### 9.4 Closing Settings

When Reading Settings closes:

- Return focus to the control that opened the panel, if it still exists.
- Preserve the current reading profile.
- Preserve the current reading position where feasible.
- Avoid unexpectedly stopping TTS playback.

If Settings is implemented as a modal dialog, keyboard focus must remain within the dialog until it closes.

If Settings is implemented as a non-modal panel, focus must follow the corresponding native accessibility conventions.

## 10. Focus Management

### 10.1 General Requirements

The application must maintain predictable focus behavior during interface transitions.

Focus should not disappear or move unexpectedly when:

- Switching reading modes.
- Opening or closing Reading Settings.
- Changing color themes.
- Adjusting font size.
- Starting or stopping playback.
- Updating highlighted text.

### 10.2 Visible Focus Indicators

All keyboard-focusable controls must have a visible focus indicator.

The focus indicator should:

- Remain distinguishable across all three color themes.
- Use the semantic `focus` color token from `docs/contracts/color-themes.json`.
- Have sufficient contrast against adjacent colors.
- Remain visible when controls are highlighted or selected.
- Avoid relying on color alone when another visual distinction is necessary.

Focus indicators should not be removed merely for aesthetic reasons.

### 10.3 Focus Restoration

After closing a temporary interface:

- Return focus to the control that initiated the interaction when feasible.
- If the original control no longer exists, move focus to a logical alternative.
- Avoid moving focus to unrelated controls.

### 10.4 Avoiding Keyboard Traps

The application must not trap keyboard focus in ordinary interface areas.

Users must be able to navigate away from:

- The Reading Area.
- Playback controls.
- Font and theme selectors.
- Reading Ruler interactions.
- Non-modal settings panels.

Modal dialogs may temporarily contain focus according to standard accessibility conventions, but they must provide a predictable way to close or dismiss the dialog.

## 11. Platform-Specific Considerations

### 11.1 Windows

The Windows implementation should:

- Use platform-compatible keyboard navigation patterns.
- Check for conflicts before registering global shortcuts.
- Preserve expected behavior for standard controls.
- Expose accessible control information through the relevant Windows accessibility APIs.
- Avoid overriding shortcuts commonly used by assistive technologies.

### 11.2 macOS

The macOS implementation should:

- Follow native macOS keyboard conventions.
- Avoid reserved shortcuts such as `Cmd + Space`.
- Respect system-level accessibility settings.
- Support appropriate keyboard focus behavior for native controls.
- Check whether additional permissions are required for global shortcuts or screen capture.

### 11.3 Cross-Platform Consistency

The same user action should have consistent behavior across supported platforms.

However, identical key combinations are not mandatory when they conflict with platform conventions.

The final shortcut mapping should prioritize accessibility and predictability over strict shortcut uniformity.

## 12. Shortcut Conflict Handling

Before finalizing keyboard shortcuts, the implementation team should:

1. Review operating system shortcut reservations.
2. Check common assistive technology shortcut conflicts.
3. Verify that global shortcuts can be registered.
4. Test shortcuts inside editable fields.
5. Confirm that shortcuts behave consistently across reading modes.
6. Document any platform-specific differences.

If a shortcut conflicts with another essential action, it should be replaced rather than silently overriding the existing behavior.

User-configurable shortcuts may be considered as a future enhancement.

## 13. Accessibility Validation Checklist

Before keyboard navigation is considered complete, verify that:

- All essential reader functions can be used without a mouse.
- Tab order follows the visual and logical interface structure.
- Keyboard focus is visible across all color themes.
- Play/Pause can be activated using standard keyboard controls.
- Voice selection is keyboard-accessible.
- Reading Settings can be opened and closed without a mouse.
- All reading preferences can be changed using the keyboard.
- Focus Mode can be entered and exited using the keyboard.
- Focus returns to an appropriate control after closing panels.
- The Reading Area supports keyboard scrolling.
- Reading Ruler can be moved without a mouse.
- Ruler shortcuts do not block standard text navigation.
- Global shortcuts are checked for conflicts.
- Reader shortcuts do not interfere with editable text fields.
- No unintended keyboard traps are present.
- Shortcut behavior is documented for Windows and macOS.
- Screen reader users can identify controls and their current states.

## 14. Related Documents

- `docs/contracts/reading-profile.schema.json`
- `docs/contracts/color-themes.json`
- `docs/design/color-themes.md`
- `docs/design/desktop-reader-wireframes.md`
- `docs/design/dyslexia-aids-visual-parameters.md`

## 15. Implementation Notes

This document defines the proposed keyboard interaction behavior for the Dyslexia Reader desktop application.

The shortcut mappings are initial design proposals and should not be treated as finalized platform-level registrations.

The implementation team should validate them against native framework behavior, operating system restrictions and assistive technology compatibility.

The final implementation must ensure that essential functionality remains accessible through standard keyboard navigation even when optional shortcuts are unavailable.
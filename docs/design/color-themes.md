# Dyslexia Reader — Color Theme Specifications

## 1. Overview

This document defines the three color themes supported by Dyslexia Reader in Sprint 1.

The goal is to provide customizable, readable interfaces with consistent color specifications across the native C++ desktop application and the React web dashboard.

All themes use semantic color tokens and are designed to meet WCAG 2.2 contrast requirements.

Color preferences vary between individuals, and no theme is assumed to be universally optimal for people with dyslexia.

## 2. Supported Themes

The following identifiers match the `theme` property defined in `docs/contracts/reading-profile.schema.json`.

| Theme | Identifier | Background | Text |
|---|---|---|---|
| Warm Cream | `warm-cream` | `#FFFDD0` | `#111111` |
| Sepia | `sepia` | `#F4ECD8` | `#3B3026` |
| Dark High-Contrast | `dark-high-contrast` | `#121212` | `#F5F5F5` |

**Default theme:** `warm-cream`

### 2.1 Warm Cream

A light theme with a warm cream background and near-black text.

| Color Token | HEX |
|---|---|
| Background | `#FFFDD0` |
| Text | `#111111` |
| Surface | `#FFF8E7` |
| Border | `#70644D` |
| Focus | `#155EEF` |
| Sentence Highlight | `#DCEBFF` |
| Word Highlight Border | `#155EEF` |
| Reading Ruler | `#E4E9B4` |

Primary text contrast: **18.18:1**

### 2.2 Sepia

A paper-inspired theme with muted warm colors and dark brown text.

| Color Token | HEX |
|---|---|
| Background | `#F4ECD8` |
| Text | `#3B3026` |
| Surface | `#EDE1C8` |
| Border | `#75634F` |
| Focus | `#174EA6` |
| Sentence Highlight | `#D9E7ED` |
| Word Highlight Border | `#174EA6` |
| Reading Ruler | `#E1D7B9` |

Primary text contrast: **10.90:1**

### 2.3 Dark High-Contrast

A dark theme with light text and clearly distinguishable interaction indicators.

| Color Token | HEX |
|---|---|
| Background | `#121212` |
| Text | `#F5F5F5` |
| Surface | `#242424` |
| Border | `#A0A0A0` |
| Focus | `#FFD166` |
| Sentence Highlight | `#263B53` |
| Word Highlight Border | `#FFD166` |
| Reading Ruler | `#333D2B` |

Primary text contrast: **17.18:1**

## 3. Accessibility Requirements

### 3.1 Text Contrast

The following WCAG 2.2 contrast thresholds apply:

- Normal text: at least 4.5:1 (AA).
- Large text: at least 3:1 (AA).
- Normal text at AAA level: at least 7:1.

All three proposed primary text/background combinations exceed the AAA threshold.

### 3.2 Highlight Contrast

Text must remain readable when sentence highlighting or the reading ruler is active.

| Theme | Text / Sentence Highlight Contrast |
|---|---|
| Warm Cream | 15.62:1 |
| Sepia | 10.14:1 |
| Dark High-Contrast | 10.51:1 |

### 3.3 Keyboard Focus

Keyboard focus indicators must remain clearly visible in every theme.

The `focus` color token defines the theme-specific focus indicator. Focus appearance must also be evaluated against adjacent interface colors in the implemented UI.

### 3.4 Dyslexia Aid Highlighting

- `sentenceHighlight`: background of the active sentence during text-to-speech playback.
- `wordHighlightBorder`: outline around the currently spoken word.
- `rulerHighlight`: highlighted region used by the interactive reading ruler.

Highlighting must not make text unreadable or rely exclusively on color to communicate essential information.

## 4. Implementation Guidelines

### 4.1 Shared Identifiers

Both desktop and web implementations must use the identifiers defined in the reading-profile schema:

- `warm-cream`
- `sepia`
- `dark-high-contrast`

### 4.2 Semantic Color Tokens

Colors must be referenced through semantic tokens rather than scattered hardcoded HEX values.

This ensures consistency and simplifies future theme modifications.

### 4.3 Theme Persistence

The reading profile stores the selected theme identifier in its `theme` property.

The corresponding color palette is resolved by the client.

### 4.4 Desktop and Web Consistency

The C++ desktop reader and React dashboard should use the same theme definitions, even though they have separate UI implementations.

Theme switching should update the reading preview immediately.

## 5. Design Rationale

The three themes offer distinct visual alternatives:

- **Warm Cream:** a light reading environment with a warm background.
- **Sepia:** a paper-inspired reading environment with muted colors.
- **Dark High-Contrast:** a dark reading environment with light text and prominent focus indicators.

The palettes are initial design proposals based on general accessibility principles. They are not clinically validated treatments for dyslexia.

User testing will be necessary to evaluate individual comfort and preferences.

## 6. Validation

Before final release:

1. Verify implemented text and UI element contrast against WCAG 2.2.
2. Test keyboard focus visibility in all themes.
3. Verify text readability during highlighting.
4. Test theme switching and persistence.
5. Collect feedback from representative users and refine the palettes.

## 7. References

- [W3C — WCAG 2.2](https://www.w3.org/TR/WCAG22/)
- [W3C — Contrast Minimum](https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html)
- [W3C — Non-text Contrast](https://www.w3.org/WAI/WCAG22/Understanding/non-text-contrast.html)
- [W3C — Focus Appearance](https://www.w3.org/WAI/WCAG22/Understanding/focus-appearance.html)
- [British Dyslexia Association — Dyslexia Friendly Style Guide](https://www.bdadyslexia.org.uk/advice/employers/creating-a-dyslexia-friendly-workplace/dyslexia-friendly-style-guide)

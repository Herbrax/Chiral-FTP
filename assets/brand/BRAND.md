# chiral-ftp brand notes

## Concept
Two stepped arrows, one up and one down: the down arrow is the up arrow rotated 180°. Each arrow has a Z-shaped jog in its shaft, which gives the pair an S-shaped handedness (chirality). Together they read as a file transfer: upload and download. Flat 2D, two-tone gold split down each arrow's centre line, with chiselled (crystal-cut) tails.

## Wordmark
- Text: `chiral-ftp`, all lowercase, one word with the hyphen.
- Font: **IBM Plex Mono**, weight **500 (Medium)**. Free, SIL OFL. Google Fonts: `family=IBM+Plex+Mono:wght@500`.
- Letter-spacing: **-0.02em** (tight; still reads as monospace).
- Colour split: `chiral` is white on dark backgrounds and near-black on light ones. `-ftp` (the hyphen included) is gold.
- Fallback stack: `'IBM Plex Mono', ui-monospace, SFMono-Regular, Menlo, Consolas, monospace`.
- Never set it in bold or italic, and never in a proportional font.

## Colours
| Role | Dark background | Light background |
|---|---|---|
| Background | `#0E0F11` (icon) / `#131518` (surface) | `#F5F3EC` |
| `chiral` text | `#F5F3EC` (white) | `#111111` (black) |
| `-ftp` text | `#D9B65E` | `#A8791F` |
| Mark, light facet | `#F2D98A` | `#C79F43` |
| Mark, dark facet | `#C79F43` | `#8F6B1C` |

## Files
- `chiral-ftp-mark.svg`: mark only, for dark backgrounds. Transparent.
- `chiral-ftp-mark-light.svg`: mark only, deeper gold for light backgrounds. Transparent.
- `chiral-ftp-logo-dark.svg`: mark + wordmark, white `chiral` and gold `-ftp`, for dark backgrounds. Transparent.
- `chiral-ftp-logo-light.svg`: mark + wordmark, black `chiral` and gold `-ftp`, for light backgrounds. Transparent.
- `chiral-ftp-icon.svg`: 512 px app icon, rounded square `#0E0F11` with the mark centred.

The two lockup SVGs use live `<text>`, so they need IBM Plex Mono installed or loaded (e.g. via the Google Fonts link above) to render in the exact face. The mark files and icon contain no text and render identically everywhere.

## Usage
- Minimum size for the mark: 16 px (the two arrows stay distinct).
- Clear space around the mark: at least the width of one arrow shaft (about 1/12 of the mark's width).
- Don't recolour the facets, stretch the mark, or add effects such as gradients, glows or drop shadows. The style is flat.

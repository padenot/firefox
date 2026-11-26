# Reviewing a desktop theme patch

This is the entry point for reviewing a patch that changes front-end
stylesheets, design tokens or theme manifests. It lists, for each kind of
change in the diff, the documents that hold the checks the diff is reviewed
against. Read the ones that match what the diff changes before reviewing it,
then check the diff against each of them. Paths are relative to the root of the
repository.

## Every stylesheet change

Read `docs/code-quality/coding-style/css_guidelines.md` and treat it as a
checklist: check the diff against each of its rules, and cite the heading of
the section each finding comes from.

## Colors

For a diff that adds or changes a color, background, border, outline, shadow,
`fill` or `stroke`, a system color, a color token, or a `(forced-colors)`,
`(prefers-contrast)` or `(prefers-color-scheme)` rule:

- `.claude/skills/desktop-theme-colors/SKILL.md`: the regimes a color resolves
  in, what the forced-color adjustment does to a declaration, and pairing.
- `accessible/docs/HCMCSSChecklist.md`: apply its checks to every color the
  diff adds or modifies (foreground/background pairs, matching interaction
  states, borders, visual effects, SVG colors).
- `.claude/skills/desktop-theme-colors/references/system-color-pairs.md`: which
  system colors pair, per state and per selector.
- `.claude/skills/desktop-theme-colors/references/forced-color-filter.md`, §1
  and §3: what each declaration paints under forced colors.
- `toolkit/themes/shared/design-system/docs/README.design-tokens.stories.md`,
  "Chrome native OS theme" and "High Contrast Mode (HCM) media queries".
- `docs/code-quality/lint/linters/stylelint-plugin-mozilla/rules/use-paired-color-tokens.md`,
  for a background or text color set without its pair, including in a `:hover`
  or other state rule the lint rule skips.

## Layout and sizing

For a diff that moves, sizes or clips a box (dimensions, spacing, overflow,
positioning, popovers, anchors, fonts, direction):

- `.claude/skills/desktop-theme-layout/SKILL.md`.
- css_guidelines "Chrome versus in-content" and "UI density".
- `.claude/skills/desktop-theme-layout/references/positioning.md`, for a
  positioned box, a popover or an anchored box.
- `.claude/skills/desktop-theme-layout/references/environment-sweep.md`, §3:
  what system font size, scrollbars, scaling and writing direction do to a box.
- `docs/code-quality/coding-style/rtl_guidelines.md`, for anything that depends
  on direction.

## Design tokens and CSS variables

For a diff that edits a `*.tokens.json`, adds, renames or removes a CSS variable,
or hard-codes a value a token could provide:

- `.claude/skills/desktop-theme-tokens/SKILL.md`.
- css_guidelines "Using CSS variables" and "Adding new variables".
- README.design-tokens.stories.md "JSON format deep dive".
- `.claude/skills/desktop-theme-tokens/references/token-layers.md`, for a
  high-contrast value or a change that themes paint differently.
- `.claude/skills/desktop-theme-tokens/references/nova-overrides.md`, for a
  `nova` block or a surface Nova styles.
- `.claude/skills/desktop-theme-tokens/references/rem-roots.md`, for a
  rem-valued token.
- `.claude/skills/desktop-theme-tokens/references/theme-manifests.md`, for a
  built-in theme manifest.
- `docs/code-quality/lint/linters/stylelint-plugin-mozilla/rules/use-design-tokens.md`,
  and `use-design-tokens/space.md` beside it for spacing.

## With a build and a browser

When you can run a build and a browser, also run the live steps of the subject
skills above: probe the used values in each regime they name (they load
`.claude/skills/desktop-probing/SKILL.md` for the instruments), and take
before/after screenshots with `.claude/skills/desktop-theme-screenshots/SKILL.md`
when the result needs showing. Without them, review from the diff and the
source, and where a finding rests on a number only a live step can produce (a
contrast ratio, a resolved color, a size), ask the author for that measurement
rather than inferring it.

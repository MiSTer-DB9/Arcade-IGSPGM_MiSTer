# docs/comments — extended code commentary

Source comments in this repo are limited to critical exceptions and
warnings about unexpected or surprising behavior — things that will break
the next edit if unknown.  One or two lines.  Explanations, reasoning,
design narratives, measurement evidence, task context and project goals do
NOT belong in code comments; they belong here.

- One markdown file per source file, mirroring its path:
  `rtl/igs023.sv` -> `docs/comments/rtl/igs023.sv.md`.
- Read the matching commentary file before working on unfamiliar code, and
  add/update it in the same change when you alter the code it describes.
- Anchor commentary to code with `### \`symbol\`` headings naming the
  module/signal/function/state machine involved, optionally followed by a
  blockquoted copy of the key source line.  Do not use line numbers - they
  rot.

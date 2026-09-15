# LogitOS website

## Intent

A Chinese product introduction for people curious about independent operating
systems and native AI workflows. Primary action: explore the real desktop;
secondary action: read source and build instructions. This is a marketing page.

## Direction

Apple's large product framing and OpenAI's calm editorial narrative are references,
not assets to reuse. Reference pages supplied by the user:

- https://www.apple.com/iphone-18-pro/
- https://openai.com/zh-Hans-CN/index/gpt-6-astra/

The signature is a real guest desktop rising in front of an oversized LogitOS
metallic wordmark on a dark stage. A lilac/apricot light picks up the guest palette.
The desktop straightens with scroll, then approaches the real task sidebar.
The architecture chapter uses four pale dimensional plates that separate as the
reader advances. Wide margins, short Chinese headings and authentic screenshots
anchor the remaining editorial chapters. No fabricated performance numbers,
waitlist, download, or AI chat.

## Tokens and typography

Runtime owner: `styles/tokens.css`. Paper #FAFAFA, ink #242730, muted #70717C,
violet #64528E, lilac #B3A8EF, peach #ECC3B4. White is the raised surface;
Scene-specific surfaces live in `styles/scenes.css`: cinema #101117 and architecture
#EAE9EF. Colors must remain legible without gradient effects. Shared surface and
interaction colors derive from the base tokens.

Display: Helvetica Neue / PingFang SC; body: native system sans; utility: SF Mono /
Consolas. No remote fonts or requests. Hero statement 25–42 px; the decorative
wordmark reaches 290 px. Section title 35–52 px,
body 15–17 px. Desktop content width 1180 px; mobile side inset 20 px.

## Layout

Hero → project introduction → application tour → native task journey → OpenLogit
graphics → AetherScript and Studio → system layers → subsystem directory → human/AI
collaboration and FAQ → source/build CTA → project status. A full-width product narrative was
chosen over a two-column SaaS hero because the desktop is the artifact to inspect.

## Interaction

Native anchor scrolling, keyboard-operable application tabs, a native modal dialog
for image enlargement, and clipboard copy with a selectable fallback. No automatic
carousel, continuous canvas animation, or hidden content requiring animation.
Honor reduced motion and forced colors; maintain visible focus and scrollbars.

## Motion

At viewports at least 900 × 650, the opening stage pins below the navigation for
1900 px of scroll travel, moving through desktop, document and task phases. Scale,
perspective, crop and light follow actual scroll position. The final crop fits the
real task sidebar in the viewport. The architecture pins for 1500 px and separates
four plates, linking each stage to its explanation. Buttons scroll to phase
positions; ordinary anchor links let readers continue immediately. No wheel or
touch interception, playback clock or perpetual loop is used.

Small viewports and reduced motion use a complete static narrative with every
description visible and no long pinned tracks. Other sections reveal once with up
to 170 ms stagger, 720 ms duration and 26 px movement. Tour changes animate after
image decoding and reject stale decode completions. Modal, menu and FAQ transitions
are finite.

`scenes.js` owns deterministic scene transforms; `styles/scenes.css` owns the
enhanced and static scene layouts. `motion.js` owns Web Animations and the
event-driven scroll update; `styles/motion.css` owns CSS control transitions.
Base HTML/CSS remains visible;
focused groups are never faded. Reduced motion cancels current JS animations,
disables CSS transitions and removes scroll perspective. Hidden tabs stop scheduled
frames; an idle page schedules no new JS animation frames.

## Content boundary

This is an experimental OS. QEMU screenshots are identified as captures; the task
journey is identified as a schematic. Model tasks require service configuration.
Do not infer hardware compatibility from QEMU or general AI autonomy from the
specific document workflow. See `assets/README.md` for source provenance.

The expanded chapters distinguish AetherScript's evolving native subset from
completed self-hosting, OpenLogit software rendering from GPU compatibility, and
the dedicated Project disk from default startup. Keep subsystem facts and dated
evidence in `sections/evidence.html`; do not copy stale umbrella claims from old
README paragraphs. The homepage code sample is a source excerpt, not a simulated
terminal or a replacement for Code Studio's actual interface.

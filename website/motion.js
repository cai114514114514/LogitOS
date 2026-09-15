// SPDX-License-Identifier: MIT
import { createScenes } from "./scenes.js";
const preference = matchMedia("(prefers-reduced-motion: reduce)");
const active = new Map();
const easing = "cubic-bezier(0.2, 0.75, 0.3, 1)";

// Individual translate/scale properties preserve the product's perspective and
// hover transforms. No persistent hidden class: missing JS never hides content.
export function enter(
  element,
  { delay = 0, distance = 26, duration = 720 } = {},
) {
  active.get(element)?.cancel();
  if (
    preference.matches ||
    document.hidden ||
    !element.animate ||
    element.contains(document.activeElement)
  )
    return;
  const animation = element.animate(
    [
      { opacity: 0, translate: `0 ${distance}px` },
      { opacity: 1, translate: "0 0" },
    ],
    { duration, delay, easing, fill: "backwards" },
  );
  active.set(element, animation);
  const forget = () => {
    if (active.get(element) === animation) active.delete(element);
  };
  animation.addEventListener("finish", forget, { once: true });
  animation.addEventListener("cancel", forget, { once: true });
}

export function initMotion() {
  const revealGroups = [
    ".intro > .eyebrow, .intro .editorial-grid > *",
    ".experience .section-heading, .tour-controls, .tour-panel",
    ".intelligence-copy, .workflow, .section-footnote",
    ".graphics .section-heading, .graphics-gallery figure, .graphics-features article",
    ".language-copy, .code-sample",
    ".capabilities .section-heading, .capability-grid article",
    ".making .editorial-grid > *, .principles article, .project-questions",
    ".start-copy, .terminal-card",
  ];
  const delays = new Map();
  revealGroups.forEach((selector) => {
    document.querySelectorAll(selector).forEach((element, index) => {
      delays.set(element, Math.min(index % 3, 2) * 85);
    });
  });

  const observer =
    "IntersectionObserver" in window
      ? new IntersectionObserver(
          (entries) => {
            for (const entry of entries) {
              if (!entry.isIntersecting) continue;
              observer.unobserve(entry.target);
              // Anchor navigation and restored scroll can land above an element's
              // start. Do not fade a paragraph the person is already reading.
              if (entry.boundingClientRect.top < 80) continue;
              enter(entry.target, { delay: delays.get(entry.target) });
            }
          },
          { threshold: 0.08, rootMargin: "0px 0px -24px 0px" },
        )
      : null;
  delays.forEach((_, element) => observer?.observe(element));

  const paintScenes = createScenes();
  const header = document.querySelector(".site-header");
  let frame = 0;

  function paintScroll() {
    frame = 0;
    if (document.hidden) return;
    paintScenes(preference.matches);
    const length = document.documentElement.scrollHeight - innerHeight;
    header.style.setProperty(
      "--reading-progress",
      length > 0 ? Math.min(1, Math.max(0, scrollY / length)) : 0,
    );
  }

  // One frame per scroll/resize burst, with no perpetual animation loop. Once
  // input stops, there are no JS animation wakes; mobile omits the depth effect.
  function schedule() {
    if (!frame && !document.hidden) frame = requestAnimationFrame(paintScroll);
  }
  function settle() {
    active.forEach((animation) => animation.cancel());
    active.clear();
  }
  preference.addEventListener("change", () => {
    settle();
    schedule();
  });
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) {
      settle();
      cancelAnimationFrame(frame);
      frame = 0;
    } else schedule();
  });
  document.addEventListener("focusin", (event) => {
    // A keyboard jump must never focus a button in a still-transparent group.
    for (const [element, animation] of active) {
      if (element.contains(event.target)) animation.cancel();
    }
  });
  addEventListener("scroll", schedule, { passive: true });
  addEventListener("resize", schedule, { passive: true });
  addEventListener("pageshow", schedule);
  schedule();
}

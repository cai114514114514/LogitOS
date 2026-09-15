// SPDX-License-Identifier: MIT
const clamp = (value) => Math.max(0, Math.min(1, value));
const mix = (a, b, amount) => a + (b - a) * amount;
const smooth = (value) => {
  const x = clamp(value);
  return x * x * (3 - 2 * x);
};

// Each scene samples actual scroll position. There is no accumulated timeline,
// wheel interception or playback clock: reverse scroll, anchors and restoration
// must produce the same frame as forward scrolling to that exact position.
export function createScenes() {
  const cinema = document.querySelector(".cinema");
  const architecture = document.querySelector(".architecture");
  const stage = cinema.querySelector(".cinema-stage");
  const device = cinema.querySelector(".cinema-device");
  const word = cinema.querySelector(".cinema-word");
  const title = cinema.querySelector(".cinema-title");
  const light = cinema.querySelector(".cinema-light");
  const focus = cinema.querySelector(".cinema-focus");
  const captions = [...cinema.querySelectorAll("[data-desktop-copy]")];
  const desktopSteps = [...cinema.querySelectorAll("[data-desktop-step]")];
  const plates = [...architecture.querySelectorAll("[data-plate]")];
  const notes = [...architecture.querySelectorAll("[data-stack-copy]")];
  const stackSteps = [...architecture.querySelectorAll("[data-stack-step]")];
  const header = document.querySelector(".site-header");
  let enabled = null;

  function position(section) {
    const top = header.offsetHeight;
    const bounds = section.getBoundingClientRect();
    const travel = Math.max(1, section.offsetHeight - (innerHeight - top));
    return {
      progress: clamp((top - bounds.top) / travel),
      visible: bounds.bottom > top && bounds.top < innerHeight,
    };
  }

  function selectCopy(items, buttons, index) {
    items.forEach((item, itemIndex) => {
      const hidden = enabled && itemIndex !== index;
      if (item.hidden !== hidden) item.hidden = hidden;
    });
    buttons.forEach((button, itemIndex) => {
      button.setAttribute("aria-pressed", String(itemIndex === index));
    });
  }

  function jump(section, progress) {
    const travel = section.offsetHeight - (innerHeight - header.offsetHeight);
    scrollTo({
      top:
        scrollY +
        section.getBoundingClientRect().top -
        header.offsetHeight +
        travel * progress,
      behavior: "smooth",
    });
  }
  desktopSteps.forEach((button, index) => {
    button.addEventListener("click", () => jump(cinema, [0, 0.46, 0.9][index]));
  });
  stackSteps.forEach((button, index) => {
    button.addEventListener("click", () =>
      jump(architecture, [0.08, 0.35, 0.62, 0.93][index]),
    );
  });

  function setMode(reduced) {
    const next = !reduced && innerWidth >= 900 && innerHeight >= 650;
    if (next === enabled) return;
    enabled = next;
    document.documentElement.classList.toggle("scroll-scenes", enabled);
    [...desktopSteps, ...stackSteps].forEach((button) => {
      button.disabled = !enabled;
    });
    if (!enabled) {
      [device, word, title, light, focus, ...plates].forEach((element) =>
        element.removeAttribute("style"),
      );
      [...captions, ...notes].forEach((element) => {
        element.hidden = false;
      });
    }
  }

  function paintDesktop(progress) {
    const unfold = smooth(progress / 0.4);
    const approach = smooth((progress - 0.48) / 0.38);
    const width = stage.clientWidth;
    // The final crop magnifies the actual task sidebar (right-hand portion of
    // the source screenshot). It adds no fictional controls or generated UI.
    const closeScale = Math.min(
      1.35,
      (stage.clientHeight - 95) / device.offsetHeight,
    );
    const scale = mix(0.8, 1, unfold) + approach * (closeScale - 1);
    const rise = mix(70, 5, unfold) - approach * 100;
    device.style.transform = [
      "translate(-50%, -42%)",
      `translate3d(${width * 0.05 * approach}px, ${rise}px, 0)`,
      "perspective(1800px)",
      `rotateX(${mix(28, 0, unfold)}deg)`,
      `rotateY(${mix(-12, 0, unfold)}deg)`,
      `rotateZ(${mix(-4, 0, unfold)}deg)`,
      `scale(${scale})`,
    ].join(" ");
    device.style.transformOrigin = `${mix(50, 82, approach)}% 45%`;
    word.style.transform = `translateY(${-unfold * 80}px) scale(${1 + unfold * 0.16})`;
    word.style.opacity = String(1 - smooth(progress / 0.42));
    title.style.opacity = String(1 - smooth((progress - 0.04) / 0.2));
    title.style.transform = `translateY(${-unfold * 35}px)`;
    light.style.transform = `translateX(${mix(-15, 20, progress)}%) scale(${mix(1, 1.4, progress)})`;
    focus.style.opacity = String(approach);
    cinema.style.setProperty("--scene-progress", progress);
    const index = progress < 0.32 ? 0 : progress < 0.72 ? 1 : 2;
    selectCopy(captions, desktopSteps, index);
    cinema.classList.toggle("scene-closeup", progress >= 0.72);
    cinema.classList.toggle("scene-unfolded", progress >= 0.25);
    desktopSteps.forEach((button, i) =>
      button.style.setProperty("--step-progress", clamp(progress * 3 - i)),
    );
  }

  function paintStack(progress) {
    const spread = smooth((progress - 0.02) / 0.84);
    const index = Math.min(3, Math.floor(progress * 4));
    const spacing = Math.min(114, (innerHeight - header.offsetHeight) * 0.15);
    plates.forEach((plate) => {
      const level = Number(plate.dataset.plate);
      const offset = (level - 1.5) * mix(18, spacing, spread);
      plate.style.transform = `translate(-50%, -50%) translate3d(${mix(0, level * 8, spread)}px, ${-offset}px, ${level * 3}px) rotateX(${mix(62, 47, spread)}deg) rotateZ(${mix(-32, -18, spread)}deg)`;
      plate.style.setProperty("--plate-light", level === index ? 1 : 0);
    });
    selectCopy(notes, stackSteps, index);
  }

  return function paint(reduced) {
    setMode(reduced);
    const desktop = position(cinema);
    header.classList.toggle(
      "on-dark",
      cinema.getBoundingClientRect().bottom > header.offsetHeight,
    );
    if (!enabled) return;
    if (desktop.visible) paintDesktop(desktop.progress);
    const stack = position(architecture);
    if (stack.visible) paintStack(stack.progress);
  };
}

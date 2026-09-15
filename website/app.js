// SPDX-License-Identifier: MIT
import { initTour } from "./tour.js";
import { initMotion } from "./motion.js";

initTour();
initMotion();

const menuButton = document.querySelector("#menu-toggle");
const navLinks = document.querySelector("#nav-links");
function setMenu(open) {
  menuButton.setAttribute("aria-expanded", String(open));
  menuButton.setAttribute("aria-label", open ? "收起导航菜单" : "展开导航菜单");
  navLinks.classList.toggle("is-open", open);
}
menuButton.addEventListener("click", () => {
  setMenu(menuButton.getAttribute("aria-expanded") !== "true");
});
navLinks.addEventListener("click", (event) => {
  if (event.target.closest("a")) setMenu(false);
});
document.addEventListener("keydown", (event) => {
  if (
    event.key === "Escape" &&
    menuButton.getAttribute("aria-expanded") === "true"
  ) {
    setMenu(false);
    menuButton.focus();
  }
});
menuButton.disabled = false;

const copyButton = document.querySelector("#copy-command");
const copyStatus = document.querySelector("#copy-status");
copyButton.addEventListener("click", async () => {
  const command = document.querySelector("#start-command");
  copyButton.disabled = true;
  try {
    await navigator.clipboard.writeText(command.textContent);
    copyStatus.textContent = "启动命令已复制。";
  } catch {
    // Clipboard can be denied by browser policy or a non-secure preview host.
    // Keep the command selectable and never report success on that path.
    const range = document.createRange();
    range.selectNodeContents(command);
    const selection = window.getSelection();
    selection.removeAllRanges();
    selection.addRange(range);
    copyStatus.textContent = "自动复制未获允许，命令已选中，请手动复制。";
  } finally {
    copyButton.disabled = false;
  }
});
copyButton.disabled = false;

// Observe sections without a scroll loop or scroll hijacking. The page remains
// fully readable with JavaScript disabled and with reduced motion enabled.
const sectionLinks = [...document.querySelectorAll('.nav-links a[href^="#"]')];
if ("IntersectionObserver" in window) {
  const observer = new IntersectionObserver(
    (entries) => {
      for (const entry of entries) {
        const link = sectionLinks.find(
          (item) => item.hash === `#${entry.target.id}`,
        );
        if (entry.isIntersecting) link.setAttribute("aria-current", "location");
        else link.removeAttribute("aria-current");
      }
    },
    { rootMargin: "-20% 0px -60% 0px" },
  );
  sectionLinks.forEach((link) =>
    observer.observe(document.querySelector(link.hash)),
  );
}

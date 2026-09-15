// SPDX-License-Identifier: MIT
import { enter } from "./motion.js";
// Copy and image provenance stay paired here: a new scene must show an actual
// guest capture, rather than a designed interface presented as working software.
const scenes = {
  textedit: {
    app: "TEXTEDIT",
    title: "想法落笔，修改有据。",
    description:
      "在文档里提出修改目标，查看带有来源的候选内容，再决定是否应用。智能协作，就在你正在做的工作旁边。",
    image: "./assets/textedit.png",
    alt: "TextEdit 原生文档编辑器与任务侧栏，展示带来源引用的项目摘要",
  },
  finder: {
    app: "FINDER",
    title: "从一份资料，到一项任务。",
    description:
      "在文件所在的位置发起工作。Finder 的原生任务入口，把你的目标与已有资料放在一起，让后续处理有据可依。",
    image: "./assets/finder.png",
    alt: "Finder 原生文件窗口中的资料与已发送的任务",
  },
  browser: {
    app: "BROWSER",
    title: "从自己的窗口，望向网络。",
    description:
      "用内置浏览器探索网页。HTML 解析、页面布局与脚本交互在系统中逐步连接，兼容性也在一次次真实页面验证中向前。",
    image: "./assets/browser.png",
    alt: "LogitOS 内置浏览器显示引擎组件工作台，包含 Flex 与 Grid 布局示例",
  },
};

export function initTour() {
  const tabs = [...document.querySelectorAll("[data-scene]")];
  const panel = document.querySelector("#tour-panel");
  const picture = document.querySelector("#tour-image");
  const dialog = document.querySelector("#image-dialog");
  const expand = document.querySelector("#expand-image");
  let revision = 0;

  function selectScene(tab) {
    if (tab.getAttribute("aria-selected") === "true") return;
    const selection = ++revision;
    const scene = scenes[tab.dataset.scene];
    for (const item of tabs) {
      const selected = item === tab;
      item.setAttribute("aria-selected", String(selected));
      item.tabIndex = selected ? 0 : -1;
    }
    panel.setAttribute("aria-labelledby", tab.id);
    picture.src = scene.image;
    picture.alt = scene.alt;
    document.querySelector("#tour-app").textContent = scene.app;
    document.querySelector("#tour-title").textContent = scene.title;
    document.querySelector("#tour-description").textContent = scene.description;
    // Decoding can finish out of order under rapid keyboard navigation. Only
    // the current selection may animate; state and accessible labels update now.
    picture
      .decode()
      .then(() => {
        if (selection !== revision) return;
        enter(picture, { distance: 10, duration: 420 });
        enter(document.querySelector(".tour-caption"), {
          distance: 5,
          duration: 360,
        });
      })
      .catch(() => {
        /* The native image error/alt remains visible; no fake scene. */
      });
  }

  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => selectScene(tab));
    tab.addEventListener("keydown", (event) => {
      const nextIndex = {
        ArrowRight: (index + 1) % tabs.length,
        ArrowLeft: (index + tabs.length - 1) % tabs.length,
        Home: 0,
        End: tabs.length - 1,
      }[event.key];
      if (nextIndex === undefined) return;
      event.preventDefault();
      tabs[nextIndex].focus();
      selectScene(tabs[nextIndex]);
    });
  });

  expand.addEventListener("click", () => {
    const enlarged = document.querySelector("#dialog-image");
    enlarged.src = picture.src;
    enlarged.alt = picture.alt;
    document.querySelector("#image-dialog-title").textContent =
      `${document.querySelector("#tour-app").textContent} · 系统实拍`;
    dialog.showModal();
  });
  document
    .querySelector("#close-image")
    .addEventListener("click", () => dialog.close());
  // Native dialog supplies inert background, focus containment and Escape.
  // Restore the trigger explicitly so keyboard users continue where they left off.
  dialog.addEventListener("close", () => expand.focus({ preventScroll: true }));
  dialog.addEventListener("click", (event) => {
    const bounds = dialog.getBoundingClientRect();
    if (
      event.clientX < bounds.left ||
      event.clientX > bounds.right ||
      event.clientY < bounds.top ||
      event.clientY > bounds.bottom
    )
      dialog.close();
  });
  // Progressive enhancement: controls become available only after their handlers
  // exist. Without JS the screenshots and the noscript image links still work.
  [...tabs, expand, document.querySelector("#close-image")].forEach(
    (button) => {
      button.disabled = false;
    },
  );
}

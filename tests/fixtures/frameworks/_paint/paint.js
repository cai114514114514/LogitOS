/* Did the framework actually put anything on the page?
 *
 * "Its scripts ran clean" is not "it rendered". React and Vite produce zero
 * uncaught exceptions in this corpus, and that number on its own would support
 * either "they work" or "they silently mounted nothing" -- which are opposite
 * findings. So this module runs AFTER the application's own module (document
 * order is evaluation order for modules) and reports what is in the DOM.
 *
 * Reported twice, immediately and after a microtask turn, because a framework
 * that bootstraps through a promise -- Angular does -- has not mounted yet at
 * the moment this module's body runs, and reporting only the first would call
 * that a blank page. */
function report(when) {
  var body = document.body;
  var html = body ? (body.innerHTML || "") : "";
  var mounts = ["root", "app", "app-root", "__next"];
  var found = "";
  for (var i = 0; i < mounts.length; i++) {
    var el = document.getElementById(mounts[i]);
    if (el) found += " #" + mounts[i] + "=" + (el.innerHTML || "").length;
  }
  var btn = document.getElementById("inc");
  var lazy = document.getElementById("lazy");
  console.log("#PAINT\t" + when + "\tbody=" + html.length + found +
              "\tbutton=" + (btn ? JSON.stringify(btn.textContent || "") : "none") +
              "\tlazy=" + (lazy ? JSON.stringify(lazy.textContent || "") : "none"));
}
report("sync");
Promise.resolve().then(function () { report("microtask"); });

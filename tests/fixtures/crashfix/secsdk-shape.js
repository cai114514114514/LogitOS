/* secsdk-shape.js -- the douyin specimen's crashing function, reduced to the
 * statement that threw. The full specimen is committed beside this as
 * douyin-runtime_bundler_34.js; the crash site is byte 38277:
 *
 *   {key:"init",value:function(){
 *       var e=document.currentScript.getAttribute("project-id"),
 *           t=document.currentScript.getAttribute("custom-report-host");
 *       ...
 *
 * which ran synchronously during the script's own evaluation (scoreboard
 * collapse-nocs, 2026-08-30: "TypeError: cannot read property 'getAttribute'
 * of null at value (runtime_bundler_34.js) ... at <eval> (:7)"). Per HTML, a
 * classic script's document.currentScript during its own execution IS the
 * element; it was null here only because the embedder handed js_page_eval no
 * node, so currentScript was null for EVERY dynamically inserted script.
 */
var pid = document.currentScript.getAttribute("project-id");
var host = document.currentScript.getAttribute("custom-report-host");
console.log('SECSDK-SHAPE currentScript non-null, project-id=' + pid +
            ' custom-report-host=' + host);

export function text(id, value) { document.getElementById(id).textContent = String(value); }
var lines = [];
export function log(value) {
    lines.push(String(value));
    if (lines.length > 30) lines.shift();
    text('events', lines.join('\n'));
}
export function on(id, fn) {
    document.getElementById(id).addEventListener('click', function () {
        try { fn(); } catch (e) { log(id + ': ' + e.name + ': ' + e.message); }
    });
}

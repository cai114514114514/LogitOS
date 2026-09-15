import {text, log, on} from 'workbench/ui.js';

text('module-status', '模块已执行：import map → 静态依赖 → 页面 DOM 更新');
log('module mounted');
var count = 0, itemCount = 1, timer = 0;
on('count', function () { text('count-output', ++count); log('click count=' + count); });
on('add-item', function () {
    var li = document.createElement('li'); li.textContent = '新增项 ' + (++itemCount);
    document.getElementById('items').appendChild(li); log('DOM append');
});
on('remove-item', function () {
    var list = document.getElementById('items');
    if (list.lastElementChild) list.removeChild(list.lastElementChild);
    log('DOM remove');
});
on('form-state', function () {
    var form = document.getElementById('sample-form');
    var select = form.querySelector('select');
    text('form-output', 'text=' + form.querySelector('input[name=text]').value +
        '\nselectedIndex=' + select.selectedIndex + '\nselect.value=' + select.value);
    log('read native form state');
});
on('show-dialog', function () { document.getElementById('dialog').showModal(); log('showModal returned; inspect modality manually'); });
on('close-dialog', function () { document.getElementById('dialog').close(); log('dialog close'); });
on('schedule', function () {
    clearTimeout(timer); text('task-output', '已安排 300 ms timer');
    timer = setTimeout(function () {
        log('timer fired');
        Promise.resolve().then(function () {
            log('microtask fired');
            requestAnimationFrame(function () { text('task-output', 'timer → microtask → rAF 已执行'); log('rAF fired'); });
        });
    }, 300);
});
on('cancel', function () { clearTimeout(timer); text('task-output', '已调用 clearTimeout；待执行的 timer 不应继续追加记录'); log('timer cancelled'); });
on('save', function () {
    var value = document.getElementById('storage-value').value;
    localStorage.setItem('engine-components', value);
    sessionStorage.setItem('engine-components', value);
    text('storage-output', '写入完成，点击读取或切换标签页验证'); log('storage set');
});
on('read', function () {
    text('storage-output', 'local=' + localStorage.getItem('engine-components') +
        '; session=' + sessionStorage.getItem('engine-components')); log('storage read');
});
on('fetch', function () {
    fetch('sample.json').then(function (r) { if (!r.ok) throw Error('HTTP ' + r.status); return r.json(); })
        .then(function (v) { text('resource-output', JSON.stringify(v)); log('fetch body decoded'); })
        .catch(function (e) { text('resource-output', String(e)); log('fetch failed: ' + e); });
});
on('dynamic', function () {
    import('workbench/extra.js').then(function (m) { text('resource-output', m.message); log('dynamic import executed'); })
        .catch(function (e) { text('resource-output', String(e)); log('dynamic import failed: ' + e); });
});
try {
    var canvas = document.getElementById('canvas');
    var ctx = canvas.getContext('2d');
    ctx.fillStyle = '#146a72'; ctx.fillRect(0, 0, 110, 90);
    ctx.fillStyle = '#d6a451'; ctx.fillRect(120, 0, 110, 90);
    var pixels = ctx.getImageData(10, 10, 1, 1).data;
    text('canvas-output', '已执行绘制与回读，首色 RGBA=' + Array.prototype.join.call(pixels, ','));
    log('canvas draw/readback');
} catch (e) { text('canvas-output', e.name + ': ' + e.message); log('canvas unavailable: ' + e); }

'use strict';
var controller = new AbortController();
var signal = AbortSignal.any([controller.signal]);
controller.abort(false);
fetch('/worker-hold', { signal: signal }).then(function () {
  postMessage(false);
}, function (reason) {
  postMessage(reason === false && signal.aborted && typeof AbortSignal.timeout === 'function');
});

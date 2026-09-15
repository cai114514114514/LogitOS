// SPDX-License-Identifier: MIT
// The same ordinary page runs through the guest browser and the host transport
// fixture. The server sets responseBoundaryOrigin to a DIFFERENT origin.
var responseBoundary = { checks: 0, failures: 0, done: false };
(function () {
  function check(ok, name) {
    responseBoundary.checks++;
    if (!ok) responseBoundary.failures++;
    if (typeof responseBoundaryRecord === 'function') responseBoundaryRecord(!!ok, name);
    else console.log('RESPONSE-CHECK ' + (ok ? 'OK ' : 'FAIL ') + name);
  }
  function throws(fn, kind) {
    try { fn(); return false; } catch (e) { return e instanceof kind; }
  }
  function immutable(r) {
    return throws(function () { r.headers.append('x-edit', 'a'); }, TypeError) &&
      throws(function () { r.headers.set('x-edit', 'b'); }, TypeError) &&
      throws(function () { r.headers.delete('x-secret'); }, TypeError);
  }
  function hidden(r) {
    return r.status === 0 && r.statusText === '' && r.url === '' &&
      !r.ok && !r.redirected && r.type === 'opaque' && r.body === null &&
      Array.from(r.headers).length === 0;
  }
  check(throws(function () { new Response(null, { status: 0 }); }, RangeError),
    'public status zero refused');
  check(throws(function () { new Response(null, { status: 0, __allowStatus0: true }); }, RangeError),
    'public internal flag cannot bypass validation');
  check(throws(function () { new Response(null, { status: 700, __allowStatus0: true }); }, RangeError),
    'public internal flag cannot admit invalid status');
  var ordinary = new Response('local', { status: 201, type: 'opaque',
    url: 'https://forged.invalid/', redirected: true });
  check(ordinary.status === 201 && ordinary.ok, 'public valid status preserved');
  check(ordinary.type === 'default' && ordinary.url === '' && !ordinary.redirected,
    'public init ignores internal metadata');
  var init = { status: 200 };
  ['type', 'url', 'redirected', '__allowStatus0'].forEach(function (k) {
    Object.defineProperty(init, k, { get: function () { throw Error('unexpected init read'); } });
  });
  var ignored = false;
  try { ignored = new Response(null, init).type === 'default'; } catch (e) {}
  check(ignored, 'public init never reads unknown members');
  ordinary.headers.set('x-edit', 'yes');
  var copy = ordinary.clone();
  copy.headers.set('x-edit', 'copy');
  check(ordinary.headers.get('x-edit') === 'yes' && copy.headers.get('x-edit') === 'copy',
    'public clone headers stay independent and writable');
  var err = Response.error(), errCopy = err.clone();
  check(err.status === 0 && err.type === 'error' && err.body === null,
    'error response still constructible internally');
  check(immutable(err) && immutable(errCopy), 'error clone retains immutable headers');
  var redir = Response.redirect('/payload', 302);
  check(redir.status === 302 && redir.type === 'default' && !redir.redirected,
    'static redirect metadata preserved');
  check(immutable(redir) && immutable(redir.clone()), 'static redirect headers immutable');

  fetch(responseBoundaryOrigin + '/payload', { mode: 'no-cors' }).then(function (r) {
    check(true, 'opaque fetch resolves');
    check(hidden(r), 'opaque response hides status headers body and URL');
    check(immutable(r), 'opaque headers immutable');
    var clone = r.clone();
    check(hidden(clone) && immutable(clone), 'opaque clone retains filter and guard');
    return Promise.all([r.text(), clone.arrayBuffer()]).then(function (parts) {
      check(parts[0] === '' && parts[1].byteLength === 0 && !r.bodyUsed,
        'opaque body readers reveal no bytes');
    });
  }, function (e) { check(false, 'opaque fetch resolves'); }).then(function () {
    return fetch(responseBoundaryOrigin + '/redirect', { mode: 'no-cors' });
  }).then(function (r) {
    check(hidden(r), 'opaque redirect history stays hidden');
  }, function () { check(false, 'opaque redirect history stays hidden'); }).then(function () {
    return fetch('/payload');
  }).then(function (r) {
    check(r.status === 200 && r.type === 'basic' && r.url.indexOf('/payload') >= 0,
      'same origin network metadata preserved');
    check(immutable(r) && immutable(r.clone()), 'network clone retains immutable headers');
    return r.text();
  }).then(function (body) {
    check(body === 'secret payload', 'same origin body remains readable');
    return fetch(responseBoundaryOrigin + '/payload');
  }).then(function () { check(false, 'ordinary CORS denial retained'); }, function (e) {
    check(e instanceof TypeError, 'ordinary CORS denial retained');
  }).then(function () {
    return fetch('data:text/plain,data-body');
  }).then(function (r) {
    check(r.status === 200 && r.type === 'basic' && r.url.indexOf('data:') === 0,
      'data response metadata preserved');
    return r.text();
  }).then(function (body) {
    check(body === 'data-body', 'data response body preserved');
    var url = URL.createObjectURL(new Blob(['blob-body'], { type: 'text/plain' }));
    return fetch(url).then(function (r) {
      check(r.status === 200 && r.type === 'basic' && r.url === url,
        'blob response metadata preserved');
      return r.text();
    }).then(function (body) {
      URL.revokeObjectURL(url);
      check(body === 'blob-body', 'blob response body preserved');
    });
  }).catch(function (e) { check(false, 'unexpected exception: ' + e); }).then(function () {
    responseBoundary.done = true;
    var result = 'RESPONSE-BOUNDARY checks=' + responseBoundary.checks +
      ' failures=' + responseBoundary.failures;
    if (typeof document !== 'undefined' && document.getElementById) {
      document.getElementById('result').textContent = result;
      document.getElementById('details').textContent = responseBoundary.failures ?
        'Response boundary checks failed.' : 'Opaque fetch, cloning and constructor boundaries passed.';
    }
    if (typeof console !== 'undefined') console.log(result);
  });
})();

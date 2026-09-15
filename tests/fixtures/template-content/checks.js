/* Self-authored finite DOM compatibility checks: no site code or network. */
(() => {
  const results = [];
  function test(name, expected, operation) {
    let actual;
    try { actual = operation(); }
    catch (e) { actual = {unexpectedException: String(e)}; }
    results.push({name, expected, actual,
      pass: JSON.stringify(actual) === JSON.stringify(expected)});
  }
  const svg = 'http://www.w3.org/2000/svg';
  const math = 'http://www.w3.org/1998/Math/MathML';
  const firstName = n => n.firstChild ? n.firstChild.localName : null;
  test('ordinary empty firstChild', null,
    () => document.createElement('div').firstChild);
  test('ordinary HTML insertion', 'span', () => {
    const d = document.createElement('div'); d.innerHTML = '<span>one</span>';
    return firstName(d);
  });
  test('createElementNS SVG case and namespace', ['clipPath', svg], () => {
    const d = document.createElementNS(svg, 'clipPath');
    return [d.localName, d.namespaceURI];
  });
  test('createElementNS MathML namespace', ['mi', math], () => {
    const d = document.createElementNS(math, 'mi');
    return [d.localName, d.namespaceURI];
  });
  test('fresh template content identity and node type', [true, 11, null], () => {
    const t = document.createElement('template'); const f = t.content;
    return [f === t.content, f.nodeType, f.firstChild];
  });
  test('first assignment before content read', 'span', () => {
    const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
    return firstName(t.content);
  });
  test('content read before first assignment', [true, 'span'], () => {
    const t = document.createElement('template'); const f = t.content;
    t.innerHTML = '<span>one</span>'; return [f === t.content, firstName(f)];
  });
  test('second assignment replaces same content fragment', [true, 'b'], () => {
    const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
    const f = t.content; t.innerHTML = '<b>two</b>';
    return [f === t.content, firstName(f)];
  });
  test('template content insertion drains fragment', ['span', null], () => {
    const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
    const f = t.content; const d = document.createElement('div');
    d.appendChild(f); return [firstName(d), f.firstChild];
  });
  for (const [kind, markup, tag, uri] of [
    ['SVG', '<svg><path d="M0 0L1 1"></path></svg>', 'svg', svg],
    ['MathML', '<math><mi>x</mi></math>', 'math', math]
  ]) {
    test('fresh ' + kind + ' wrapper in template', [tag, uri], () => {
      const t = document.createElement('template'); t.innerHTML = markup;
      const n = t.content.firstChild;
      return n ? [n.localName, n.namespaceURI] : null;
    });
    test('reused template repopulates ' + kind + ' wrapper', [true, tag, uri], () => {
      const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
      const f = t.content; document.createElement('div').appendChild(f);
      t.innerHTML = markup; const n = t.content.firstChild;
      /* Guard the observation; this fixture does not dereference null. */
      return [f === t.content, n ? n.localName : null, n ? n.namespaceURI : null];
    });
  }
  test('template innerHTML serializes content after first read', '<span>one</span>', () => {
    const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
    const f = t.content; return t.innerHTML;
  });


  test('saved Element accessors update retained content', [true, 'b', '<b>two</b>'], () => {
    const descriptor = Object.getOwnPropertyDescriptor(Element.prototype, 'innerHTML');
    const t = document.createElement('template'); const f = t.content;
    descriptor.set.call(t, '<b>two</b>');
    return [f === t.content, firstName(f), descriptor.get.call(t)];
  });
  test('internal accessors ignore a public content shadow', ['span', '<span>one</span>'], () => {
    const descriptor = Object.getOwnPropertyDescriptor(Element.prototype, 'innerHTML');
    const t = document.createElement('template'); const f = t.content;
    Object.defineProperty(t, 'content', {value: document.createDocumentFragment()});
    descriptor.set.call(t, '<span>one</span>');
    return [firstName(f), descriptor.get.call(t)];
  });
  test('template table parsing keeps template context', ['tr', 'td', 'cell'], () => {
    const t = document.createElement('template');
    t.innerHTML = '<tr><td>cell</td></tr>';
    const row = t.content.firstChild;
    return row && row.firstChild ? [row.localName, row.firstChild.localName, row.textContent] : null;
  });
  test('read a pre-parsed template then replace its content', [true, 'b'], () => {
    const d = document.createElement('div');
    d.innerHTML = '<template><span>one</span></template>';
    const t = d.firstChild; const f = t.content;
    t.innerHTML = '<b>two</b>'; return [f === t.content, firstName(f)];
  });
  test('ordinary namespace wrapper children insert before anchor', ['path', 'circle', 'marker', 0], () => {
    const t = document.createElement('template');
    t.innerHTML = '<svg><path></path><circle></circle></svg>';
    const f = t.content; const wrapper = f.firstChild;
    if (!wrapper) return null;
    const children = Array.from(wrapper.childNodes);
    for (const child of children) f.appendChild(child);
    f.removeChild(wrapper);
    const target = document.createElementNS(svg, 'svg');
    const anchor = document.createElementNS(svg, 'marker'); target.appendChild(anchor);
    target.insertBefore(f, anchor);
    return [target.firstChild.localName, target.firstChild.nextSibling.localName,
      target.lastChild.localName, f.childNodes.length];
  });


  test('unmaterialized template keeps outer serialization', '<template><span>one</span></template>', () => {
    const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
    return t.outerHTML;
  });
  test('unmaterialized template keeps container serialization', '<template><span>one</span></template>', () => {
    const t = document.createElement('template'); t.innerHTML = '<span>one</span>';
    const d = document.createElement('div'); d.appendChild(t);
    const own = t.innerHTML;
    return d.innerHTML;
  });

  const passed = results.filter(r => r.pass).length;
  globalThis.templateContentResult = {checks: results.length, passed, failed: results.length - passed, results};
  return JSON.stringify(templateContentResult);
})()

/* The JavaScript half of the WebAssembly API.  js_wasm.c evaluates this and
 * calls the resulting function with one argument, `P`, the object of C
 * primitives.
 *
 * WHY THE SPLIT IS HERE AND NOT SOMEWHERE ELSE.  Everything below is a rule
 * about JavaScript -- constructor shapes and `new.target`, accessor
 * properties, the exact class of error each failure raises, the order in which
 * a promise settles, and which failures are TypeError (the caller passed
 * nonsense) against LinkError (the module and the imports do not fit).  Those
 * are far easier to get right in the language that defines them than through
 * the C API, and js_websocket.c established the convention in this directory.
 * The C side stays what it should be: bytes, handles and value conversion.
 *
 * THE ERROR CLASS IS PART OF THE CONTRACT, not decoration.  A page that does
 * `catch (e) { if (e instanceof WebAssembly.LinkError) ... }` is asking a
 * question with three possible answers, and answering "Error" to all of them
 * is a plausible wrong answer.  So: a bad ARGUMENT is a TypeError, a module
 * that will not decode or validate is a CompileError, imports that do not fit
 * are a LinkError, and a trap at run time is a RuntimeError.
 */
"(function (P) {\n"
"'use strict';\n"
"var G = (typeof globalThis !== 'undefined') ? globalThis : this;\n"
"if (typeof G.WebAssembly !== 'undefined') return;\n"
"\n"
/* ---- the three error types -------------------------------------------- */
"class CompileError extends Error {\n"
"  constructor(m) { super(m); this.name = 'CompileError'; }\n"
"}\n"
"class LinkError extends Error {\n"
"  constructor(m) { super(m); this.name = 'LinkError'; }\n"
"}\n"
"class RuntimeError extends Error {\n"
"  constructor(m) { super(m); this.name = 'RuntimeError'; }\n"
"}\n"
"\n"
/* Private state.  WeakMaps rather than properties on the objects: a page can
 * overwrite a property, and `mem.__handle = 3` pointing at somebody else's
 * linear memory is a way to read another module's bytes.  A WeakMap key
 * cannot be forged from script. */
"const HND = new WeakMap();   /* public object -> {h, tok, ...} */\n"
"const EXP = new WeakMap();   /* exported function -> {ih, fidx, inst} */\n"
/* There is deliberately NO handle->object map here.  One was tried and it was
 * wrong twice over: a Map keyed by handle is STRONG, so every Memory a page
 * ever made stayed alive forever, and handles are RECYCLED when a slot is
 * freed -- so a stale entry could hand out a Memory object aliasing somebody
 * else's linear memory.  Identity comes from where the object actually is
 * instead: an imported Memory is the very object the page passed in (carried
 * through buildImports), and a defined one is created exactly once while the
 * exports object is being built. */
"\n"
/* A BufferSource becomes a private ArrayBuffer copy, and the copy is REQUIRED
 * rather than defensive: a wasm_module is a set of spans INTO the bytes, so
 * the decoder's output stays valid only as long as those exact bytes do.  A
 * page is free to write over its own ArrayBuffer the instruction after
 * compiling, and a page that transfers it to a Worker detaches it outright. */
"function toBytes(src, who) {\n"
"  let u8;\n"
"  if (src instanceof ArrayBuffer) u8 = new Uint8Array(src);\n"
"  else if (ArrayBuffer.isView(src))\n"
"    u8 = new Uint8Array(src.buffer, src.byteOffset, src.byteLength);\n"
"  else throw new TypeError(who + ': expected a BufferSource (ArrayBuffer or "
        "ArrayBufferView)');\n"
"  return u8.slice().buffer;\n"
"}\n"
"\n"
/* The spec's ToNonWrappingUint32 for descriptor fields: an absent field is not
 * the same as 0, and a negative or fractional one is a TypeError rather than
 * something to round. */
"function u32field(d, name, dflt) {\n"
"  let v = d[name];\n"
"  if (v === undefined) {\n"
"    if (dflt === undefined) throw new TypeError('WebAssembly: ' + name + "
        "' is required');\n"
"    return dflt;\n"
"  }\n"
"  v = Number(v);\n"
"  if (!Number.isInteger(v) || v < 0 || v > 4294967295)\n"
"    throw new TypeError('WebAssembly: ' + name + ' must be a non-negative "
        "integer');\n"
"  return v;\n"
"}\n"
"\n"
"function need(r, Klass, who) {\n"
"  if (r && r.err !== undefined) throw new Klass(who + r.err);\n"
"  return r;\n"
"}\n"
"\n"
/* ---- Module ------------------------------------------------------------ */
"class Module {\n"
"  constructor(bytes) {\n"
"    if (!new.target) throw new TypeError(\"WebAssembly.Module requires 'new'\");\n"
"    const ab = toBytes(bytes, 'WebAssembly.Module');\n"
"    const r = P.compile(ab);\n"
"    if (r.err !== undefined) throw new CompileError(r.err);\n"
"    HND.set(this, { h: r.h, tok: r.tok, info: P.modInfo(r.h) });\n"
"  }\n"
"  static exports(m) {\n"
"    const s = HND.get(m);\n"
"    if (!s) throw new TypeError('WebAssembly.Module.exports: not a Module');\n"
"    return s.info.exports.map(e => ({ name: e.name, kind: e.kind }));\n"
"  }\n"
"  static imports(m) {\n"
"    const s = HND.get(m);\n"
"    if (!s) throw new TypeError('WebAssembly.Module.imports: not a Module');\n"
"    return s.info.imports.map(e => ({ module: e.module, name: e.name, "
        "kind: e.kind }));\n"
"  }\n"
"}\n"
/* NOTE: Module.customSections is DELIBERATELY ABSENT.  The decoder skips
 * custom sections without retaining them, so the only value we could return
 * is [] -- and [] is not "we do not know", it is the positive claim that the
 * module carries no section of that name.  A page using it to find a source
 * map or a name section would take the wrong branch with no way to tell.
 * Absent is a question a page can answer; [] is an answer that is wrong. */
"Object.defineProperty(Module.prototype, Symbol.toStringTag,\n"
"  { value: 'WebAssembly.Module', configurable: true });\n"
"\n"
/* ---- Memory ------------------------------------------------------------ */
"class Memory {\n"
"  constructor(desc) {\n"
"    if (!new.target) throw new TypeError(\"WebAssembly.Memory requires 'new'\");\n"
"    if (typeof desc !== 'object' || desc === null)\n"
"      throw new TypeError('WebAssembly.Memory: descriptor must be an object');\n"
"    const initial = u32field(desc, desc.initial !== undefined ? 'initial' : "
        "'minimum');\n"
"    const maximum = (desc.maximum !== undefined) ? u32field(desc, 'maximum') : -1;\n"
"    const r = P.memNew(initial, maximum);\n"
"    if (r.err !== undefined) throw new RangeError(r.err);\n"
"    HND.set(this, { h: r.h, tok: r.tok });\n"
"  }\n"
"  get buffer() {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Memory.buffer: not a Memory');\n"
"    return P.memBuffer(s.h);\n"
"  }\n"
"  grow(delta) {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Memory.grow: not a Memory');\n"
"    const d = u32field({ delta: delta }, 'delta');\n"
     /* The old buffer is detached by the C side before this returns.  That is
      * the specification, and here it is also the safety property: the bytes
      * really did move, so a view kept across this call would be reading the
      * pre-grow block. */
"    const old = P.memGrow(s.h, d);\n"
"    if (old < 0) throw new RangeError('WebAssembly.Memory.grow: refused "
        "(past the declared maximum, or past what this implementation can "
        "allocate)');\n"
"    return old;\n"
"  }\n"
"}\n"
"Object.defineProperty(Memory.prototype, Symbol.toStringTag,\n"
"  { value: 'WebAssembly.Memory', configurable: true });\n"
"\n"
/* ---- Table ------------------------------------------------------------- */
"class Table {\n"
"  constructor(desc) {\n"
"    if (!new.target) throw new TypeError(\"WebAssembly.Table requires 'new'\");\n"
"    if (typeof desc !== 'object' || desc === null)\n"
"      throw new TypeError('WebAssembly.Table: descriptor must be an object');\n"
     /* MVP has exactly one element type.  Anything else is refused by name
      * rather than quietly treated as anyfunc. */
"    if (String(desc.element) !== 'anyfunc' && String(desc.element) !== 'funcref')\n"
"      throw new TypeError(\"WebAssembly.Table: element must be 'anyfunc' \" +\n"
"        '(this is a WebAssembly MVP interpreter; other element types are "
        "post-MVP and are refused rather than approximated)');\n"
"    const initial = u32field(desc, desc.initial !== undefined ? 'initial' : "
        "'minimum');\n"
"    const maximum = (desc.maximum !== undefined) ? u32field(desc, 'maximum') : -1;\n"
"    const r = P.tabNew(initial, maximum);\n"
"    if (r.err !== undefined) throw new RangeError(r.err);\n"
"    HND.set(this, { h: r.h, tok: r.tok, fns: new Array(initial).fill(null) });\n"
"  }\n"
"  get length() {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Table.length: not a Table');\n"
"    return P.tabLen(s.h);\n"
"  }\n"
"  get(i) {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Table.get: not a Table');\n"
"    const idx = u32field({ index: i }, 'index');\n"
"    const f = P.tabGet(s.h, idx);\n"
"    if (f === -2) throw new RangeError('WebAssembly.Table.get: index out of "
        "bounds');\n"
"    if (f === -1) return null;\n"
     /* The JS function object for a slot, when we have one.  A slot the
      * MODULE filled through an element segment has a function index but no
      * JS object; making one requires knowing which instance it belongs to,
      * which the table does only once an instance has adopted it. */
"    if (s.fns[idx]) return s.fns[idx];\n"
"    if (s.inst) return exportFn(s.inst, f);\n"
"    return null;\n"
"  }\n"
"  set(i, v) {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Table.set: not a Table');\n"
"    const idx = u32field({ index: i }, 'index');\n"
"    let fidx = -1;\n"
"    if (v !== null && v !== undefined) {\n"
"      const rec = EXP.get(v);\n"
"      if (!rec) throw new TypeError('WebAssembly.Table.set: value must be an "
          "exported WebAssembly function or null');\n"
       /* REFUSED, and named.  A table slot here holds a bare function INDEX,
        * which only means anything inside one instance; storing another
        * instance's index would silently call whatever function happens to
        * sit at that index in this one.  Cross-instance funcrefs are a real
        * thing this interpreter does not represent, so it says so instead of
        * dispatching to the wrong function. */
"      if (s.inst && rec.inst !== s.inst)\n"
"        throw new TypeError('WebAssembly.Table.set: this implementation "
          "cannot store a function from a DIFFERENT instance in a table "
          "(a table slot holds a function index, which is only meaningful "
          "within one instance)');\n"
"      fidx = rec.fidx;\n"
"    }\n"
"    const rc = P.tabSet(s.h, idx, fidx);\n"
"    if (rc === -2) throw new RangeError('WebAssembly.Table.set: index out of "
        "bounds');\n"
"    s.fns[idx] = (fidx < 0) ? null : v;\n"
"    return undefined;\n"
"  }\n"
"}\n"
"Object.defineProperty(Table.prototype, Symbol.toStringTag,\n"
"  { value: 'WebAssembly.Table', configurable: true });\n"
"\n"
/* ---- Global ------------------------------------------------------------ */
"const VT = ['i32', 'i64', 'f32', 'f64'];\n"
"function zeroOf(t) { return (t === 'i64') ? 0n : 0; }\n"
"class Global {\n"
"  constructor(desc, v) {\n"
"    if (!new.target) throw new TypeError(\"WebAssembly.Global requires 'new'\");\n"
"    if (typeof desc !== 'object' || desc === null)\n"
"      throw new TypeError('WebAssembly.Global: descriptor must be an object');\n"
"    const t = String(desc.value);\n"
"    if (VT.indexOf(t) < 0)\n"
"      throw new TypeError(\"WebAssembly.Global: value must be one of \" +\n"
"        \"'i32', 'i64', 'f32', 'f64' (this is a WebAssembly MVP \" +\n"
"        'interpreter; reference types are post-MVP)');\n"
"    const mut = !!desc.mutable;\n"
"    const st = { t: t, mut: mut, own: true, v: undefined };\n"
"    HND.set(this, st);\n"
     /* An absent initial value is the type's zero, and for i64 that zero is a
      * BIGINT.  Writing 0 there would make the very first read of a fresh i64
      * global return a Number, which is the one thing rule 2 in js_wasm.c's
      * header exists to prevent. */
"    st.v = (v === undefined) ? zeroOf(t) : coerce(t, v);\n"
"  }\n"
"  get value() {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Global.value: not a Global');\n"
"    if (s.own) return s.v;\n"
"    return P.gget(s.ih, s.gidx, s.t);\n"
"  }\n"
"  set value(v) {\n"
"    const s = HND.get(this);\n"
"    if (!s) throw new TypeError('WebAssembly.Global.value: not a Global');\n"
"    if (!s.mut) throw new TypeError('WebAssembly.Global: this global is "
        "immutable');\n"
"    if (s.own) { s.v = coerce(s.t, v); return; }\n"
"    P.gset(s.ih, s.gidx, s.t, v);\n"
"  }\n"
"  valueOf() { return this.value; }\n"
"}\n"
"Object.defineProperty(Global.prototype, Symbol.toStringTag,\n"
"  { value: 'WebAssembly.Global', configurable: true });\n"
"\n"
/* Coerce a JS value to a wasm value type, in JS, with the same rules the C
 * side uses -- i64 through BigInt (so a Number THROWS rather than rounding),
 * i32 through the modulo-2^32 conversion, f32 through a real single-precision
 * round-trip rather than a pretence that it is a double. */
"const F32 = new Float32Array(1);\n"
"function coerce(t, v) {\n"
"  switch (t) {\n"
"    case 'i32': return v | 0;\n"
"    case 'i64': return BigInt.asIntN(64, BigInt(v));\n"
"    case 'f32': F32[0] = Number(v); return F32[0];\n"
"    default:    return Number(v);\n"
"  }\n"
"}\n"
"\n"
/* ---- exported functions ------------------------------------------------ */
"function exportFn(instState, fidx) {\n"
"  if (instState.fns.has(fidx)) return instState.fns.get(fidx);\n"
"  const sig = P.sig(instState.h, fidx);\n"
"  const f = function () {\n"
"    const r = P.call(instState.h, fidx, Array.prototype.slice.call(arguments));\n"
     /* A pending JS exception from an imported function has already been
      * rethrown by the C side, so anything with `.err` here is a wasm trap
      * and only a wasm trap. */
"    if (r.err !== undefined) throw new RuntimeError(r.err);\n"
"    return r.v;\n"
"  };\n"
"  Object.defineProperty(f, 'length', { value: sig.params.length, "
      "configurable: true });\n"
"  Object.defineProperty(f, 'name', { value: 'wasm[' + fidx + ']', "
      "configurable: true });\n"
"  EXP.set(f, { ih: instState.h, fidx: fidx, inst: instState });\n"
"  instState.fns.set(fidx, f);\n"
"  return f;\n"
"}\n"
"\n"
/* ---- the import object -------------------------------------------------
 * Building the array the C side wants, and this is where LinkError is
 * decided.  The distinction the specification draws, and it is worth keeping
 * straight: a non-object importObject when the module HAS imports is a
 * TypeError (the caller passed the wrong kind of thing); a namespace that
 * exists but lacks the field, or holds the wrong sort of value, is a
 * LinkError (the caller passed the right kind of thing and it does not fit). */
"function buildImports(info, importObject) {\n"
"  const out = []; let memObj = null, tabObj = null;\n"
"  if (info.imports.length && (typeof importObject !== 'object' || "
      "importObject === null))\n"
"    throw new TypeError('WebAssembly.Instance: importObject must be an object "
      "when the module declares imports');\n"
"  for (const im of info.imports) {\n"
"    const ns = importObject[im.module];\n"
"    if (typeof ns !== 'object' || ns === null)\n"
"      throw new TypeError(\"WebAssembly.Instance: import namespace '\" + "
        "im.module + \"' is not an object\");\n"
"    const v = ns[im.name];\n"
"    if (v === undefined)\n"
"      throw new LinkError(\"unknown import: '\" + im.module + '.' + im.name + "
        "\"' is not defined\");\n"
"    switch (im.kind) {\n"
"      case 'function':\n"
"        if (typeof v !== 'function')\n"
"          throw new LinkError(\"import '\" + im.module + '.' + im.name + "
            "\"' is not a function\");\n"
"        out.push({ k: 0, f: v });\n"
"        break;\n"
"      case 'global': {\n"
"        let gv;\n"
"        if (v instanceof Global) {\n"
"          const s = HND.get(v);\n"
"          if (s.t !== im.vt)\n"
"            throw new LinkError(\"import '\" + im.module + '.' + im.name + "
              "\"' is a \" + s.t + ' global where the module declares ' + im.vt);\n"
"          gv = v.value;\n"
"        } else if (typeof v === 'number' || typeof v === 'bigint') {\n"
"          gv = v;\n"
"        } else {\n"
"          throw new LinkError(\"import '\" + im.module + '.' + im.name + "
            "\"' is not a WebAssembly.Global or a number\");\n"
"        }\n"
"        out.push({ k: 1, t: im.vt, v: coerce(im.vt, gv) });\n"
"        break;\n"
"      }\n"
"      case 'memory': {\n"
"        if (!(v instanceof Memory))\n"
"          throw new LinkError(\"import '\" + im.module + '.' + im.name + "
            "\"' is not a WebAssembly.Memory\");\n"
"        const s = HND.get(v);\n"
"        if (P.memPages(s.h) < im.min)\n"
"          throw new LinkError(\"import '\" + im.module + '.' + im.name + "
            "\"' is smaller than the module's declared minimum\");\n"
"        memObj = v;\n"
"        out.push({ k: 2, h: s.h });\n"
"        break;\n"
"      }\n"
"      case 'table': {\n"
"        if (!(v instanceof Table))\n"
"          throw new LinkError(\"import '\" + im.module + '.' + im.name + "
            "\"' is not a WebAssembly.Table\");\n"
"        const s = HND.get(v);\n"
"        if (P.tabLen(s.h) < im.min)\n"
"          throw new LinkError(\"import '\" + im.module + '.' + im.name + "
            "\"' is smaller than the module's declared minimum\");\n"
"        tabObj = v;\n"
"        out.push({ k: 3, h: s.h });\n"
"        break;\n"
"      }\n"
"      default:\n"
"        throw new LinkError('unsupported import kind: ' + im.kind);\n"
"    }\n"
"  }\n"
"  return { spec: out, memObj: memObj, tabObj: tabObj };\n"
"}\n"
"\n"
/* ---- Instance ---------------------------------------------------------- */
"function makeMemory(h, tok, inst) {\n"
"  const m = Object.create(Memory.prototype);\n"
"  HND.set(m, { h: h, tok: tok, inst: inst });\n"
"  return m;\n"
"}\n"
"function makeTable(h, tok, inst) {\n"
"  const t = Object.create(Table.prototype);\n"
"  HND.set(t, { h: h, tok: tok, inst: inst, fns: [] });\n"
"  return t;\n"
"}\n"
"\n"
"function instantiateSync(module, importObject) {\n"
"  const ms = HND.get(module);\n"
"  if (!ms) throw new TypeError('WebAssembly.Instance: first argument must be "
      "a WebAssembly.Module');\n"
"  const built = buildImports(ms.info, importObject);\n"
"  const r = P.instantiate(ms.h, built.spec);\n"
"  if (r.err !== undefined) throw (r.link ? new LinkError(r.err) : "
      "new RuntimeError(r.err));\n"
"  const st = { h: r.h, tok: r.tok, mod: module, modtok: ms.tok, "
      "fns: new Map() };\n"
  /* Exactly one object per exported memory/table, and for an imported one it
   * is the object the PAGE passed in -- `instance.exports.mem === theMemory`
   * is a thing pages rely on, and handing back a second wrapper over the same
   * bytes would make that identity silently false. */
"  const memObj = built.memObj || ((r.memh >= 0) ? "
      "makeMemory(r.memh, r.memtok, st) : null);\n"
"  const tabObj = built.tabObj || ((r.tabh >= 0) ? "
      "makeTable(r.tabh, r.tabtok, st) : null);\n"
"  if (built.tabObj) HND.get(built.tabObj).inst = st;\n"
"  const exports = Object.create(null);\n"
"  for (const e of ms.info.exports) {\n"
"    switch (e.kind) {\n"
"      case 'function': exports[e.name] = exportFn(st, e.index); break;\n"
"      case 'memory':\n"
"        if (memObj) exports[e.name] = memObj;\n"
"        break;\n"
"      case 'table':\n"
"        if (tabObj) exports[e.name] = tabObj;\n"
"        break;\n"
"      case 'global': {\n"
"        const g = Object.create(Global.prototype);\n"
"        HND.set(g, { t: e.vt, mut: !!e.mutable, own: false, ih: r.h, "
          "gidx: e.index, inst: st });\n"
"        exports[e.name] = g;\n"
"        break;\n"
"      }\n"
"      default: break;\n"
"    }\n"
"  }\n"
"  Object.freeze(exports);\n"
"  return { state: st, exports: exports };\n"
"}\n"
"\n"
"class Instance {\n"
"  constructor(module, importObject) {\n"
"    if (!new.target) throw new TypeError(\"WebAssembly.Instance requires "
      "'new'\");\n"
"    const built = instantiateSync(module, importObject);\n"
"    HND.set(this, built.state);\n"
"    Object.defineProperty(this, 'exports',\n"
"      { value: built.exports, enumerable: true, writable: false, "
        "configurable: false });\n"
"  }\n"
"}\n"
"Object.defineProperty(Instance.prototype, Symbol.toStringTag,\n"
"  { value: 'WebAssembly.Instance', configurable: true });\n"
"\n"
/* ---- the namespace ----------------------------------------------------- */
"function validate(bytes) {\n"
"  let ab;\n"
"  try { ab = toBytes(bytes, 'WebAssembly.validate'); }\n"
"  catch (e) { throw e; }\n"
"  return P.validate(ab);\n"
"}\n"
"\n"
/* compile/instantiate are asynchronous in the API and synchronous underneath.
 * Building the Module inside the executor is what makes that honest: a
 * CompileError becomes a REJECTED promise rather than a synchronous throw,
 * which is the difference a `.catch()` with no surrounding try/catch sees. */
"function compile(bytes) {\n"
"  return new Promise(function (resolve) { resolve(new Module(bytes)); });\n"
"}\n"
"function instantiate(src, importObject) {\n"
"  return new Promise(function (resolve) {\n"
"    if (src instanceof Module) { resolve(new Instance(src, importObject)); "
      "return; }\n"
"    const m = new Module(src);\n"
"    resolve({ module: m, instance: new Instance(m, importObject) });\n"
"  });\n"
"}\n"
"\n"
"const WA = {\n"
"  Module: Module, Instance: Instance, Memory: Memory, Table: Table,\n"
"  Global: Global,\n"
"  CompileError: CompileError, LinkError: LinkError, RuntimeError: RuntimeError,\n"
"  validate: validate, compile: compile, instantiate: instantiate\n"
"};\n"
"\n"
/* THE STREAMING ENTRY POINTS DO NOT STREAM, AND THE HONEST THING IS TO SAY SO
 * RATHER THAN TO PRETEND.  `Response.body` is undefined in this browser --
 * there is no ReadableStream on a fetch response here -- so there is nothing
 * to consume incrementally.  Falling back to `.arrayBuffer()` is EXPLICITLY
 * CONFORMANT: the specification permits an implementation to await the whole
 * body, and the only observable difference is that compilation cannot overlap
 * the download.  It is not a stub returning something wrong; it is the same
 * Module, built a moment later.
 *
 * Defined only when Response exists.  A compileStreaming that cannot read a
 * Response is worse than an absent one, because a page feature-tests it. */
"if (typeof G.Response !== 'undefined') {\n"
"  WA.compileStreaming = function (src) {\n"
"    return Promise.resolve(src).then(function (resp) {\n"
"      if (!resp || typeof resp.arrayBuffer !== 'function')\n"
"        throw new TypeError('WebAssembly.compileStreaming: expected a "
          "Response');\n"
"      return resp.arrayBuffer();\n"
"    }).then(function (buf) { return new Module(buf); });\n"
"  };\n"
"  WA.instantiateStreaming = function (src, importObject) {\n"
"    return WA.compileStreaming(src).then(function (m) {\n"
"      return { module: m, instance: new Instance(m, importObject) };\n"
"    });\n"
"  };\n"
"}\n"
"\n"
"Object.defineProperty(WA, Symbol.toStringTag,\n"
"  { value: 'WebAssembly', configurable: true });\n"
"Object.defineProperty(G, 'WebAssembly',\n"
"  { value: WA, writable: true, enumerable: false, configurable: true });\n"
"})\n"

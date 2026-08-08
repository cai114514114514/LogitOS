/* Intl -- number, date, list and plural formatting.
 *
 * MEASURED: deepseek, once MessageChannel let React's scheduler start and the
 * app actually began rendering. It is also on the owner's list. QuickJS is
 * built here without any Intl at all, so `Intl.NumberFormat` is a
 * ReferenceError, and a React app that formats one number dies at that line
 * with its tree half-committed.
 *
 * WHAT THIS HONESTLY IS, AND IT IS THE FIRST THING TO READ
 * There is no locale data in this system. No CLDR, no ICU, no timezone
 * database -- the kernel's only clock is the RTC and it reports one wall time.
 * So this is an ENGLISH formatter wearing the Intl interface: grouping is
 * thousands-with-commas, the decimal separator is a full stop, month and day
 * names are English, and every date is formatted in the machine's own time
 * zone because there is no other one to convert to.
 *
 * That is a real limitation and it is deliberate rather than accidental. The
 * alternative to a monolingual Intl is not a multilingual one, it is no Intl,
 * and no Intl means a page that formats a price renders nothing at all. What
 * this must never do is claim otherwise: resolvedOptions() reports the locale
 * as 'en-US' and the time zone as 'UTC' whatever was requested, so a page that
 * checks gets told the truth instead of being lied to in its own locale.
 *
 * Not provided: Intl.Segmenter, Intl.DisplayNames, Intl.DurationFormat, and
 * Intl.Collator's actual collation (it compares code points, which is right
 * for ASCII and wrong for everything with an accent in it). None of them were
 * reached by the corpus; all of them would need the data this system does not
 * have.
 */
#include "quickjs.h"
#include "js_platform.h"
#include <string.h>

int printf(const char *, ...);

static const char *INTL_PRELUDE =
"(function () {\n"
"'use strict';\n"
"var G = globalThis;\n"
"if (G.Intl) return;\n"

"var MONTH = ['January','February','March','April','May','June','July',\n"
"             'August','September','October','November','December'];\n"
"var DAY = ['Sunday','Monday','Tuesday','Wednesday','Thursday','Friday','Saturday'];\n"

/* Grouping is done on the STRING, not by repeated division: a float big enough
 * to need grouping is also big enough that dividing it by 1000 in a loop
 * accumulates error, and the digits are already exact once toFixed has run. */
"function group(intStr, sep) {\n"
"  var out = '', n = 0;\n"
"  for (var i = intStr.length - 1; i >= 0; i--) {\n"
"    out = intStr[i] + out;\n"
"    if (++n % 3 === 0 && i > 0) out = sep + out;\n"
"  }\n"
"  return out;\n"
"}\n"

"function fmtNumber(v, o) {\n"
"  if (typeof v === 'bigint') v = Number(v);\n"
"  v = Number(v);\n"
"  if (!isFinite(v)) return v !== v ? 'NaN' : (v > 0 ? '\\u221e' : '-\\u221e');\n"
"  var style = o.style || 'decimal';\n"
"  if (style === 'percent') v = v * 100;\n"
"  var minF = o.minimumFractionDigits;\n"
"  var maxF = o.maximumFractionDigits;\n"
"  if (minF === undefined) minF = (style === 'currency') ? 2 : 0;\n"
"  if (maxF === undefined) maxF = (style === 'currency') ? 2 : (style === 'percent' ? 0 : 3);\n"
"  if (maxF < minF) maxF = minF;\n"
"  var neg = v < 0 || (v === 0 && 1 / v < 0);\n"
"  var a = Math.abs(v);\n"
"  var s = a.toFixed(maxF);\n"
   /* toFixed pads to maxF; trim back to minF without ever trimming a digit
      that is not a zero, which is what separates 1.50 -> 1.5 from 1.25 -> 1.2. */
"  if (maxF > minF && s.indexOf('.') >= 0) {\n"
"    while (s.length && s[s.length - 1] === '0' &&\n"
"           s.length - 1 - s.indexOf('.') > minF) s = s.slice(0, -1);\n"
"    if (s[s.length - 1] === '.') s = s.slice(0, -1);\n"
"  }\n"
"  var dot = s.indexOf('.');\n"
"  var ip = dot < 0 ? s : s.slice(0, dot);\n"
"  var fp = dot < 0 ? '' : s.slice(dot);\n"
"  if (o.minimumIntegerDigits) while (ip.length < o.minimumIntegerDigits) ip = '0' + ip;\n"
"  if (o.useGrouping !== false && o.useGrouping !== 'false') ip = group(ip, ',');\n"
"  var out = ip + fp;\n"
"  if (style === 'percent') out += '%';\n"
"  if (style === 'currency') {\n"
"    var cur = o.currency ? String(o.currency).toUpperCase() : 'USD';\n"
"    var sym = { USD: '$', EUR: '\\u20ac', GBP: '\\u00a3', JPY: '\\u00a5',\n"
"                CNY: '\\u00a5', KRW: '\\u20a9', INR: '\\u20b9' }[cur];\n"
"    if (o.currencyDisplay === 'code' || !sym) out = cur + '\\u00a0' + out;\n"
"    else out = sym + out;\n"
"  }\n"
"  return neg ? '-' + out : out;\n"
"}\n"

"function two(n) { return (n < 10 ? '0' : '') + n; }\n"

"function fmtDate(d, o) {\n"
"  if (d === undefined) d = new Date();\n"
"  if (typeof d === 'number') d = new Date(d);\n"
"  if (!(d instanceof Date)) d = new Date(Number(d));\n"
"  if (isNaN(d.getTime())) return 'Invalid Date';\n"
"  var keys = ['weekday','year','month','day','hour','minute','second',\n"
"              'dateStyle','timeStyle'];\n"
"  var any = false;\n"
"  for (var i = 0; i < keys.length; i++) if (o[keys[i]] !== undefined) any = true;\n"
"  var parts = [];\n"
"  if (!any) return two(d.getMonth() + 1) + '/' + two(d.getDate()) + '/' + d.getFullYear();\n"
"  if (o.dateStyle === 'full')\n"
"    return DAY[d.getDay()] + ', ' + MONTH[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();\n"
"  if (o.dateStyle === 'long')\n"
"    return MONTH[d.getMonth()] + ' ' + d.getDate() + ', ' + d.getFullYear();\n"
"  if (o.dateStyle === 'medium')\n"
"    return MONTH[d.getMonth()].slice(0, 3) + ' ' + d.getDate() + ', ' + d.getFullYear();\n"
"  if (o.dateStyle === 'short')\n"
"    return two(d.getMonth() + 1) + '/' + two(d.getDate()) + '/' + String(d.getFullYear()).slice(2);\n"
"  if (o.weekday) parts.push(o.weekday === 'short' ? DAY[d.getDay()].slice(0, 3) : DAY[d.getDay()]);\n"
"  var md = [];\n"
"  if (o.month === 'long') md.push(MONTH[d.getMonth()]);\n"
"  else if (o.month === 'short') md.push(MONTH[d.getMonth()].slice(0, 3));\n"
"  else if (o.month === '2-digit') md.push(two(d.getMonth() + 1));\n"
"  else if (o.month) md.push(String(d.getMonth() + 1));\n"
"  if (o.day) md.push(o.day === '2-digit' ? two(d.getDate()) : String(d.getDate()));\n"
"  if (md.length) parts.push(md.join(' '));\n"
"  if (o.year) parts.push(o.year === '2-digit' ? String(d.getFullYear()).slice(2)\n"
"                                              : String(d.getFullYear()));\n"
"  var out = parts.join(o.month === 'numeric' || o.month === '2-digit' ? '/' : ' ');\n"
"  if (o.hour || o.minute || o.second || o.timeStyle) {\n"
"    var h = d.getHours(), ap = '';\n"
"    if (o.hour12 !== false) { ap = h >= 12 ? ' PM' : ' AM'; h = h % 12; if (h === 0) h = 12; }\n"
"    var t = (o.hour === '2-digit' ? two(h) : String(h)) + ':' + two(d.getMinutes());\n"
"    if (o.second || o.timeStyle === 'medium' || o.timeStyle === 'long') t += ':' + two(d.getSeconds());\n"
"    out = out ? out + ', ' + t + ap : t + ap;\n"
"  }\n"
"  return out;\n"
"}\n"

"var Intl = {};\n"
/* The locale argument is ACCEPTED and IGNORED, and resolvedOptions says so.
   A page that asks for 'ja-JP' gets English, and gets told it got en-US --
   which is the difference between a limitation and a lie. */
"Intl.getCanonicalLocales = function (l) {\n"
"  if (l === undefined) return [];\n"
"  return (Array.isArray(l) ? l : [l]).map(String);\n"
"};\n"
"Intl.NumberFormat = function NumberFormat(loc, opts) {\n"
"  if (!(this instanceof Intl.NumberFormat)) return new Intl.NumberFormat(loc, opts);\n"
"  var o = opts || {};\n"
"  var self = this;\n"
"  this.format = function (v) { return fmtNumber(v, o); };\n"
"  this.formatToParts = function (v) {\n"
"    return [{ type: 'literal', value: self.format(v) }];\n"
"  };\n"
"  this.resolvedOptions = function () {\n"
"    return { locale: 'en-US', numberingSystem: 'latn', style: o.style || 'decimal',\n"
"             currency: o.currency, useGrouping: o.useGrouping !== false,\n"
"             minimumIntegerDigits: o.minimumIntegerDigits || 1,\n"
"             minimumFractionDigits: o.minimumFractionDigits === undefined ? 0 : o.minimumFractionDigits,\n"
"             maximumFractionDigits: o.maximumFractionDigits === undefined ? 3 : o.maximumFractionDigits };\n"
"  };\n"
"};\n"
"Intl.NumberFormat.supportedLocalesOf = function () { return []; };\n"
"Intl.DateTimeFormat = function DateTimeFormat(loc, opts) {\n"
"  if (!(this instanceof Intl.DateTimeFormat)) return new Intl.DateTimeFormat(loc, opts);\n"
"  var o = opts || {};\n"
"  var self = this;\n"
"  this.format = function (d) { return fmtDate(d, o); };\n"
"  this.formatToParts = function (d) { return [{ type: 'literal', value: self.format(d) }]; };\n"
"  this.resolvedOptions = function () {\n"
"    return { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn',\n"
"             timeZone: 'UTC' };\n"
"  };\n"
"};\n"
"Intl.DateTimeFormat.supportedLocalesOf = function () { return []; };\n"
/* Code-point order, and the name says Collator. Right for ASCII, wrong for
   anything with an accent; there is no collation data to be right with. */
"Intl.Collator = function Collator(loc, opts) {\n"
"  if (!(this instanceof Intl.Collator)) return new Intl.Collator(loc, opts);\n"
"  var o = opts || {};\n"
"  this.compare = function (a, b) {\n"
"    a = String(a); b = String(b);\n"
"    if (o.sensitivity === 'base' || o.sensitivity === 'accent') {\n"
"      a = a.toLowerCase(); b = b.toLowerCase();\n"
"    }\n"
"    if (o.numeric) {\n"
"      var na = parseFloat(a), nb = parseFloat(b);\n"
"      if (!isNaN(na) && !isNaN(nb) && na !== nb) return na < nb ? -1 : 1;\n"
"    }\n"
"    return a < b ? -1 : (a > b ? 1 : 0);\n"
"  };\n"
"  this.resolvedOptions = function () {\n"
"    return { locale: 'en-US', usage: 'sort', sensitivity: o.sensitivity || 'variant',\n"
"             numeric: !!o.numeric };\n"
"  };\n"
"};\n"
"Intl.Collator.supportedLocalesOf = function () { return []; };\n"
/* English cardinal plurals: one iff n === 1. Ordinals need the 1st/2nd/3rd/th
   rule, which is four cases and worth having because every "N results" and
   every leaderboard uses it. */
"Intl.PluralRules = function PluralRules(loc, opts) {\n"
"  if (!(this instanceof Intl.PluralRules)) return new Intl.PluralRules(loc, opts);\n"
"  var o = opts || {};\n"
"  this.select = function (n) {\n"
"    n = Number(n);\n"
"    if (o.type === 'ordinal') {\n"
"      var r10 = n % 10, r100 = n % 100;\n"
"      if (r10 === 1 && r100 !== 11) return 'one';\n"
"      if (r10 === 2 && r100 !== 12) return 'two';\n"
"      if (r10 === 3 && r100 !== 13) return 'few';\n"
"      return 'other';\n"
"    }\n"
"    return n === 1 ? 'one' : 'other';\n"
"  };\n"
"  this.resolvedOptions = function () {\n"
"    return { locale: 'en-US', type: o.type || 'cardinal',\n"
"             pluralCategories: o.type === 'ordinal' ? ['one','two','few','other']\n"
"                                                    : ['one','other'] };\n"
"  };\n"
"};\n"
"Intl.PluralRules.supportedLocalesOf = function () { return []; };\n"
"Intl.ListFormat = function ListFormat(loc, opts) {\n"
"  if (!(this instanceof Intl.ListFormat)) return new Intl.ListFormat(loc, opts);\n"
"  var o = opts || {};\n"
"  var word = o.type === 'disjunction' ? 'or' : 'and';\n"
"  this.format = function (list) {\n"
"    var a = Array.from(list || []).map(String);\n"
"    if (a.length === 0) return '';\n"
"    if (a.length === 1) return a[0];\n"
"    if (a.length === 2) return a[0] + ' ' + word + ' ' + a[1];\n"
"    return a.slice(0, -1).join(', ') + ', ' + word + ' ' + a[a.length - 1];\n"
"  };\n"
"  this.resolvedOptions = function () { return { locale: 'en-US', type: o.type || 'conjunction' }; };\n"
"};\n"
"Intl.RelativeTimeFormat = function RelativeTimeFormat(loc, opts) {\n"
"  if (!(this instanceof Intl.RelativeTimeFormat)) return new Intl.RelativeTimeFormat(loc, opts);\n"
"  var o = opts || {};\n"
"  this.format = function (v, unit) {\n"
"    v = Number(v);\n"
"    var u = String(unit).replace(/s$/, '');\n"
"    var n = Math.abs(v);\n"
"    var plural = n === 1 ? u : u + 's';\n"
"    if (o.numeric === 'auto' && v === 0) return 'now';\n"
"    return v < 0 ? n + ' ' + plural + ' ago' : 'in ' + n + ' ' + plural;\n"
"  };\n"
"  this.resolvedOptions = function () { return { locale: 'en-US', numeric: o.numeric || 'always' }; };\n"
"};\n"
"G.Intl = Intl;\n"

/* toLocaleString on the built-ins routes through the same formatters. Without
 * this a page that never touches Intl directly -- `(1234.5).toLocaleString()`
 * is the commonest formatting call on the web -- still gets QuickJS's
 * ungrouped toString. */
"try {\n"
"  Number.prototype.toLocaleString = function (loc, opts) { return fmtNumber(this, opts || {}); };\n"
"  Date.prototype.toLocaleString = function (loc, opts) {\n"
"    return fmtDate(this, opts || { year: 'numeric', month: 'numeric', day: 'numeric',\n"
"                                   hour: 'numeric', minute: '2-digit', second: '2-digit' });\n"
"  };\n"
"  Date.prototype.toLocaleDateString = function (loc, opts) {\n"
"    return fmtDate(this, opts || {});\n"
"  };\n"
"  Date.prototype.toLocaleTimeString = function (loc, opts) {\n"
"    return fmtDate(this, opts || { hour: 'numeric', minute: '2-digit', second: '2-digit' });\n"
"  };\n"
"} catch (e) {}\n"

/* SuppressedError. MEASURED on deepseek, and reached only once the event loop
 * ran: it is the ES2024 error `using`/`await using` throws when a disposal
 * fails while another error is already in flight. Bundlers emit a feature test
 * for it at module top level. QuickJS 2024-01 does not have it. */
"if (!G.SuppressedError) {\n"
"  var SE = function SuppressedError(error, suppressed, message) {\n"
"    var e = Error.call(this, message);\n"
"    this.error = error; this.suppressed = suppressed;\n"
"    this.message = message === undefined ? '' : String(message);\n"
"    this.name = 'SuppressedError';\n"
"    if (e.stack) this.stack = e.stack;\n"
"  };\n"
"  SE.prototype = Object.create(Error.prototype);\n"
"  SE.prototype.constructor = SE;\n"
"  SE.prototype.name = 'SuppressedError';\n"
"  G.SuppressedError = SE;\n"
"}\n"
"})\n";

void js_intl_install(JSContext *ctx)
{
    if (!ctx) return;
    JSValue fn = JS_Eval(ctx, INTL_PRELUDE, strlen(INTL_PRELUDE), "<intl>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(fn)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[intl] prelude failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
        JS_FreeValue(ctx, fn);
        return;
    }
    JSValue r = JS_Call(ctx, fn, JS_UNDEFINED, 0, 0);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        printf("[intl] install failed: %s\n", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
    JS_FreeValue(ctx, fn);
}

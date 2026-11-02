# HTML parsing

The browser parses HTML in two stages: a tokenizer turns source bytes into tokens, and a tree builder creates DOM nodes using insertion modes and recovery rules. Page navigation, fragment insertion, and `DOMParser` use this implementation. The core sources are `c/apps/gui/browser/html/tokenizer.c` and `c/apps/gui/browser/html/tree.c`; their callers connect the resulting tree to CSS, JavaScript, and rendering.

## Entry points and ownership

The interfaces in `c/apps/gui/browser/html/tree.h` cover whole documents and fragments:

| Entry point | Result |
|---|---|
| `html_parse()` | An `N_DOCUMENT` root, with its owning `struct dom_doc` returned through the output argument. |
| `html_parse_fragment()` | A synthetic HTML element whose children form the fragment. The caller supplies the context element’s name and namespace. |
| `html_parse_ex()` and `html_parse_fragment_ex()` | The corresponding operations with a flag controlling declarative shadow root attachment. |

Both default wrappers pass zero for the declarative shadow flag. To release the owning document, call `dom_free(dom_doc_root(doc))`. This matters for fragments because the returned synthetic element is a child of the document root.

The migration comment in `c/apps/gui/browser/html/tree.h` still describes replacement of the legacy scanner as pending. The current `dom_parse()` implementation in `c/apps/gui/browser/dom/tree.c` already calls `html_parse()`, and page loading uses that wrapper from `c/apps/gui/browser/app/main.c`.

Fragment parsing derives the tokenizer state and insertion mode from its context. A row context selects the rules that accept table cells; a textarea context selects RCDATA. The parser creates a context node for these decisions and inserts the result beneath the synthetic HTML root.

In `c/apps/gui/browser/js/dom/dom.c`, the `innerHTML` setter parses into a temporary document, imports the resulting children into the destination document, and frees the temporary document. Importing gives the inserted nodes storage owned by their destination.

## Tokenizer and states

`struct html_tokenizer` holds the state machine, borrowed input, and working buffers. `html_tok_next()` returns doctype, start tag, end tag, comment, character, and EOF tokens through `struct html_token`. Tokens include payload lengths, attributes, tag identities, and source byte offsets. The structures and public interfaces are defined in `c/apps/gui/browser/html/tokenizer.h`.

The tree builder selects content models through `html_tok_set_state()`:

| State | Behavior |
|---|---|
| `HTML_STATE_DATA` | Recognizes markup and character references in general HTML content. |
| `HTML_STATE_RCDATA` | Expands character references and recognizes an appropriate closing tag, as used for title and textarea contents. |
| `HTML_STATE_RAWTEXT` | Preserves text and ampersands while looking for an appropriate closing tag, as used for style contents. |
| `HTML_STATE_SCRIPT_DATA` | Processes script text, including escaped and double escaped states. |
| `HTML_STATE_PLAINTEXT` | Treats the remaining input as text. |
| `HTML_STATE_CDATA_SECTION` | Collects character data through the closing CDATA delimiter. |

Internal states in `c/apps/gui/browser/html/tokenizer.c` handle tag names, attribute names and values, comments, doctypes, and closing tag candidates. The tree builder supplies the foreign namespace flag that enables CDATA recognition within SVG and MathML content.

The input reader folds CRLF and lone CR into LF. ASCII letters in tag and attribute names become lowercase, and duplicate attributes retain their first occurrence. Attributes under construction use offsets into a growable buffer so buffer relocation preserves their names and values.

Closing tag candidates in RCDATA, raw text, and script data are compared with the last emitted start tag. A candidate for another element becomes character data. Script escape states add another distinction: an appropriate closing tag can close an escaped script, while the corresponding sequence in double escaped text is emitted as text and returns the tokenizer to the escaped state.

Fragment initialization sets the last emitted start tag to empty. For a script context, a closing script sequence in the fragment counts as text, because the context selects the content model but no start tag was emitted for it.

For runs requiring no transformation, the tokenizer returns slices of the input buffer. Character references, newline normalization, and other transformations use working buffers. Every token payload must be consumed or copied before the next `html_tok_next()` call. The tree builder copies text into DOM storage and uses `dom_text_append()` to merge it with an adjacent text node at the insertion location.

## Generated tables and character references

`tools/gen_html_tables.py` produces the committed lookup tables:

| Generated file | Contents |
|---|---|
| `c/apps/gui/browser/html/entities.inc` | 2,231 named character references, including 106 spellings without a trailing semicolon. |
| `c/apps/gui/browser/html/tags.inc` | 147 recognized tag names, numeric identities, and a sorted lookup table. |

The generator reads named references from Python’s `html.entities.html5` data. Each entry contains one or two Unicode code points, which the tokenizer encodes as UTF-8 during expansion.

Named reference matching uses the longest matching prefix. The tokenizer narrows a range in the sorted table as it reads each byte and remembers the latest complete match. This accommodates both longer names and their legacy prefixes. In an attribute value, a match without a semicolon followed by an ASCII letter, digit, or equals sign preserves the literal input.

Numeric references accept decimal and hexadecimal forms. Recovery maps zero, surrogate values, and values above the Unicode range to U+FFFD. Values in the C1 control range pass through the compatibility mapping in `c/apps/gui/browser/html/tokenizer.c`.

`html_tag_id()` searches the generated tag table. An unrecognized name receives `HTAG_UNKNOWN` and retains its name bytes in the token. The builder uses identities to index classification flags for special elements, formatting elements, scope boundaries, and implied end tags. Those flags, together with SVG and MathML name adjustments, are maintained in `c/apps/gui/browser/html/tree.c`.

The sorted entity representation supports prefix matching directly, and the lookup data is built at generation time. Tag identities turn the builder’s repeated category checks into indexed lookups and switches.

## Tree builder and insertion modes

`struct html_tb` holds the tokenizer, document, stack of open elements, active formatting list, template insertion mode stack, and current insertion mode. It also tracks the head and form elements, fragment context, and pending table text.

The open element stack records parsing ancestry. The active formatting list preserves formatting elements that recovery may reconstruct after they leave that stack. Markers bound reconstruction within structures such as templates and table cells.

`tb_process()` chooses HTML or foreign content processing from the adjusted current node and token type. `tb_mode()` dispatches the HTML insertion modes:

| Mode group | Purpose |
|---|---|
| Initial, before HTML, before head | Process the doctype and establish document structure. |
| In head, in head noscript, after head | Handle metadata and the transition to body content. |
| In body | Insert body content and apply rules for individual elements. |
| Text | Collect content after a tokenizer content model change. |
| In table, in table text, in caption, in column group, in table body, in row, in cell | Maintain table structure and place misplaced content. |
| In template | Select rules for template contents through the template mode stack. |
| After body, after after body | Process content following body closure. |
| In frameset, after frameset, after after frameset | Process frameset documents and trailing content. |

Handlers can change mode and request processing of the same token again. For example, a cell token encountered inside a table can cause the builder to synthesize a table body and row before inserting the cell. `reset_insertion_mode()` walks the open element stack to recover the appropriate mode after structural changes.

Select contents use the body rules, with handling for select, option, optgroup, and related elements in `in_body()`.

For SVG and MathML, the builder assigns namespaces and adjusts element and attribute names. Integration points allow HTML processing within foreign content. Source offsets also let the builder retain the outer SVG source span for rendering.

## Error recovery

Recovery rules construct a usable tree from malformed markup. The main mechanisms in `c/apps/gui/browser/html/tree.c` are:

| Situation | Recovery |
|---|---|
| Missing HTML, head, or body element | Synthesize the element and process the pending token again. |
| An omitted closing tag | Generate implied end tags according to the current element and scope. |
| Misnested formatting | Reconstruct active formatting elements and apply the adoption agency algorithm. |
| Missing table containers | Synthesize containers such as a table body or row. |
| Misplaced content in a table | Insert it outside the table, usually just before the table element. |
| Malformed declarations or comments | Follow comment recovery states, including comment closure at `--!>`. |
| EOF in text or template content | Unwind the relevant parser state and continue EOF processing. |

Scope checks bound searches through the open element stack. Table scope, for example, stops at HTML, table, and template elements in the HTML namespace. This keeps closing operations tied to the structure relevant to their insertion mode.

The adoption agency algorithm repairs formatting ancestry through cloning and reparenting. The active formatting list retains at most three equivalent entries after its last marker, bounding the entries available for later reconstruction.

Table text is buffered until the builder sees the end of the character run. Whitespace is inserted at the current location. A run containing other characters goes through body character handling, which places the text outside the table. Buffering makes that decision consistent across token boundaries.

Doctype handling selects no quirks, limited quirks, or quirks mode from the token’s name, public identifier, system identifier, and forced quirks flag.

## Script and style handling

`parse_text_element()` inserts an element, changes the tokenizer content model, saves the insertion mode, and enters text mode. Title uses RCDATA, style uses raw text, and script uses script data. A closing tag pops the element and restores the saved insertion mode. Textarea handling selects RCDATA and removes one initial LF.

The builder initializes its scripting flag to enabled, so HTML noscript contents follow the raw text path.

The application consumes script and style text through the constructed DOM. In `c/apps/gui/browser/app/main.c`, `collect_style()` gathers style element text after parsing. The loading path performs an initial style, layout, and paint pass, then fetches external stylesheets. Later it opens the page runtime, calls `collect_scripts()`, fetches script resources, and invokes `run_collected_scripts()`.

For the live document’s `innerHTML` setter, `c/apps/gui/browser/js/dom/dom.c` marks scripts on the imported copies as already started. The script insertion machinery consults this marker when processing later subtree insertions.

## Streaming input

The tokenizer supports a growing input buffer. `html_tok_next()` distinguishes these outcomes:

| Return value | Meaning |
|---|---|
| `1` | A token is available. |
| `0` | More input is needed. |
| `-1` | The EOF token has already been delivered. |

When input ends with `eof` set to zero, the tokenizer restores the position and state saved at the start of the call, discards partial token data, and returns zero. The caller supplies the expanded buffer through `html_tok_feed()` and retries. That buffer must preserve the previous prefix because saved positions refer to offsets within it. The final feed sets `eof` to one.

This replay design lets the state machine handle partial tags, attributes, comments, and doctypes through the same paths used for complete input. Its cost is rescanning an unfinished token or character run after a refill.

Navigation supplies a completed response buffer. `c/apps/gui/browser/app/downloads.inc` waits for response completion and takes the body; `c/apps/gui/browser/app/main.c` passes those bytes to `dom_parse()`. Tree builder initialization marks the input as final.

`tests/unit/html_tok_test.c` compares complete input with input exposed one byte per refill. `tests/unit/html_rawtext_perf_test.c` checks borrowed text slices, transformations, and replay at an input boundary.

## DOMParser

`DOMParser.parseFromString()` is implemented in `c/apps/gui/browser/js/dom/domparser.c`. For the MIME value "text/html", matched without ASCII case distinctions, it converts the JavaScript input string to bytes and calls `dom_parse()`. The result follows the same HTML construction and recovery paths used by navigation.

Each result has a `struct dp_arena` containing its document, root, and reference count. Document and node wrappers retain that arena. Their finalizers release references; the last release frees the document.

The binding provides document accessors, traversal, queries, and mutations through its own wrapper class, and parsing constructs and returns the document. Frame adoption can later associate the document with a script context and offer its scripts for execution through `c/apps/gui/browser/js/dom/frame.c`.

## Limitations

- The parser reports no parse error codes or line and column locations. Tokens still carry byte offsets. The corpus harnesses compare token streams and trees.
- `insert_element()` stops linking new elements when the open element stack reaches `DOM_MAX_TREE_DEPTH`, currently 512. It retains them on the parsing stack as orphans to protect recursive DOM consumers.
- The tree builder stores template children directly on the template element. With declarative shadow attachment enabled, conversion occurs at the closing template tag; EOF retains the template.
- Script evaluation occurs after document parsing. Parser suspension and input insertion around script execution are unimplemented. The `document.write()` compatibility helper in `c/apps/gui/browser/js/runtime/platform.c`, when installed, inserts parsed fragments into the built tree.
- The tokenizer processes bytes and provides no general character encoding decoder. Character references synthesize UTF-8.
- Allocation failures can truncate tokenizer buffers or omit nodes, yielding a partial document.
- DOMParser defaults a missing MIME argument to HTML. Other MIME values produce a diagnostic and an HTML document containing a `parsererror` element; this binding provides no XML parsing.

## See also

- [The document tree](dom.md)
- [Loading pages: fetch, URLs and the HTTP cache](loading-and-fetch.md)
- [CSS: parsing, cascade and computed style](css.md)
- [The JavaScript runtime](javascript-runtime.md)
- [DOM and event bindings](dom-bindings.md)
- [Testing the browser](testing.md)

## Source map

| File | What it contains |
|---|---|
| `c/apps/gui/browser/html/tokenizer.h` | Token structures, public states, payload lifetime, and input feed contract. |
| `c/apps/gui/browser/html/tokenizer.c` | Tokenization, character references, input replay, and borrowed text slices. |
| `c/apps/gui/browser/html/tree.h` | Document, fragment, and declarative shadow interfaces. |
| `c/apps/gui/browser/html/tree.c` | Insertion modes, scope checks, formatting recovery, tables, and foreign content. |
| `c/apps/gui/browser/html/entities.inc` | Generated named character references. |
| `c/apps/gui/browser/html/tags.inc` | Generated tag identities and lookup data. |
| `tools/gen_html_tables.py` | Entity and tag table generator. |
| `c/apps/gui/browser/dom/tree.h` | DOM interfaces and parser depth constant. |
| `c/apps/gui/browser/dom/tree.c` | Document storage, text mutation, document release, and `dom_parse()`. |
| `c/apps/gui/browser/app/downloads.inc` | Response completion and document body acquisition. |
| `c/apps/gui/browser/app/main.c` | Page parsing, stylesheet collection, and script scheduling. |
| `c/apps/gui/browser/js/dom/dom.c` | Live document fragment insertion and `innerHTML`. |
| `c/apps/gui/browser/js/dom/domparser.c` | DOMParser binding, document ownership, and MIME handling. |
| `c/apps/gui/browser/js/dom/frame.c` | Frame adoption and script execution for adopted documents. |
| `c/apps/gui/browser/js/runtime/platform.c` | `document.write()` compatibility helper. |
| `tests/unit/html_tok_test.c` | Token corpus comparisons and input refill comparisons. |
| `tests/unit/html5lib_test.c` | Document and fragment tree comparisons against a failure baseline. |
| `tests/unit/html_rawtext_perf_test.c` | Borrowed text slice, transformation, and refill checks. |

/* This fixture waits for real guest input. The QMP driver locates each 8x8
 * color anchor, clicks 60 pixels to its right, and performs:
 * button -> input "guest" + Enter -> button. No script activates a control.
 *
 * A broken validator can emit submit instead of invalid on the first click.
 * Both paths advance the protocol and record the result, so the negative
 * control fails an assertion rather than hanging before the keyboard phase. */
async function testHTMLUI(check) {
    var panel = document.createElement('div');
    panel.style.cssText = 'width:320px;padding:10px;margin:4px 0 10px;' +
        'border:1px solid #8798a6;background:#ffffff;color:#14202a;font-size:14px;';
    var instructions = document.createElement('div');
    instructions.textContent = 'Guest input: Submit, type guest + Enter, then Submit.';
    instructions.style.cssText = 'margin:0 0 8px;line-height:20px;';
    panel.appendChild(instructions);

    var form = document.createElement('form');
    form.style.cssText = 'margin:0;padding:0;';
    panel.appendChild(form);
    var input = document.createElement('input');
    input.setAttribute('type', 'text');
    input.setAttribute('required', '');
    input.setAttribute('name', 'guest-input');
    input.setAttribute('aria-label', 'Guest validation input');
    input.style.cssText = 'box-sizing:border-box;width:180px;height:30px;' +
        'margin:0;font-size:14px;flex:0 0 180px;';
    var button = document.createElement('button');
    button.setAttribute('type', 'submit');
    button.textContent = 'Submit';
    button.style.cssText = 'box-sizing:border-box;width:90px;height:30px;' +
        'margin:0;font-size:14px;flex:0 0 90px;';

    function row(control, color) {
        var line = document.createElement('div');
        line.style.cssText = 'display:flex;align-items:center;gap:12px;height:30px;margin:0 0 6px;';
        var anchor = document.createElement('div');
        anchor.style.cssText = 'width:8px;height:8px;flex:0 0 8px;background:' + color + ';';
        // The control starts 20px from the anchor's left edge: 8px + 12px gap.
        line.appendChild(anchor);
        line.appendChild(control);
        form.appendChild(line);
    }
    row(input, '#0c75b7');
    row(button, '#ae53c5');

    var phase = 0, initialInvalid = 0, initialSubmit = 0;
    var enterSubmit = 0, buttonSubmit = 0;
    var resolveFirst, resolveEnter, resolveButton;
    var firstDone = new Promise(function (resolve) { resolveFirst = resolve; });
    var enterDone = new Promise(function (resolve) { resolveEnter = resolve; });
    var buttonDone = new Promise(function (resolve) { resolveButton = resolve; });

    function finishFirst() {
        if (phase !== 0) return;
        phase = 1;
        console.log('HTML_UI_FIRST_DONE');
        resolveFirst();
    }
    function finishEnter() {
        if (phase !== 1) return;
        phase = 2;
        console.log('HTML_UI_ENTER_DONE');
        resolveEnter();
    }
    function finishButton() {
        if (phase !== 2) return;
        phase = 3;
        console.log('HTML_UI_BUTTON_DONE');
        resolveButton();
    }
    function checkSubmit(event, action) {
        check(event.isTrusted === true, 'guest ' + action + ' submit is trusted');
        check(event.submitter === button, 'guest ' + action + ' uses the actual submit button');
        check(input.value === 'guest', 'guest ' + action + ' submits the keyboard-edited value');
    }

    input.addEventListener('invalid', function (event) {
        if (phase === 0) {
            initialInvalid++;
            check(event.isTrusted === true && !event.bubbles && event.cancelable,
                'guest required click fires a trusted non-bubbling cancelable invalid');
            check(input.value === '', 'guest first click validates an empty control');
            finishFirst();
        } else if (phase === 1) {
            check(false, 'guest valid Enter reaches submit instead of invalid');
            finishEnter();
        } else if (phase === 2) {
            check(false, 'guest valid mouse click reaches submit instead of invalid');
            finishButton();
        }
    });
    form.addEventListener('submit', function (event) {
        // Even the intentionally broken image must stay on this fixture page.
        event.preventDefault();
        if (phase === 0) {
            initialSubmit++;
            check(false, 'guest empty required click blocks submit');
            finishFirst();
        } else if (phase === 1) {
            enterSubmit++;
            checkSubmit(event, 'Enter');
            finishEnter();
        } else if (phase === 2) {
            buttonSubmit++;
            checkSubmit(event, 'mouse');
            finishButton();
        } else {
            check(false, 'guest activation does not dispatch an extra submit');
        }
    });

    document.body.insertBefore(panel, document.body.firstChild);
    try {
        console.log('HTML_UI_READY');
        await firstDone;
        check(initialInvalid === 1 && initialSubmit === 0,
            'guest empty required button click is rejected exactly once');
        await enterDone;
        check(enterSubmit === 1, 'guest Enter submits exactly once');
        await buttonDone;
        check(enterSubmit === 1 && buttonSubmit === 1,
            'guest Enter and mouse button each submit exactly once');
    } finally {
        panel.remove();
    }
}

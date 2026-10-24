/* Shared guest/browser semantics fixture. The harness supplies check(bool,
 * label). Every submit listener cancels navigation so later assertions still
 * execute in this document; native click/Enter consumers are exercised by
 * the outer device driver against the same required controls. */
async function testHTMLForms(check) {
    function element(tag, attrs) {
        var node = document.createElement(tag);
        Object.keys(attrs || {}).forEach(function (name) {
            node.setAttribute(name, attrs[name]);
        });
        return node;
    }
    function throws(fn, name) {
        try { fn(); } catch (error) { return error.name === name; }
        return false;
    }

    var host = element('div');
    document.body.appendChild(host);
    try {
        var form = element('form', { id: 'validation-main' });
        var input = element('input', { required: '', name: 'text' });
        var button = element('button', { type: 'submit', name: 'send', value: 'yes' });
        form.appendChild(input);
        form.appendChild(button);
        host.appendChild(form);

        var submissions = [], invalid = 0, capture = 0, bubble = 0;
        form.addEventListener('submit', function (event) {
            submissions.push(event);
            event.preventDefault();
        });
        form.addEventListener('invalid', function () { capture++; }, true);
        form.addEventListener('invalid', function () { bubble++; });
        input.oninvalid = function (event) {
            invalid++;
            check(event.cancelable && !event.bubbles && event.target === input,
                'invalid is cancelable, non-bubbling and targets the control');
        };
        var state = input.validity;
        check(state instanceof ValidityState && state === input.validity,
            'ValidityState has its interface and SameObject identity');
        check(state.valueMissing && !state.valid && input.willValidate,
            'required empty input is a candidate with valueMissing');
        check(input.validationMessage.length > 0, 'invalid control has a validation message');
        check(!input.checkValidity() && invalid === 1 && capture === 1 && bubble === 0,
            'checkValidity fires one invalid through capture without bubbling');
        check(!form.checkValidity() && invalid === 2,
            'form.checkValidity validates its owned controls');
        form.requestSubmit(button);
        check(submissions.length === 0 && invalid === 3,
            'invalid requestSubmit blocks submit before navigation');
        input.oninvalid = function (event) { invalid++; event.preventDefault(); };
        check(!input.reportValidity(), 'canceling invalid does not make reportValidity true');
        form.requestSubmit(button);
        check(submissions.length === 0, 'canceling invalid does not allow submission');
        input.oninvalid = null;

        input.value = 'ready';
        check(state.valid && !state.valueMissing && input.validationMessage === '',
            'previous ValidityState object reads current value');
        input.setCustomValidity('Please choose another value.');
        check(state.customError && !state.valid && input.validationMessage === 'Please choose another value.',
            'custom error drives live state and message');
        input.disabled = true;
        check(!input.willValidate && input.checkValidity() && state.customError && input.validationMessage === '',
            'disabled control keeps custom state but is barred from validation');
        input.disabled = false;
        input.setCustomValidity('');
        form.requestSubmit(button);
        check(submissions.length === 1 && submissions[0] instanceof SubmitEvent &&
            submissions[0].submitter === button && submissions[0].isTrusted,
            'requestSubmit dispatches a native SubmitEvent with the actual submitter');
        form.requestSubmit();
        check(submissions.length === 2 && submissions[1].submitter === null,
            'omitted requestSubmit submitter is null');

        input.value = '';
        form.setAttribute('novalidate', '');
        form.requestSubmit(button);
        check(submissions.length === 3, 'novalidate bypasses interactive validation');
        check(!form.checkValidity(), 'novalidate does not change explicit checkValidity');
        form.removeAttribute('novalidate');
        button.setAttribute('formnovalidate', '');
        form.requestSubmit(button);
        check(submissions.length === 4, 'submitter formnovalidate bypasses validation');
        button.removeAttribute('formnovalidate');
        check(throws(function () { form.requestSubmit(input); }, 'TypeError'),
            'requestSubmit rejects a non-submit control');
        check(throws(function () { form.requestSubmit({}); }, 'TypeError'),
            'requestSubmit rejects an unbranded object');
        var foreignForm = element('form'), foreignButton = element('button');
        foreignForm.appendChild(foreignButton);
        host.appendChild(foreignForm);
        check(throws(function () { form.requestSubmit(foreignButton); }, 'NotFoundError'),
            'requestSubmit rejects a submit button owned by another form');
        check(throws(function () { input.checkValidity.call(form); }, 'TypeError'),
            'constraint method validates its interface receiver');
        check(throws(function () { return Object.getOwnPropertyDescriptor(ValidityState.prototype, 'valid').get.call({}); }, 'TypeError'),
            'ValidityState getter rejects a forged receiver');

        var detached = element('input', { required: '' });
        detached.value = 'stored before insertion';
        detached.setCustomValidity('detached error');
        var detachedState = detached.validity;
        check(!detached.checkValidity() && detachedState.customError,
            'disconnected controls expose real native validation state');
        form.appendChild(detached);
        check(detached.value === 'stored before insertion' &&
            detached.validity === detachedState && detachedState.customError,
            'insertion preserves detached value and ValidityState identity');
        detached.setCustomValidity('');
        var detachedCheckbox = element('input', { type: 'checkbox', required: '' });
        detachedCheckbox.checked = true;
        form.appendChild(detachedCheckbox);
        check(detachedCheckbox.checked && detachedCheckbox.validity.valid,
            'insertion preserves detached checkedness');
        var detachedForm = element('form'), detachedField = element('input', { required: '' });
        detachedForm.appendChild(detachedField);
        check(!detachedForm.checkValidity() && detachedField.form === detachedForm,
            'detached form checks controls in its own tree');
        var detachedSubmitEvents = 0;
        detachedForm.addEventListener('submit', function () { detachedSubmitEvents++; });
        detachedForm.submit();
        check(detachedSubmitEvents === 0, 'submit() dispatches no submit event on a disconnected form');

        var external = element('input', { form: 'validation-main', required: '' });
        host.appendChild(external);
        input.value = 'ready';
        check(external.form === form && !form.checkValidity(),
            'form attribute associates an out-of-subtree required control');
        external.value = 'ready';
        external.setAttribute('form', '');
        form.appendChild(external);
        check(external.form === null, 'present empty form attribute overrides connected ancestry');
        external.removeAttribute('form');
        check(external.form === form, 'removing form attribute restores nearest ancestor owner');
        var wrongId = element('div', { id: 'validation-duplicate' });
        var sameIdForm = element('form', { id: 'validation-duplicate' });
        host.appendChild(wrongId);
        host.appendChild(sameIdForm);
        external.setAttribute('form', 'validation-duplicate');
        check(external.form === null, 'first non-form ID blocks a later same-ID form');
        external.removeAttribute('form');
        detachedField.setAttribute('form', 'validation-main');
        check(detachedField.form === detachedForm,
            'detached control does not borrow an owner from the connected document');

        var fieldset = element('fieldset', { disabled: '' });
        var firstLegend = element('legend'), secondLegend = element('legend');
        var allowed = element('input', { required: '' });
        var barred = element('input', { required: '' });
        var second = element('input', { required: '' });
        firstLegend.appendChild(allowed);
        secondLegend.appendChild(second);
        fieldset.appendChild(firstLegend);
        fieldset.appendChild(barred);
        fieldset.appendChild(secondLegend);
        form.appendChild(fieldset);
        check(allowed.willValidate && !allowed.checkValidity(),
            'disabled fieldset exempts descendants of its first legend');
        check(!barred.willValidate && barred.checkValidity() && !second.willValidate,
            'fieldset bars ordinary descendants and later legends');
        allowed.value = 'ready';
        var readOnly = element('textarea', { required: '', readonly: '' });
        form.appendChild(readOnly);
        check(!readOnly.willValidate && readOnly.checkValidity(),
            'readonly textarea is barred');

        var checkbox = element('input', { type: 'checkbox', required: '' });
        form.appendChild(checkbox);
        check(checkbox.validity.valueMissing && !checkbox.checkValidity(), 'required checkbox needs checkedness');
        checkbox.checked = true;
        check(checkbox.validity.valid && checkbox.value === 'on', 'checked checkbox passes and defaults value to on');
        var radioA = element('input', { type: 'radio', name: 'group', required: '' });
        var radioB = element('input', { type: 'radio', name: 'group', form: 'validation-main' });
        form.appendChild(radioA);
        host.appendChild(radioB);
        check(radioA.validity.valueMissing && radioB.validity.valueMissing,
            'required radio group marks every member missing');
        radioB.checked = true;
        check(radioA.validity.valid && radioB.validity.valid, 'checked external radio satisfies its group');
        radioA.checked = true;
        check(!radioB.checked && radioA.checked, 'checking a radio clears its same-owner peer');

        var select = element('select', { required: '' });
        var placeholder = element('option', { value: '' });
        var choice = element('option', { value: 'chosen' });
        select.appendChild(placeholder); select.appendChild(choice); form.appendChild(select);
        check(select.validity.valueMissing, 'single select first empty placeholder fails required');
        choice.selected = true;
        check(select.validity.valid && select.value === 'chosen', 'option selectedness satisfies required select');
        var multi = element('select', { required: '', multiple: '' });
        var emptyChoice = element('option', { value: '' });
        var extraChoice = element('option', { value: 'extra' });
        multi.appendChild(emptyChoice); multi.appendChild(extraChoice); form.appendChild(multi);
        check(multi.validity.valueMissing, 'required multiple select fails with no selected option');
        emptyChoice.selected = true; extraChoice.selected = true;
        check(multi.validity.valid && emptyChoice.selected && extraChoice.selected,
            'multiple select keeps both selections and permits an empty selected value');

        var email = element('input', { type: 'email' });
        email.value = 'not-an-email';
        check(email.validity.typeMismatch, 'email rejects a missing-at address');
        email.value = ' person@example.com ';
        check(email.value === 'person@example.com' && email.validity.valid, 'email sanitizes surrounding ASCII whitespace');
        email.multiple = true;
        email.value = 'one@example.com, two@example.org';
        check(email.value === 'one@example.com,two@example.org' && email.validity.valid,
            'multiple email validates and sanitizes each address');
        email.value = 'one@example.com,';
        check(email.validity.typeMismatch, 'multiple email rejects an empty list member');
        var url = element('input', { type: 'url' });
        url.value = '/relative';
        check(url.validity.typeMismatch, 'URL input requires an absolute URL');
        url.value = 'https://example.com/path';
        check(url.validity.valid, 'URL input accepts an absolute WHATWG URL');
        url.value = 'https://invalid host/';
        check(url.validity.typeMismatch, 'URL uses real host parsing');
        var pattern = element('input', { pattern: '[A-Z]{2}' });
        pattern.value = 'ab';
        var hasUnicodeSets = false;
        try {
            var consonants = new RegExp('[[a-z]&&[^aeiou]]', 'v');
            hasUnicodeSets = consonants.test('b') && !consonants.test('a');
        } catch (error) {}
        check(hasUnicodeSets ? pattern.validity.patternMismatch : !pattern.validity.patternMismatch,
            'pattern validation does not substitute u semantics for unavailable Unicode sets');

        var number = element('input', { type: 'number', min: '0.2', max: '1', step: '0.1' });
        number.value = '0.3';
        check(number.validity.valid, 'decimal number step accepts ordinary binary-rounding error');
        number.value = '0.35';
        check(number.validity.stepMismatch, 'number step detects an off-lattice value');
        number.value = '0.1';
        check(number.validity.rangeUnderflow, 'number validates minimum');
        number.value = '1.1';
        check(number.validity.rangeOverflow, 'number validates maximum');
        number.setAttribute('step', 'any'); number.value = '0.35';
        check(!number.validity.stepMismatch && number.validity.valid, 'step=any removes the step constraint');
        number.value = 'not-a-number';
        check(number.value === '' && !number.validity.badInput,
            'script number assignment sanitizes invalid text without claiming user badInput');
        var defaultStep = element('input', { type: 'number' });
        defaultStep.value = '0.5';
        check(defaultStep.validity.stepMismatch, 'number defaults to step one');
        defaultStep.setAttribute('value', '0.5'); defaultStep.value = '1.5';
        check(defaultStep.validity.valid, 'number step base uses the value content attribute when min is absent');
        defaultStep.setAttribute('min', ' +2junk'); defaultStep.value = '1';
        check(defaultStep.validity.rangeUnderflow, 'numeric attributes use the HTML prefix parser');
        var date = element('input', { type: 'date' });
        date.value = '2026-09-30';
        check(date.type === 'text' && date.value === '2026-09-30',
            'unimplemented temporal input states honestly expose the text fallback');
        var length = element('textarea', { minlength: '4', maxlength: '6' });
        length.value = 'x';
        check(!length.validity.tooShort, 'script assignment is excluded from user-only minlength validation');
        length.value = 'abcdefgh';
        check(!length.validity.tooLong, 'script assignment is excluded from user-only maxlength validation');
        length.value = 'a\r\nb';
        check(length.value === 'a\nb', 'textarea API value normalizes CRLF to LF');

        input.setCustomValidity('persists');
        form.reset();
        check(input.validity.customError && input.validationMessage === 'persists',
            'form reset keeps the explicit custom error');
        input.setCustomValidity('');
        check(Array.prototype.indexOf.call(form.elements, external) >= 0 &&
            Array.prototype.indexOf.call(form.elements, radioB) >= 0,
            'form controls include real owned external nodes');
    } finally {
        host.remove();
    }
}

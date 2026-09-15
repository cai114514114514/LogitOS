/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 * Copyright 2009 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

/* A real inherited property, not an ancestor hit-test veto. Keeping it in
 * LibCSS makes important, unset/revert and computed style agree; css_extra
 * must not add a second producer. SVG paint-specific keywords stay absent.
 * Correction 2026-09-10: all also participates in the HTML box hit contract.
 * Rejecting it leaves a dialog inheriting none from its positioning wrapper:
 * the local ordinary-modal regression painted the dialog but all native clicks
 * hit its backdrop (17/80 host assertions red before this addition). Preserve
 * the distinct computed keyword; do not replace author CSS with auto. SVG
 * fill/stroke/path geometry hit testing is deliberately not added here. */
css_error css__cascade_pointer_events(uint32_t opv, css_style *style,
		css_select_state *state)
{
	uint16_t value = CSS_POINTER_EVENTS_INHERIT;

	UNUSED(style);

	if (hasFlagValue(opv) == false) {
		switch (getValue(opv)) {
		case POINTER_EVENTS_AUTO:
			value = CSS_POINTER_EVENTS_AUTO;
			break;
		case POINTER_EVENTS_NONE:
			value = CSS_POINTER_EVENTS_NONE;
			break;
		case POINTER_EVENTS_ALL:
			value = CSS_POINTER_EVENTS_ALL;
			break;
		}
	}

	if (css__outranks_existing(getOpcode(opv), isImportant(opv), state,
			getFlagValue(opv))) {
		return set_pointer_events(state->computed, value);
	}

	return CSS_OK;
}

css_error css__set_pointer_events_from_hint(const css_hint *hint,
		css_computed_style *style)
{
	return set_pointer_events(style, hint->status);
}

css_error css__initial_pointer_events(css_select_state *state)
{
	return set_pointer_events(state->computed, CSS_POINTER_EVENTS_AUTO);
}

css_error css__copy_pointer_events(
		const css_computed_style *from,
		css_computed_style *to)
{
	if (from == to) {
		return CSS_OK;
	}

	return set_pointer_events(to, get_pointer_events(from));
}

css_error css__compose_pointer_events(const css_computed_style *parent,
		const css_computed_style *child,
		css_computed_style *result)
{
	uint8_t type = get_pointer_events(child);

	return css__copy_pointer_events(
			type == CSS_POINTER_EVENTS_INHERIT ? parent : child,
			result);
}

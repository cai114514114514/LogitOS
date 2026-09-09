/*
 * border_radius.c -- LogitOS ADDITION to LibCSS. Not upstream NetSurf code.
 *
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * The select half of border-radius: cascade / hint / initial / copy / compose
 * for each of the four corner longhands. This is boilerplate on the
 * border_spacing.c pattern -- the (horizontal, vertical) pair behind one
 * opcode -- and the only reason it is four copies rather than a loop is that
 * the propget/propset accessors the generator emits are per-property inline
 * functions, not an indexed table.
 *
 * INITIAL VALUE IS 0px AND THE TYPE IS *SET*, NOT INHERIT. border-radius is
 * not an inherited property; the initial value of every corner is 0, which is
 * what makes an un-declared corner square rather than "ask the parent".
 */

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "select/propset.h"
#include "select/propget.h"
#include "utils/utils.h"

#include "select/properties/properties.h"
#include "select/properties/helpers.h"

#define BORDER_RADIUS_CORNER(pname)					\
css_error css__cascade_##pname(uint32_t opv, css_style *style,		\
		css_select_state *state)				\
{									\
	uint16_t value = CSS_BORDER_RADIUS_INHERIT;			\
	css_fixed hlength = 0, vlength = 0;				\
	uint32_t hunit = UNIT_PX, vunit = UNIT_PX;			\
									\
	if (hasFlagValue(opv) == false) {				\
		value = CSS_BORDER_RADIUS_SET;				\
		hlength = *((css_fixed *) style->bytecode);		\
		advance_bytecode(style, sizeof(hlength));		\
		hunit = *((uint32_t *) style->bytecode);		\
		advance_bytecode(style, sizeof(hunit));			\
									\
		vlength = *((css_fixed *) style->bytecode);		\
		advance_bytecode(style, sizeof(vlength));		\
		vunit = *((uint32_t *) style->bytecode);		\
		advance_bytecode(style, sizeof(vunit));			\
	}								\
									\
	hunit = css__to_css_unit(hunit);				\
	vunit = css__to_css_unit(vunit);				\
									\
	if (css__outranks_existing(getOpcode(opv), isImportant(opv),	\
			state, getFlagValue(opv))) {			\
		return set_##pname(state->computed, value,		\
				hlength, hunit, vlength, vunit);	\
	}								\
									\
	return CSS_OK;							\
}									\
									\
css_error css__set_##pname##_from_hint(const css_hint *hint,		\
		css_computed_style *style)				\
{									\
	return set_##pname(style, hint->status,				\
		hint->data.position.h.value, hint->data.position.h.unit,	\
		hint->data.position.v.value, hint->data.position.v.unit);\
}									\
									\
css_error css__initial_##pname(css_select_state *state)			\
{									\
	return set_##pname(state->computed, CSS_BORDER_RADIUS_SET,	\
			0, CSS_UNIT_PX, 0, CSS_UNIT_PX);		\
}									\
									\
css_error css__copy_##pname(const css_computed_style *from,		\
		css_computed_style *to)					\
{									\
	css_fixed hlength = 0, vlength = 0;				\
	css_unit hunit = CSS_UNIT_PX, vunit = CSS_UNIT_PX;		\
	uint8_t type = get_##pname(from, &hlength, &hunit,		\
			&vlength, &vunit);				\
									\
	if (from == to)							\
		return CSS_OK;						\
									\
	return set_##pname(to, type, hlength, hunit, vlength, vunit);	\
}									\
									\
css_error css__compose_##pname(const css_computed_style *parent,	\
		const css_computed_style *child,			\
		css_computed_style *result)				\
{									\
	css_fixed hlength = 0, vlength = 0;				\
	css_unit hunit = CSS_UNIT_PX, vunit = CSS_UNIT_PX;		\
	uint8_t type = get_##pname(child, &hlength, &hunit,		\
			&vlength, &vunit);				\
									\
	return css__copy_##pname(					\
			type == CSS_BORDER_RADIUS_INHERIT ? parent : child, \
			result);					\
}

BORDER_RADIUS_CORNER(border_top_left_radius)
BORDER_RADIUS_CORNER(border_top_right_radius)
BORDER_RADIUS_CORNER(border_bottom_right_radius)
BORDER_RADIUS_CORNER(border_bottom_left_radius)

#undef BORDER_RADIUS_CORNER

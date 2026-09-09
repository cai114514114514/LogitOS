/*
 * border_radius.c -- LogitOS ADDITION to LibCSS. Not upstream NetSurf code.
 *
 * Licensed under the MIT License,
 *		  http://www.opensource.org/licenses/mit-license.php
 *
 * WHY THIS FILE IS HAND-WRITTEN AND NOT A LINE IN properties.gen.
 * The generator emits ONE value per property. A border-radius corner is a
 * PAIR -- a horizontal and a vertical radius -- and the shorthand's grammar is
 *
 *     <length-percentage>{1,4} [ / <length-percentage>{1,4} ]?
 *
 * which the generator's grammar cannot express either. The shape below is
 * css__parse_border_spacing's (two lengths behind one OPV) crossed with
 * css__parse_padding's (the 1/2/3/4 fill rule), both of which are in this
 * directory already.
 *
 * WHAT THE SHORTHAND DOES, AND WHY IT DOES IT AT PARSE TIME.
 * `border-radius` has NO opcode of its own. It expands here into the four
 * corner longhands, exactly the way `padding` expands into the four padding
 * sides. That is what puts every corner through the real cascade: origin,
 * specificity, source order and !important are all decided per longhand by
 * machinery that already exists and is already correct, and there is exactly
 * one place a corner's used value can come from.
 *
 * THE CORNER ORDER IS TL TR BR BL, CLOCKWISE FROM TOP-LEFT, and the fill rule
 * for a short list is the box-side rule with the OPPOSITE corner as the
 * partner rather than the opposite side:
 *   1 value  -> all four
 *   2 values -> [0] = TL,BR   [1] = TR,BL
 *   3 values -> [0] = TL      [1] = TR,BL   [2] = BR
 *   4 values -> TL TR BR BL
 * Getting that wrong is a plausible-wrong-answer: `border-radius: 8px 0` would
 * round the top edge instead of the two diagonal corners, which looks like a
 * design choice rather than a bug.
 *
 * NEGATIVE VALUES ARE INVALID, not clamped. CSS says so, and clamping would
 * accept a declaration the author's fallback was written for.
 */

#include <assert.h>
#include <string.h>

#include "bytecode/bytecode.h"
#include "bytecode/opcodes.h"
#include "parse/properties/properties.h"
#include "parse/properties/utils.h"

/* TL, TR, BR, BL -- clockwise from the top-left. */
static const uint16_t corner_prop[4] = {
	CSS_PROP_BORDER_TOP_LEFT_RADIUS,
	CSS_PROP_BORDER_TOP_RIGHT_RADIUS,
	CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS,
	CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS
};

/**
 * One <length-percentage>, non-negative, no angle/time/frequency.
 */
static css_error parse_radius_lp(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_fixed *length, uint32_t *unit)
{
	css_error error;

	error = css__parse_unit_specifier(c, vector, ctx, UNIT_PX, length, unit);
	if (error != CSS_OK)
		return error;

	if ((*unit & UNIT_ANGLE) || (*unit & UNIT_TIME) || (*unit & UNIT_FREQ))
		return CSS_INVALID;

	if (*length < 0)
		return CSS_INVALID;

	return CSS_OK;
}

/**
 * Emit one corner: OPV + horizontal (length, unit) + vertical (length, unit).
 * The layout matches css__cascade_border_*_radius in
 * src/select/properties/border_radius.c, which is the only reader.
 */
static css_error append_corner(css_style *result, uint16_t prop,
		css_fixed hlen, uint32_t hunit,
		css_fixed vlen, uint32_t vunit)
{
	css_error error;

	error = css__stylesheet_style_appendOPV(result, prop, 0,
			BORDER_RADIUS_SET);
	if (error != CSS_OK)
		return error;

	return css__stylesheet_style_vappend(result, 4,
			hlen, hunit, vlen, vunit);
}

/**
 * Parse the border-radius shorthand.
 */
css_error css__parse_border_radius(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	int32_t orig_ctx = *ctx;
	int32_t prev_ctx;
	css_error error;
	const css_token *token;
	css_fixed h[4] = { 0, 0, 0, 0 }, v[4] = { 0, 0, 0, 0 };
	uint32_t hu[4] = { 0, 0, 0, 0 }, vu[4] = { 0, 0, 0, 0 };
	int nh = 0, nv = 0, i;
	enum flag_value flag_value;
	/* [n_given - 1][corner] -> which of the parsed values that corner
	 * takes. Row 0 is the 1-value case, row 3 the 4-value case. */
	static const int pick[4][4] = {
		{ 0, 0, 0, 0 },
		{ 0, 1, 0, 1 },
		{ 0, 1, 2, 1 },
		{ 0, 1, 2, 3 }
	};

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		for (i = 0; i < 4; i++) {
			error = css_stylesheet_style_flag_value(result,
					flag_value, corner_prop[i]);
			if (error != CSS_OK) {
				*ctx = orig_ctx;
				return error;
			}
		}
		parserutils_vector_iterate(vector, ctx);
		return CSS_OK;
	}

	/* Up to four horizontal radii. */
	do {
		prev_ctx = *ctx;

		if (is_css_inherit(c, token)) {
			*ctx = orig_ctx;
			return CSS_INVALID;
		}

		error = parse_radius_lp(c, vector, ctx, &h[nh], &hu[nh]);
		if (error != CSS_OK) {
			if (nh == 0) {
				*ctx = orig_ctx;
				return CSS_INVALID;
			}
			break;
		}

		nh++;
		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);
	} while (*ctx != prev_ctx && token != NULL && nh < 4);

	/* Optional `/ <lp>{1,4}` for the vertical radii. */
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL && tokenIsChar(token, '/')) {
		parserutils_vector_iterate(vector, ctx);
		consumeWhitespace(vector, ctx);
		token = parserutils_vector_peek(vector, *ctx);

		do {
			prev_ctx = *ctx;

			if (token == NULL || is_css_inherit(c, token)) {
				*ctx = orig_ctx;
				return CSS_INVALID;
			}

			error = parse_radius_lp(c, vector, ctx, &v[nv], &vu[nv]);
			if (error != CSS_OK) {
				if (nv == 0) {
					/* A `/` with nothing usable after it
					 * is a malformed declaration, not a
					 * horizontal-only one. */
					*ctx = orig_ctx;
					return CSS_INVALID;
				}
				break;
			}

			nv++;
			consumeWhitespace(vector, ctx);
			token = parserutils_vector_peek(vector, *ctx);
		} while (*ctx != prev_ctx && token != NULL && nv < 4);
	}

	for (i = 0; i < 4; i++) {
		int hi = pick[nh - 1][i];
		int vi = nv > 0 ? pick[nv - 1][i] : hi;

		error = append_corner(result, corner_prop[i],
				h[hi], hu[hi],
				nv > 0 ? v[vi] : h[hi],
				nv > 0 ? vu[vi] : hu[hi]);
		if (error != CSS_OK) {
			*ctx = orig_ctx;
			return error;
		}
	}

	return CSS_OK;
}

/**
 * Parse one corner longhand: <lp> <lp>?  |  inherit
 */
static css_error parse_corner(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result, uint16_t prop)
{
	int32_t orig_ctx = *ctx;
	css_error error;
	const css_token *token;
	css_fixed hlen = 0, vlen = 0;
	uint32_t hunit = 0, vunit = 0;
	enum flag_value flag_value;

	token = parserutils_vector_peek(vector, *ctx);
	if (token == NULL)
		return CSS_INVALID;

	flag_value = get_css_flag_value(c, token);
	if (flag_value != FLAG_VALUE__NONE) {
		parserutils_vector_iterate(vector, ctx);
		return css_stylesheet_style_flag_value(result, flag_value, prop);
	}

	error = parse_radius_lp(c, vector, ctx, &hlen, &hunit);
	if (error != CSS_OK) {
		*ctx = orig_ctx;
		return CSS_INVALID;
	}

	vlen = hlen;
	vunit = hunit;

	consumeWhitespace(vector, ctx);
	token = parserutils_vector_peek(vector, *ctx);
	if (token != NULL) {
		int32_t save = *ctx;
		if (parse_radius_lp(c, vector, ctx, &vlen, &vunit) != CSS_OK) {
			/* Not a second radius. Leave it for the core
			 * !important parser, as border-spacing does. */
			*ctx = save;
			vlen = hlen;
			vunit = hunit;
		}
	}

	error = append_corner(result, prop, hlen, hunit, vlen, vunit);
	if (error != CSS_OK)
		*ctx = orig_ctx;

	return error;
}

css_error css__parse_border_top_left_radius(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_corner(c, vector, ctx, result,
			CSS_PROP_BORDER_TOP_LEFT_RADIUS);
}

css_error css__parse_border_top_right_radius(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_corner(c, vector, ctx, result,
			CSS_PROP_BORDER_TOP_RIGHT_RADIUS);
}

css_error css__parse_border_bottom_right_radius(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_corner(c, vector, ctx, result,
			CSS_PROP_BORDER_BOTTOM_RIGHT_RADIUS);
}

css_error css__parse_border_bottom_left_radius(css_language *c,
		const parserutils_vector *vector, int32_t *ctx,
		css_style *result)
{
	return parse_corner(c, vector, ctx, result,
			CSS_PROP_BORDER_BOTTOM_LEFT_RADIUS);
}

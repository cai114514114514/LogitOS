/*
 * This file is part of LibCSS
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 *
 * Copyright 2018 Michael Drake <tlsa@netsurf-browser.org>
 */

#ifndef css_select_mq_h_
#define css_select_mq_h_

#include "select/helpers.h"
#include "select/strings.h"
#include "select/unit.h"

static inline bool mq_match_feature_range_length_op1(
		css_mq_feature_op op,
		const css_mq_value *value,
		const css_fixed client_len,
		const css_unit_ctx *unit_ctx)
{
	css_fixed v;

	if (value->type != CSS_MQ_VALUE_TYPE_DIM) {
		return false;
	}

	if (value->data.dim.unit != UNIT_PX) {
		v = css_unit_len2px_mq(unit_ctx,
				value->data.dim.len,
				css__to_css_unit(value->data.dim.unit));
	} else {
		v = value->data.dim.len;
	}

	switch (op) {
	case CSS_MQ_FEATURE_OP_BOOL: return false;
	case CSS_MQ_FEATURE_OP_LT:   return v <  client_len;
	case CSS_MQ_FEATURE_OP_LTE:  return v <= client_len;
	case CSS_MQ_FEATURE_OP_EQ:   return v == client_len;
	case CSS_MQ_FEATURE_OP_GTE:  return v >= client_len;
	case CSS_MQ_FEATURE_OP_GT:   return v >  client_len;
	default:
		return false;
	}
}

static inline bool mq_match_feature_range_length_op2(
		css_mq_feature_op op,
		const css_mq_value *value,
		const css_fixed client_len,
		const css_unit_ctx *unit_ctx)
{
	css_fixed v;

	if (op == CSS_MQ_FEATURE_OP_UNUSED) {
		return true;
	}
	if (value->type != CSS_MQ_VALUE_TYPE_DIM) {
		return false;
	}

	if (value->data.dim.unit != UNIT_PX) {
		v = css_unit_len2px_mq(unit_ctx,
				value->data.dim.len,
				css__to_css_unit(value->data.dim.unit));
	} else {
		v = value->data.dim.len;
	}

	switch (op) {
	case CSS_MQ_FEATURE_OP_LT:  return client_len <  v;
	case CSS_MQ_FEATURE_OP_LTE: return client_len <= v;
	case CSS_MQ_FEATURE_OP_EQ:  return client_len == v;
	case CSS_MQ_FEATURE_OP_GTE: return client_len >= v;
	case CSS_MQ_FEATURE_OP_GT:  return client_len >  v;
	default:
		return false;
	}
}

static inline bool mq_match_feature_eq_ident_op1(
		css_mq_feature_op op,
		const css_mq_value *value,
		const lwc_string *client_value)
{
	bool is_match;

	if (value->type != CSS_MQ_VALUE_TYPE_IDENT) {
		return false;
	}

	if (value->data.ident == NULL || client_value == NULL) {
		return false;
	}

	switch (op) {
	case CSS_MQ_FEATURE_OP_EQ:
		return (lwc_string_isequal(value->data.ident,
				client_value, &is_match) == lwc_error_ok) &&
				is_match;
	default:
		return false;
	}
}

/**
 * Match media query features.
 *
 * \param[in] feat      Condition to match.
 * \param[in] unit_ctx  Current unit conversion context.
 * \param[in] media     Current media spec, to check against feat.
 * \return true if condition matches, otherwise false.
 */
static inline bool mq_match_feature(
		const css_mq_feature *feat,
		const css_unit_ctx *unit_ctx,
		const css_media *media,
		const css_select_strings *str)
{
	bool match;

#ifndef CSS_DEVICE_MEDIA_NEGCTL
	/* The device dimensions are supplied by the embedder, never inferred from
	 * viewport width/height. A real telemetry script binary-searched
	 * min-device-width; an always-false unknown-feature answer made it issue
	 * 248,073 queries (past negative widths) before the diagnostic fuel guard.
	 * Reuse the range operators so stylesheet @media and matchMedia agree. */
	css_fixed device_len = 0;
	bool device_feature = false;
	if (lwc_string_isequal(feat->name, str->device_width, &match) == lwc_error_ok && match) {
		device_len = media->device_width; device_feature = true;
	} else if (lwc_string_isequal(feat->name, str->device_height, &match) == lwc_error_ok && match) {
		device_len = media->device_height; device_feature = true;
	}
	if (device_feature) {
		/* Zero denotes an embedder with no display information, not a tiny
		 * viewport. Do not manufacture a positive device verdict there. */
		if (device_len <= 0) return false;
		if (feat->op == CSS_MQ_FEATURE_OP_BOOL) return true;
		return mq_match_feature_range_length_op1(feat->op, &feat->value, device_len, unit_ctx) &&
		       mq_match_feature_range_length_op2(feat->op2, &feat->value2, device_len, unit_ctx);
	}
#endif

	/* One matcher serves both stylesheet @media and JS matchMedia. Boolean
	 * syntax is false for no-preference; unknown values and ranges are false. */
	if (lwc_string_isequal(feat->name, str->prefers_reduced_motion, &match) == lwc_error_ok && match) {
#ifdef OPENLOGIT_REDUCED_MOTION_DISABLED
		return false;
#else
		if (feat->op == CSS_MQ_FEATURE_OP_BOOL) return media->prefers_reduced_motion;
		if (feat->op != CSS_MQ_FEATURE_OP_EQ || feat->value.type != CSS_MQ_VALUE_TYPE_IDENT)
			return false;
		if (!feat->value.data.ident) return false;
		const char *value = lwc_string_data(feat->value.data.ident);
		size_t len = lwc_string_length(feat->value.data.ident);
		const char *want = media->prefers_reduced_motion ? "reduce" : "no-preference";
		if (len != (media->prefers_reduced_motion ? 6u : 13u)) return false;
		/* CSS keyword matching is ASCII-insensitive, unlike custom identifiers. */
		for (size_t j=0;j<len;j++) if (((unsigned char)value[j] | 32) != (unsigned char)want[j]) return false;
		return true;
#endif
	}

	/* TODO: Use interned string for comparison. */
	if (lwc_string_isequal(feat->name,
			str->width, &match) == lwc_error_ok &&
			match == true) {
		if (!mq_match_feature_range_length_op1(feat->op, &feat->value,
					media->width, unit_ctx)) {
			return false;
		}
		return mq_match_feature_range_length_op2(feat->op2,
				&feat->value2, media->width, unit_ctx);

	} else if (lwc_string_isequal(feat->name,
			str->height, &match) == lwc_error_ok &&
			match == true) {
		if (!mq_match_feature_range_length_op1(feat->op, &feat->value,
				media->height, unit_ctx)) {
			return false;
		}

		return mq_match_feature_range_length_op2(feat->op2,
				&feat->value2, media->height, unit_ctx);

	} else if (lwc_string_isequal(feat->name,
			str->prefers_color_scheme, &match) == lwc_error_ok &&
			match == true) {
		if (mq_match_feature_eq_ident_op1(feat->op, &feat->value,
				media->prefers_color_scheme) ||
		    feat->op == CSS_MQ_FEATURE_OP_BOOL) {
			return true;
		}

		return false;
	}

	/* TODO: Look at other feature names. */

	return false;
}

/**
 * Match media query conditions.
 *
 * \param[in] cond      Condition to match.
 * \param[in] unit_ctx  Current unit conversion context.
 * \param[in] media     Current media spec, to check against cond.
 * \return true if condition matches, otherwise false.
 */
static inline bool mq_match_condition(
		const css_mq_cond *cond,
		const css_unit_ctx *unit_ctx,
		const css_media *media,
		const css_select_strings *str)
{
	bool matched = !cond->op;

	for (uint32_t i = 0; i < cond->nparts; i++) {
		bool part_matched;
		if (cond->parts[i] == NULL) {
			/* <general-enclosed>: syntax inside the parentheses that
			 * is well-formed but means nothing to this UA --
			 * `(min-widtcalc(640px - 1px))`, a typo'd feature name
			 * that lexed as a FUNCTION, or a feature from a level of
			 * the spec we do not implement.
			 *
			 * mq_parse_media_in_parens accepts it and reports it as
			 * a NULL part, on purpose: the production exists so a
			 * future feature does not invalidate the whole query.
			 * The spec says it evaluates to FALSE. Reading through
			 * the NULL instead was a crash on any page whose
			 * stylesheet contained one -- reachable from ordinary
			 * @media parsing via mq_rule_good_for_media, not only
			 * from css_select_ctx_media_matches. Found by
			 * tests/unit/css_vars_fuzz.c. */
			part_matched = false;
		} else if (cond->parts[i]->type == CSS_MQ_FEATURE) {
			part_matched = mq_match_feature(
					cond->parts[i]->data.feat,
					unit_ctx, media, str);
		} else {
			assert(cond->parts[i]->type == CSS_MQ_COND);
			part_matched = mq_match_condition(
					cond->parts[i]->data.cond,
					unit_ctx, media, str);
		}

		if (cond->op) {
			/* OR */
			matched |= part_matched;
			if (matched) {
				break; /* Short-circuit */
			}
		} else {
			/* AND */
			matched &= part_matched;
			if (!matched) {
				break; /* Short-circuit */
			}
		}
	}

	return matched != cond->negate;
}

/**
 * Test whether media query list matches current media.
 *
 * If anything in the list matches, the list matches.  If none match
 * it doesn't match.
 *
 * \param[in] m         Media query list.
 * \param[in] unit_ctx  Current unit conversion context.
 * \param[in] media     Current media spec, to check against m.
 * \return true if media query list matches media
 */
static inline bool mq__list_match(
		const css_mq_query *m,
		const css_unit_ctx *unit_ctx,
		const css_media *media,
		const css_select_strings *str)
{
	for (; m != NULL; m = m->next) {
		/* Check type */
		if (!!(m->type & media->type) != m->negate_type) {
			if (m->cond == NULL ||
					mq_match_condition(m->cond,
							unit_ctx, media, str)) {
				/* We have a match, no need to look further. */
				return true;
			}
		}
	}

	return false;
}

/**
 * Test whether the rule applies for current media.
 *
 * \param rule      Rule to test
 * \param unit_ctx  Current unit conversion context.
 * \param media     Current media spec
 * \return true iff chain's rule applies for media
 */
static inline bool mq_rule_good_for_media(
		const css_rule *rule,
		const css_unit_ctx *unit_ctx,
		const css_media *media,
		const css_select_strings *str)
{
	bool applies = true;
	const css_rule *ancestor = rule;

	while (ancestor != NULL) {
		const css_rule_media *m = (const css_rule_media *) ancestor;

		if (ancestor->type == CSS_RULE_MEDIA) {
			applies = mq__list_match(m->media,
					unit_ctx, media, str);
			if (applies == false) {
				break;
			}
		}

		if (ancestor->ptype != CSS_RULE_PARENT_STYLESHEET) {
			ancestor = ancestor->parent;
		} else {
			ancestor = NULL;
		}
	}

	return applies;
}

#endif

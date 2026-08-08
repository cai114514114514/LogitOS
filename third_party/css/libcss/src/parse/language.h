/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2008 John-Mark Bell <jmb@netsurf-browser.org>
 */

#ifndef css_css__parse_language_h_
#define css_css__parse_language_h_

#include <parserutils/utils/stack.h>
#include <parserutils/utils/vector.h>

#include <libcss/functypes.h>
#include <libcss/types.h>

#include "lex/lex.h"
#include "parse/parse.h"
#include "parse/propstrings.h"

/**
 * CSS namespace mapping
 */
typedef struct css_namespace {
	lwc_string *prefix;		/**< Namespace prefix */
	lwc_string *uri;		/**< Namespace URI */
} css_namespace;

/**
 * Context for a CSS language parser
 */
typedef struct css_language {
	css_stylesheet *sheet;		/**< The stylesheet to parse for */

#define STACK_CHUNK 32
	parserutils_stack *context;	/**< Context stack */

	enum {
		CHARSET_PERMITTED,
		IMPORT_PERMITTED,
		NAMESPACE_PERMITTED,
		HAD_RULE
	} state;			/**< State flag, for at-rule handling */

	/** Interned strings */
	lwc_string **strings;

	lwc_string *default_namespace;	/**< Default namespace URI */
	css_namespace *namespaces;	/**< Array of namespace mappings */
	uint32_t num_namespaces;	/**< Number of namespace mappings */
} css_language;

css_error css__language_create(css_stylesheet *sheet, css_parser *parser,
		void **language);
css_error css__language_destroy(css_language *language);

/******************************************************************************
 * Helper functions                                                           *
 ******************************************************************************/

/**
 * Consume all leading whitespace tokens
 *
 * \param vector  The vector to consume from
 * \param ctx     The vector's context
 */
static inline void consumeWhitespace(const parserutils_vector *vector, int32_t *ctx)
{
	const css_token *token = NULL;

	while ((token = parserutils_vector_peek(vector, *ctx)) != NULL &&
			token->type == CSS_TOKEN_S)
		parserutils_vector_iterate(vector, ctx);
}

/**
 * Determine if a token is a character
 *
 * \param token  The token to consider
 * \param c      The character to match (lowercase ASCII only)
 * \return True if the token matches, false otherwise
 */
/* ---- LogitOS patch: the declaration-drop reporter ----------------------
 *
 * A page that renders as an unstyled column is a page whose declarations went
 * somewhere and did nothing, and until this hook existed there was no way to
 * ask WHICH ones. parseProperty() is the single funnel every declaration in
 * every sheet passes through, and it has exactly three ways to throw one away.
 * When the pointer is NULL (the shipping browser) this costs one predicted
 * branch per declaration and nothing else; the audit tool in
 * tests/unit/css_audit.c sets it and counts.
 *
 * Deliberately NOT a build-time #ifdef: the whole point is to measure the same
 * bytes the browser runs, and a separately-configured LibCSS is a different
 * parser. */
enum {
	CSS_DROP_UNKNOWN_PROP = 0,	/**< no such property in this LibCSS */
	CSS_DROP_BAD_VALUE    = 1,	/**< property known, its handler refused */
	CSS_DROP_TRAILING     = 2,	/**< junk after an otherwise valid value */
	CSS_DROP_ACCEPTED     = 3	/**< not a drop: the declaration was taken */
};
/** name/nlen is the property name as written; NULL to disable (the default). */
extern void (*css__parse_drop_report)(const char *name, size_t nlen, int reason);

static inline bool tokenIsChar(const css_token *token, uint8_t c)
{
	bool result = false;

	if (token != NULL && token->type == CSS_TOKEN_CHAR &&
	                lwc_string_length(token->idata) == 1) {
		char d = lwc_string_data(token->idata)[0];

		/* Ensure lowercase comparison */
		if ('A' <= d && d <= 'Z')
			d += 'a' - 'A';

		result = (d == c);
	}

	return result;
}

#endif


/*
 * This file is part of LibCSS.
 * Licensed under the MIT License,
 *                http://www.opensource.org/licenses/mit-license.php
 * Copyright 2008 John-Mark Bell <jmb@netsurf-browser.org>
 */

#include <stdbool.h>
#include <string.h>

#include <parserutils/charset/mibenum.h>

#include "charset/detect.h"
#include "utils/utils.h"

static parserutils_error css_charset_read_bom_or_charset(const uint8_t *data,
		size_t len, uint16_t *mibenum);
static parserutils_error try_unicode_charset(const uint8_t *data,
        size_t len, unsigned width, bool little, uint16_t *result);
static parserutils_error try_ascii_compatible_charset(const uint8_t *data,
		size_t len, uint16_t *result);

/**
 * Extract a charset from a chunk of data
 *
 * \param data     Pointer to buffer containing data
 * \param len      Buffer length
 * \param mibenum  Pointer to location containing current MIB enum
 * \param source   Pointer to location containing current charset source
 * \return PARSERUTILS_OK on success, appropriate error otherwise
 *
 * ::mibenum and ::source will be updated on exit
 *
 * CSS 2.1 $4.4
 */
parserutils_error css__charset_extract(const uint8_t *data, size_t len,
		uint16_t *mibenum, uint32_t *source)
{
	parserutils_error error;
	uint16_t charset = 0;

	if (data == NULL || mibenum == NULL || source == NULL)
		return PARSERUTILS_BADPARM;

	/* If the charset was dictated by the client, we've nothing to detect */
	if (*source == CSS_CHARSET_DICTATED)
		return PARSERUTILS_OK;

	/* Look for a BOM and/or @charset */
	error = css_charset_read_bom_or_charset(data, len, &charset);
	if (error != PARSERUTILS_OK)
		return error;

	if (charset != 0) {
		*mibenum = charset;
		*source = CSS_CHARSET_DOCUMENT;

		return PARSERUTILS_OK;
	}

	/* If we've already got a charset from the linking mechanism or
	 * referring document, then we've nothing further to do */
	if (*source != CSS_CHARSET_DEFAULT)
		return PARSERUTILS_OK;

	/* We've not yet found a charset, so use the default fallback */
	charset = parserutils_charset_mibenum_from_name("UTF-8", SLEN("UTF-8"));

	*mibenum = charset;
	*source = CSS_CHARSET_DEFAULT;

	return PARSERUTILS_OK;
}


/**
 * Inspect the beginning of a buffer of data for the presence of a
 * UTF Byte Order Mark and/or an @charset rule
 *
 * \param data     Pointer to buffer containing data
 * \param len      Buffer length
 * \param mibenum  Pointer to location to receive MIB enum
 * \return PARSERUTILS_OK on success, appropriate error otherwise
 */
parserutils_error css_charset_read_bom_or_charset(const uint8_t *data,
		size_t len, uint16_t *mibenum)
{
	parserutils_error error;
	uint16_t charset = 0;

	if (data == NULL)
		return PARSERUTILS_BADPARM;

	/* We require at least 4 bytes of data */
	if (len < 4)
		return PARSERUTILS_NEEDDATA;


	/* Look for BOM */
	if (data[0] == 0x00 && data[1] == 0x00 &&
			data[2] == 0xFE && data[3] == 0xFF) {
		charset = parserutils_charset_mibenum_from_name("UTF-32BE",
				SLEN("UTF-32BE"));
	} else if (data[0] == 0xFF && data[1] == 0xFE &&
			data[2] == 0x00 && data[3] == 0x00) {
		charset = parserutils_charset_mibenum_from_name("UTF-32LE",
				SLEN("UTF-32LE"));
	} else if (data[0] == 0xFE && data[1] == 0xFF) {
		charset = parserutils_charset_mibenum_from_name("UTF-16BE",
				SLEN("UTF-16BE"));
	} else if (data[0] == 0xFF && data[1] == 0xFE) {
		charset = parserutils_charset_mibenum_from_name("UTF-16LE",
				SLEN("UTF-16LE"));
	} else if (data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
		charset = parserutils_charset_mibenum_from_name("UTF-8",
				SLEN("UTF-8"));
	}

	/* BOM beats @charset.
	 * UAs differ here, but none appear to match the spec.
	 * The spec indicates that any @charset present in conjunction with a
	 * BOM, should match the BOM. In reality, it appears UAs just take the
	 * BOM as gospel and ignore any @charset rule. The w3c CSS validator
	 * appears to do the same (at the least, it doesn't complain about a
	 * mismatch).
	 */
	if (charset != 0) {
		*mibenum = charset;
		return PARSERUTILS_OK;
	}

	/* Previously the detector discarded NEEDDATA here. With a network split
	 * after "@cha", that committed UTF-8 before the declared name arrived.
	 * Each candidate now waits only while the bytes actually match its prefix. */
	for (unsigned width = 4; width >= 2; width -= 2) {
		for (unsigned order = 0; order < 2; order++) {
			error = try_unicode_charset(data, len, width, order == 0, &charset);
			if (error != PARSERUTILS_OK)
				return error;
			if (charset != 0) {
				*mibenum = charset;
				return PARSERUTILS_OK;
			}
		}
	}

	error = try_ascii_compatible_charset(data, len, &charset);
	if (error == PARSERUTILS_OK)
		*mibenum = charset;
	return error;
}

/* CSS's UTF-16/32 declaration forms contain ASCII code units only. Reading
 * bytes explicitly avoids unaligned loads and signed shifts on malformed data. */
static bool ascii_unit(const uint8_t *data, unsigned width, bool little,
        char *value)
{
	unsigned position = little ? 0 : width - 1;
	for (unsigned i = 0; i < width; i++) {
		if (i != position && data[i] != 0)
			return false;
	}
	if (data[position] > 127)
		return false;
	*value = (char)data[position];
	return true;
}

static parserutils_error try_unicode_charset(const uint8_t *data,
        size_t len, unsigned width, bool little, uint16_t *result)
{
	const char prefix[] = "@charset \"";
	size_t prefix_bytes = (sizeof(prefix) - 1) * width;
	*result = 0;
	for (size_t i = 0; i < prefix_bytes; i++) {
		if (i == len)
			return PARSERUTILS_NEEDDATA;
		unsigned position = little ? 0 : width - 1;
		uint8_t expected = i % width == position ? prefix[i / width] : 0;
		if (data[i] != expected)
			return PARSERUTILS_OK;
	}

	/* Only UTF-16/32 with an optional byte-order suffix is valid in this
	 * branch. Count the entire name: truncating a long label to eight bytes
	 * would silently accept "UTF-16LEgarbage" as a real encoding. */
	char name[8];
	size_t count = 0;
	for (size_t at = prefix_bytes; ; at += width) {
		if (len - at < width)
			return PARSERUTILS_NEEDDATA;
		char value;
		if (!ascii_unit(data + at, width, little, &value))
			return PARSERUTILS_OK;
		if (value == '"') {
			if (len - at < 2 * width)
				return PARSERUTILS_NEEDDATA;
			char next;
			if (!ascii_unit(data + at + width, width, little, &next) || next != ';')
				return PARSERUTILS_OK;
			const char *base = width == 2 ? "UTF-16" : "UTF-32";
			const char *suffix = little ? "LE" : "BE";
			if ((count == 6 || count == 8) && memcmp(name, base, 6) == 0 &&
					(count == 6 || memcmp(name + 6, suffix, 2) == 0)) {
				char label[9];
				memcpy(label, base, 6);
				memcpy(label + 6, suffix, 3);
				*result = parserutils_charset_mibenum_from_name(label, 8);
			}
			return PARSERUTILS_OK;
		}
		if (count < sizeof(name))
			name[count] = value >= 'a' && value <= 'z' ? value - 32 : value;
		count++;
	}
}

parserutils_error try_ascii_compatible_charset(const uint8_t *data, size_t len,
		uint16_t *result)
{
	uint16_t charset = 0;

#define CHARSET "@charset \""

	size_t prefix_length = len < SLEN(CHARSET) ? len : SLEN(CHARSET);
	if (memcmp(data, CHARSET, prefix_length) != 0) {
		*result = 0;
		return PARSERUTILS_OK;
	}
	if (len <= SLEN(CHARSET))
		return PARSERUTILS_NEEDDATA;

	/* Look for @charset, assuming ASCII-compatible source data */
	if (memcmp(data, CHARSET, SLEN(CHARSET)) == 0) {
		const uint8_t *start = data + SLEN(CHARSET);
		const uint8_t *end;

		/* Look for "; at end of charset declaration */
		for (end = start; end < data + len; end++) {
			if (*end == '"' && end < data + len - 1 &&
					*(end + 1) == ';')
				break;
		}

		if (end == data + len) {
			/* Ran out of input */
			return PARSERUTILS_NEEDDATA;
		}

		/* Convert to MIB enum */
		charset = parserutils_charset_mibenum_from_name(
				(const char *) start,  end - start);

		/* Any non-ASCII compatible charset must be ignored, as
		 * we've just used an ASCII parser to read it. */
		if (charset == parserutils_charset_mibenum_from_name(
					"UTF-32", SLEN("UTF-32")) ||
			charset == parserutils_charset_mibenum_from_name(
					"UTF-32LE", SLEN("UTF-32LE")) ||
			charset == parserutils_charset_mibenum_from_name(
					"UTF-32BE", SLEN("UTF-32BE")) ||
			charset == parserutils_charset_mibenum_from_name(
					"UTF-16", SLEN("UTF-16")) ||
			charset == parserutils_charset_mibenum_from_name(
					"UTF-16LE", SLEN("UTF-16LE")) ||
			charset == parserutils_charset_mibenum_from_name(
					"UTF-16BE", SLEN("UTF-16BE"))) {

			charset = 0;
		}
	}

#undef CHARSET

	*result = charset;

	return PARSERUTILS_OK;
}

/*
 * sfods-write.c : export Simplified Flat Open Document Spreadsheet files
 *
 * sfods carries values, types, formulas and named ranges and nothing else, so
 * everything a workbook holds beyond that -- styling, column widths, merges,
 * comments, charts, and so on -- is dropped here.  See
 * https://github.com/fwilhe2/sfods for the format.
 *
 * Copyright (C) 2026 Florian Wilhelm
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301
 * USA
 */

#include <gnumeric-config.h>
#include <gnumeric.h>
#include "openoffice.h"
#include "sfods.h"

#include <gnm-plugin.h>
#include <workbook-view.h>
#include <workbook.h>
#include <sheet.h>
#include <cell.h>
#include <value.h>
#include <ranges.h>
#include <expr.h>
#include <expr-name.h>
#include <parse-util.h>
#include <gutils.h>

#include <goffice/goffice.h>

#include <gsf/gsf-output.h>
#include <gsf/gsf-utils.h>

#include <glib/gi18n-lib.h>
#include <string.h>

typedef struct {
	GOIOContext	*context;
	GsfOutput	*out;
	WorkbookView const *wbv;
	Workbook const	*wb;
	GnmConventions	*conv;
	GODateConventions const *date_conv;
} SFODSWriteState;

/* ------------------------------------------------------------------------- */

static void
sfods_attr (SFODSWriteState *state, char const *name, char const *value)
{
	char *escaped = g_markup_escape_text (value, -1);
	gsf_output_printf (state->out, " %s=\"%s\"", name, escaped);
	g_free (escaped);
}

/*
 * The format keeps its text in a CDATA section, which can hold anything but
 * the sequence that ends it.
 */
static void
sfods_cdata (SFODSWriteState *state, char const *text)
{
	char const *p = text, *hit;

	gsf_output_puts (state->out, "<![CDATA[");
	while (NULL != (hit = strstr (p, "]]>"))) {
		gsf_output_write (state->out, hit - p + 2, (guint8 const *)p);
		gsf_output_puts (state->out, "]]><![CDATA[>");
		p = hit + 3;
	}
	gsf_output_puts (state->out, p);
	gsf_output_puts (state->out, "]]>");
}

/* ------------------------------------------------------------------------- */

/**
 * sfods_format_currency_code:
 * @fmt: a number format
 *
 * sfods insists on an ISO 4217 code, but a Gnumeric currency format carries
 * whatever symbol the user picked.  goffice spells most of its currencies as
 * the code already ("[$EUR]"), which needs only unwrapping; the few it spells
 * as a glyph ("$", "[$€-1]") are mapped here.  Anything else has no code, and
 * the caller has to write the cell as a plain float.
 *
 * Returns: (transfer full) (nullable): the ISO 4217 code, or %NULL.
 **/
char *
sfods_format_currency_code (GOFormat const *fmt)
{
	static struct {
		char const *symbol;
		char const *code;
	} const glyphs[] = {
		{ "€",  "EUR" },
		{ "$",  "USD" },
		{ "£",  "GBP" },
		{ "¥",  "JPY" },
		{ "₹",  "INR" },
		{ "₽",  "RUB" },
		{ "₩",  "KRW" },
		{ "₪",  "ILS" },
		{ "₴",  "UAH" },
		{ "₺",  "TRY" }
	};
	GOFormatDetails details;
	char const *symbol = NULL;
	char *inner = NULL;
	char *code = NULL;
	unsigned i;

	if (fmt == NULL)
		return NULL;

	go_format_details_init (&details, GO_FORMAT_UNKNOWN);
	go_format_get_details (fmt, &details, NULL);
	if (details.currency != NULL)
		symbol = details.currency->symbol;

	if (symbol == NULL) {
		/* goffice only fills in the currency for symbols it has in
		 * its own table, so a code it does not carry has to be read
		 * back out of the format itself. */
		char const *xl = go_format_as_XL (fmt);
		char const *open = xl ? strstr (xl, "[$") : NULL;

		if (open != NULL) {
			char const *p = open + 2;
			inner = g_strndup (p, strcspn (p, "-]"));
			symbol = inner;
		}
	} else if (symbol[0] == '[' && symbol[1] == '$') {
		/* "[$EUR]", "[$EUR-407]" and "[$€-1]" all reduce to what sits
		 * between the "[$" and the "]" or "-". */
		char const *p = symbol + 2;
		inner = g_strndup (p, strcspn (p, "-]"));
		symbol = inner;
	}

	if (symbol != NULL) {
		if (strlen (symbol) == 3 &&
		    g_ascii_isupper (symbol[0]) &&
		    g_ascii_isupper (symbol[1]) &&
		    g_ascii_isupper (symbol[2]))
			code = g_strdup (symbol);
		else
			for (i = 0; code == NULL && i < G_N_ELEMENTS (glyphs); i++)
				if (0 == strcmp (symbol, glyphs[i].symbol))
					code = g_strdup (glyphs[i].code);
	}

	g_free (inner);
	go_format_details_finalize (&details);
	return code;
}

/*
 * Decides how a cell presents itself in sfods terms.  On return @currency
 * holds the ISO code when the answer is SFODS_TYPE_CURRENCY.
 */
static SFODSValueType
sfods_cell_type (GnmCell const *cell, char **currency)
{
	GOFormat const *fmt;

	*currency = NULL;

	if (cell->value == NULL)
		return SFODS_TYPE_NONE;

	switch (cell->value->v_any.type) {
	case VALUE_BOOLEAN:
		return SFODS_TYPE_BOOLEAN;

	case VALUE_STRING:
	case VALUE_ERROR:
		/* sfods has no error type; an error reads as its text. */
		return SFODS_TYPE_STRING;

	case VALUE_FLOAT:
		fmt = gnm_cell_get_format (cell);
		if (go_format_is_date (fmt) > 0)
			return SFODS_TYPE_DATE;
		if (go_format_is_time (fmt) > 0)
			return SFODS_TYPE_TIME;
		/* go_format_get_family reports a currency format as a plain
		 * number, so ask for the code and let the answer decide.  No
		 * code means the cell cannot be written as a currency. */
		*currency = sfods_format_currency_code (fmt);
		if (*currency != NULL)
			return SFODS_TYPE_CURRENCY;
		if (go_format_get_family (fmt) == GO_FORMAT_PERCENTAGE)
			return SFODS_TYPE_PERCENTAGE;
		return SFODS_TYPE_FLOAT;

	case VALUE_EMPTY:
	case VALUE_CELLRANGE:
	case VALUE_ARRAY:
	default:
		return SFODS_TYPE_NONE;
	}
}

/*
 * The spec fixes how each type spells its value, independently of any locale.
 * Returns NULL for a string cell, whose content lives in <text> instead.
 */
static char *
sfods_value_as_string (SFODSWriteState *state, GnmCell const *cell,
		       SFODSValueType type)
{
	gnm_float f;

	switch (type) {
	case SFODS_TYPE_BOOLEAN:
		return g_strdup (value_get_as_bool (cell->value, NULL)
				 ? "true" : "false");

	case SFODS_TYPE_DATE: {
		GDate date;
		gnm_float frac;
		int serial;

		f = value_get_as_float (cell->value);
		serial = (int)gnm_floor (f);
		frac = f - serial;
		go_date_serial_to_g (&date, serial, state->date_conv);
		if (!g_date_valid (&date))
			return NULL;

		if (frac > 0) {
			/* A date carrying a time of day.  The spec spells a
			 * date "YYYY-MM-DD"; ODF's "T" form is the only way
			 * to keep the time, so use it when there is one. */
			int secs = (int)gnm_fake_round (frac * 86400);
			return g_strdup_printf
				("%04u-%02u-%02uT%02d:%02d:%02d",
				 g_date_get_year (&date),
				 g_date_get_month (&date),
				 g_date_get_day (&date),
				 secs / 3600, (secs / 60) % 60, secs % 60);
		}
		return g_strdup_printf ("%04u-%02u-%02u",
					g_date_get_year (&date),
					g_date_get_month (&date),
					g_date_get_day (&date));
	}

	case SFODS_TYPE_TIME: {
		int secs;

		f = value_get_as_float (cell->value);
		if (f < 0)
			return NULL;
		secs = (int)gnm_fake_round (f * 86400);
		return g_strdup_printf ("%02d:%02d:%02d",
					secs / 3600, (secs / 60) % 60,
					secs % 60);
	}

	case SFODS_TYPE_FLOAT:
	case SFODS_TYPE_PERCENTAGE:
	case SFODS_TYPE_CURRENCY: {
		GString *str = g_string_new (NULL);
		value_get_as_gstring (cell->value, str, state->conv);
		return g_string_free (str, FALSE);
	}

	case SFODS_TYPE_STRING:
	case SFODS_TYPE_NONE:
	default:
		return NULL;
	}
}

static char const *
sfods_type_name (SFODSValueType type)
{
	switch (type) {
	case SFODS_TYPE_STRING:		return "string";
	case SFODS_TYPE_FLOAT:		return "float";
	case SFODS_TYPE_CURRENCY:	return "currency";
	case SFODS_TYPE_PERCENTAGE:	return "percentage";
	case SFODS_TYPE_DATE:		return "date";
	case SFODS_TYPE_TIME:		return "time";
	case SFODS_TYPE_BOOLEAN:	return "boolean";
	case SFODS_TYPE_NONE:
	default:			return NULL;
	}
}

static char *
sfods_cell_formula (SFODSWriteState *state, GnmCell const *cell)
{
	GnmParsePos pp;
	char *formula, *prefixed;

	if (!gnm_cell_has_expr (cell))
		return NULL;
	/* Only the corner of an array carries the expression. */
	if (gnm_expr_top_is_array_elem (cell->base.texpr, NULL, NULL))
		return NULL;

	parse_pos_init_cell (&pp, cell);
	formula = gnm_expr_top_as_string (cell->base.texpr, &pp, state->conv);
	prefixed = g_strconcat ("of:=", formula, NULL);
	g_free (formula);
	return prefixed;
}

/* ------------------------------------------------------------------------- */

static void
sfods_write_cell (SFODSWriteState *state, GnmCell *cell, int row, int col)
{
	SFODSValueType type = SFODS_TYPE_NONE;
	char *currency = NULL, *value = NULL, *formula = NULL, *text = NULL;
	char const *type_name;
	char *pos;

	if (cell != NULL) {
		type = sfods_cell_type (cell, &currency);
		formula = sfods_cell_formula (state, cell);
		if (type == SFODS_TYPE_STRING)
			text = gnm_cell_get_rendered_text (cell);
		else
			value = sfods_value_as_string (state, cell, type);
	}

	if (value == NULL && text == NULL && formula == NULL) {
		/* There is no gap syntax; an empty cell holds the place. */
		gsf_output_puts (state->out, "      <cell />\n");
		g_free (currency);
		return;
	}

	gsf_output_puts (state->out, "      <cell");

	/* The attribute order the format documents: R C value formula type
	 * currency. */
	pos = g_strdup_printf ("%d", row + 1);
	sfods_attr (state, "R", pos);
	g_free (pos);
	pos = g_strdup_printf ("%d", col + 1);
	sfods_attr (state, "C", pos);
	g_free (pos);

	if (value != NULL)
		sfods_attr (state, "value", value);
	if (formula != NULL)
		sfods_attr (state, "formula", formula);
	if (NULL != (type_name = sfods_type_name (type)))
		sfods_attr (state, "type", type_name);
	if (currency != NULL)
		sfods_attr (state, "currency", currency);

	/* The two cell shapes are exclusive: text lives in a child, a value
	 * in an attribute on a self-closing element. */
	if (text != NULL) {
		gsf_output_puts (state->out, "> <text>");
		sfods_cdata (state, text);
		gsf_output_puts (state->out, "</text> </cell>\n");
	} else
		gsf_output_puts (state->out, " />\n");

	g_free (currency);
	g_free (value);
	g_free (formula);
	g_free (text);
}

/* ------------------------------------------------------------------------- */

static void
sfods_collect_name (G_GNUC_UNUSED gpointer key, GnmNamedExpr *nexpr,
		    GSList **names)
{
	if (!expr_name_is_active (nexpr) ||
	    expr_name_is_placeholder (nexpr) ||
	    nexpr->texpr == NULL)
		return;

	/* sfods can only express named *ranges*. */
	if (!gnm_expr_top_is_rangeref (nexpr->texpr))
		return;

	*names = g_slist_prepend (*names, nexpr);
}

static void
sfods_write_named_range (SFODSWriteState *state, GnmNamedExpr *nexpr,
			 char const *indent)
{
	Sheet *sheet = nexpr->pos.sheet;
	GnmExprTop const *texpr;
	GnmCellRef ref;
	char *formula;

	if (sheet == NULL)
		sheet = workbook_sheet_by_index (state->wb, 0);
	if (sheet == NULL)
		return;

	gsf_output_printf (state->out, "%s  <named-range", indent);
	sfods_attr (state, "name", expr_name_name (nexpr));

	gnm_cellref_init (&ref, sheet, nexpr->pos.eval.col,
			  nexpr->pos.eval.row, FALSE);
	texpr = gnm_expr_top_new (gnm_expr_new_cellref (&ref));
	formula = gnm_expr_top_as_string (texpr, &nexpr->pos, state->conv);
	sfods_attr (state, "base-cell-address", odf_strip_brackets (formula));
	g_free (formula);
	gnm_expr_top_unref (texpr);

	formula = gnm_expr_top_as_string (nexpr->texpr, &nexpr->pos,
					  state->conv);
	sfods_attr (state, "cell-range-address", odf_strip_brackets (formula));
	g_free (formula);

	gsf_output_puts (state->out, " />\n");
}

/*
 * Empty <named-expressions> elements are not emitted: they reparse as an
 * empty string rather than as an empty collection.
 */
static void
sfods_write_named_expressions (SFODSWriteState *state, GSList *names,
			       char const *indent)
{
	GSList *l;

	if (names == NULL)
		return;

	gsf_output_printf (state->out, "%s<named-expressions>\n", indent);
	for (l = names; l != NULL; l = l->next)
		sfods_write_named_range (state, l->data, indent);
	gsf_output_printf (state->out, "%s</named-expressions>\n", indent);
}

/* ------------------------------------------------------------------------- */

static void
sfods_write_sheet (SFODSWriteState *state, Sheet *sheet)
{
	GnmRange extent;
	GSList *names = NULL;
	int row, col;

	gsf_output_puts (state->out, "  <table");
	sfods_attr (state, "name", sheet->name_unquoted);
	gsf_output_puts (state->out, ">\n");

	/* On an empty sheet the extent comes back inverted. */
	extent = sheet_get_cells_extent (sheet);
	if (extent.start.col <= extent.end.col &&
	    extent.start.row <= extent.end.row)
		for (row = 0; row <= extent.end.row; row++) {
			gsf_output_puts (state->out, "    <row>\n");
			for (col = 0; col <= extent.end.col; col++)
				sfods_write_cell (state,
						  sheet_cell_get (sheet, col, row),
						  row, col);
			gsf_output_puts (state->out, "    </row>\n");
		}

	gnm_sheet_foreach_name (sheet, (GHFunc)sfods_collect_name, &names);
	names = g_slist_reverse (names);
	sfods_write_named_expressions (state, names, "    ");
	g_slist_free (names);

	gsf_output_puts (state->out, "  </table>\n");
}

G_MODULE_EXPORT void
sfods_file_save (GOFileSaver const *fs, GOIOContext *io_context,
		 WorkbookView const *wbv, GsfOutput *output);

void
sfods_file_save (G_GNUC_UNUSED GOFileSaver const *fs, GOIOContext *io_context,
		 WorkbookView const *wbv, GsfOutput *output)
{
	SFODSWriteState state;
	GnmLocale *locale;
	GSList *names = NULL;
	int i, n;

	state.context	= io_context;
	state.out	= output;
	state.wbv	= wbv;
	state.wb	= wb_view_get_workbook (wbv);
	state.conv	= odf_expr_conventions_new ();
	state.date_conv	= workbook_date_conv (state.wb);

	/* ODS spells floats out to the last bit it can; a format meant to be
	 * hand-written and diffed wants the shortest exact spelling. */
	state.conv->output.decimal_digits = -1;

	locale = gnm_push_C_locale ();

	gsf_output_puts (output, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	gsf_output_puts (output, "<spreadsheet>\n");

	n = workbook_sheet_count (state.wb);
	for (i = 0; i < n; i++)
		sfods_write_sheet (&state,
				   workbook_sheet_by_index (state.wb, i));

	/* Document scope comes last. */
	workbook_foreach_name (state.wb, TRUE, (GHFunc)sfods_collect_name,
			       &names);
	names = g_slist_reverse (names);
	sfods_write_named_expressions (&state, names, "  ");
	g_slist_free (names);

	gsf_output_puts (output, "</spreadsheet>\n");

	gnm_pop_C_locale (locale);
	g_object_unref (state.conv);
}

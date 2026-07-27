/*
 * sfods-read.c : import Simplified Flat Open Document Spreadsheet files
 *
 * sfods is a small subset of ODF: values, types, formulas and named ranges,
 * and nothing else.  See https://github.com/fwilhe2/sfods for the format.
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
#include <expr.h>
#include <expr-name.h>
#include <parse-util.h>
#include <mstyle.h>
#include <sheet-style.h>
#include <gutils.h>
#include <command-context.h>

#include <goffice/goffice.h>

#include <gsf/gsf-libxml.h>
#include <gsf/gsf-input.h>
#include <gsf/gsf-utils.h>

#include <glib/gi18n-lib.h>
#include <string.h>
#include <stdlib.h>

#define CXML2C(s) ((char const *)(s))

/*
 * A cell's formula is parsed only once the whole file has been read, because
 * the named ranges it may refer to are written after the cells that use them.
 * Same for the named ranges themselves: their addresses may name a table that
 * has not been reached yet.
 */
typedef struct {
	Sheet *sheet;
	int col, row;
	char *formula;
} SFODSFormula;

typedef struct {
	Sheet *scope;		/* NULL for a document-wide name */
	char *name;
	char *base;		/* baseCellAddress, may be NULL */
	char *range;		/* cellRangeAddress */
} SFODSName;

typedef struct {
	GOIOContext	*context;
	WorkbookView	*wbv;
	Workbook	*wb;
	Sheet		*sheet;		/* table being read */
	GnmConventions	*convs;

	int		 row;		/* 0-based, within the table */
	int		 col;		/* 0-based, within the row */

	/* the cell currently being read */
	struct {
		SFODSValueType type;
		char	*value;
		char	*formula;
		char	*currency;
		GString	*text;
		gboolean has_text;
	} cell;

	GSList		*formulas;	/* SFODSFormula, reversed */
	GSList		*names;		/* SFODSName, reversed */

	int		 warnings;
} SFODSReadState;

#define SFODS_MAX_WARNINGS 20

static void
sfods_warning (SFODSReadState *state, char const *fmt, ...)
{
	char *msg;
	va_list args;

	if (state->warnings++ > SFODS_MAX_WARNINGS)
		return;

	va_start (args, fmt);
	msg = g_strdup_vprintf (fmt, args);
	va_end (args);

	go_io_warning (state->context, "%s", msg);
	g_free (msg);
}

/* Rule 4 of the spec: throughout, the empty string counts as absent. */
static char *
sfods_attr_dup (xmlChar const *v)
{
	char const *s = CXML2C (v);
	return (s == NULL || *s == '\0') ? NULL : g_strdup (s);
}

/**
 * sfods_value_type_from_name:
 * @name: the value of a cell's @type attribute
 *
 * Returns: the matching #SFODSValueType, or %SFODS_TYPE_NONE if @name is not
 * one of the seven the format defines.
 **/
SFODSValueType
sfods_value_type_from_name (char const *name)
{
	static struct {
		char const *name;
		SFODSValueType type;
	} const types[] = {
		{ "string",	SFODS_TYPE_STRING },
		{ "float",	SFODS_TYPE_FLOAT },
		{ "currency",	SFODS_TYPE_CURRENCY },
		{ "percentage",	SFODS_TYPE_PERCENTAGE },
		{ "date",	SFODS_TYPE_DATE },
		{ "time",	SFODS_TYPE_TIME },
		{ "boolean",	SFODS_TYPE_BOOLEAN }
	};
	unsigned i;

	if (name == NULL)
		return SFODS_TYPE_NONE;

	for (i = 0; i < G_N_ELEMENTS (types); i++)
		if (0 == strcmp (name, types[i].name))
			return types[i].type;

	return SFODS_TYPE_NONE;
}

/**
 * sfods_currency_format:
 * @code: an ISO 4217 code
 *
 * Returns: (transfer full): a number format for @code.  Built by goffice
 * rather than spelled out here, because only the layouts goffice generates
 * are ones it recognizes as currency again -- which is what
 * #sfods_format_currency_code needs on the way back out.
 **/
GOFormat *
sfods_currency_format (char const *code)
{
	GOFormatCurrency const *c;
	GOFormatCurrency unlisted;
	GOFormatDetails details;
	GString *xl;
	GOFormat *fmt;
	char *symbol = g_strdup_printf ("[$%s]", code);

	for (c = _go_format_currencies (); c != NULL && c->symbol != NULL; c++)
		if (0 == strcmp (c->symbol, symbol))
			break;

	if (c == NULL || c->symbol == NULL) {
		/* Not a code goffice knows.  The format still has to say so,
		 * or the value comes back out as a plain float. */
		unlisted.symbol	     = symbol;
		unlisted.description = NULL;
		unlisted.precedes    = TRUE;
		unlisted.has_space   = TRUE;
		c = &unlisted;
	}

	go_format_details_init (&details, GO_FORMAT_CURRENCY);
	details.currency      = c;
	details.num_decimals  = 2;
	details.thousands_sep = TRUE;

	xl = g_string_new (NULL);
	go_format_generate_str (xl, &details);
	fmt = go_format_new_from_XL (xl->str);

	g_string_free (xl, TRUE);
	go_format_details_finalize (&details);
	g_free (symbol);

	return fmt;
}

/*
 * The spec spells values in a fixed way that does not depend on the locale:
 * decimal numbers with a '.', dates as YYYY-MM-DD, times as hh:mm:ss and
 * booleans as true/false.  The whole import runs in the C locale, so
 * go_strtod and friends do the right thing.
 */
static GnmValue *
sfods_parse_value (SFODSReadState *state, char const *str, GOFormat **fmt)
{
	*fmt = NULL;

	switch (state->cell.type) {
	case SFODS_TYPE_STRING:
		return value_new_string (str);

	case SFODS_TYPE_BOOLEAN:
		if (0 == g_ascii_strcasecmp (str, "true"))
			return value_new_bool (TRUE);
		if (0 == g_ascii_strcasecmp (str, "false"))
			return value_new_bool (FALSE);
		sfods_warning (state, _("Invalid boolean value '%s'"), str);
		return NULL;

	case SFODS_TYPE_DATE: {
		unsigned y, m, d, h = 0, mi = 0;
		gnm_float s = 0;
		GDate date;
		int serial;
		int n = gnm_sscanf (str, "%u-%u-%uT%u:%u:%" GNM_SCANF_g,
				    &y, &m, &d, &h, &mi, &s);

		/* g_date_set_dmy asserts, so the check has to come first. */
		if (n < 3 || !g_date_valid_dmy (d, m, y)) {
			sfods_warning (state, _("Invalid date value '%s'"), str);
			return NULL;
		}
		g_date_set_dmy (&date, d, m, y);
		serial = go_date_g_to_serial (&date, workbook_date_conv (state->wb));
		if (n >= 6) {
			gnm_float frac = (h + mi / (gnm_float)60 +
					  s / (gnm_float)3600) / 24;
			*fmt = go_format_ref (go_format_default_date_time ());
			return value_new_float (serial + frac);
		}
		*fmt = go_format_ref (go_format_default_date ());
		return value_new_int (serial);
	}

	case SFODS_TYPE_TIME: {
		unsigned h, mi;
		gnm_float s = 0;
		int n = gnm_sscanf (str, "%u:%u:%" GNM_SCANF_g, &h, &mi, &s);

		if (n < 2) {
			sfods_warning (state, _("Invalid time value '%s'"), str);
			return NULL;
		}
		*fmt = go_format_ref (go_format_default_time ());
		return value_new_float ((h * 3600 + mi * 60 + s) / (gnm_float)86400);
	}

	case SFODS_TYPE_PERCENTAGE:
	case SFODS_TYPE_CURRENCY:
	case SFODS_TYPE_FLOAT:
	case SFODS_TYPE_NONE:
	default: {
		char *end;
		gnm_float f = gnm_strto (str, &end);

		if (end == str || *end != '\0') {
			/* Not a number.  The spec does not allow this, but a
			 * string is a better guess than dropping the cell. */
			sfods_warning (state, _("Invalid numeric value '%s'"), str);
			return value_new_string (str);
		}
		if (state->cell.type == SFODS_TYPE_PERCENTAGE)
			*fmt = go_format_ref (go_format_default_percentage ());
		else if (state->cell.type == SFODS_TYPE_CURRENCY &&
			 state->cell.currency != NULL)
			*fmt = sfods_currency_format (state->cell.currency);
		return value_new_float (f);
	}
	}
}

/* Grow the sheet if the file addresses a cell outside it. */
static gboolean
sfods_sheet_accommodate (SFODSReadState *state, int col, int row)
{
	GnmSheetSize const *ss = gnm_sheet_get_size (state->sheet);
	int cols, rows;
	GOUndo *undo;
	gboolean err;

	if (col < ss->max_cols && row < ss->max_rows)
		return TRUE;

	cols = MAX (col + 1, ss->max_cols);
	rows = MAX (row + 1, ss->max_rows);
	gnm_sheet_suggest_size (&cols, &rows);
	undo = gnm_sheet_resize (state->sheet, cols, rows, NULL, &err);
	if (undo)
		g_object_unref (undo);

	ss = gnm_sheet_get_size (state->sheet);
	return col < ss->max_cols && row < ss->max_rows;
}

/* ------------------------------------------------------------------------- */

static void
sfods_table_start (GsfXMLIn *xin, xmlChar const **attrs)
{
	SFODSReadState *state = xin->user_state;
	char *name = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1]; attrs += 2)
		if (0 == strcmp (CXML2C (attrs[0]), "name"))
			name = sfods_attr_dup (attrs[1]);

	if (name == NULL)
		name = workbook_sheet_get_free_name (state->wb, _("Sheet"),
						     TRUE, FALSE);
	else if (workbook_sheet_by_name (state->wb, name) != NULL) {
		char *unique = workbook_sheet_get_free_name (state->wb, name,
							     FALSE, FALSE);
		sfods_warning (state, _("Duplicate table name \"%s\", "
					"renamed to \"%s\"."), name, unique);
		g_free (name);
		name = unique;
	}

	state->sheet = sheet_new (state->wb, name, GNM_DEFAULT_COLS,
				  GNM_DEFAULT_ROWS);
	workbook_sheet_attach (state->wb, state->sheet);
	g_free (name);

	state->row = -1;
}

static void
sfods_table_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	SFODSReadState *state = xin->user_state;
	state->sheet = NULL;
}

static void
sfods_row_start (GsfXMLIn *xin, G_GNUC_UNUSED xmlChar const **attrs)
{
	SFODSReadState *state = xin->user_state;

	state->row++;
	state->col = -1;
}

static void
sfods_cell_start (GsfXMLIn *xin, xmlChar const **attrs)
{
	SFODSReadState *state = xin->user_state;
	char *type = NULL;

	state->col++;

	/* R and C are informational; position is what places a cell. */
	for (; attrs != NULL && attrs[0] && attrs[1]; attrs += 2) {
		char const *a = CXML2C (attrs[0]);

		if (0 == strcmp (a, "value"))
			state->cell.value = sfods_attr_dup (attrs[1]);
		else if (0 == strcmp (a, "formula"))
			state->cell.formula = sfods_attr_dup (attrs[1]);
		else if (0 == strcmp (a, "type"))
			type = sfods_attr_dup (attrs[1]);
		else if (0 == strcmp (a, "currency"))
			state->cell.currency = sfods_attr_dup (attrs[1]);
	}

	if (type != NULL) {
		state->cell.type = sfods_value_type_from_name (type);
		if (state->cell.type == SFODS_TYPE_NONE)
			sfods_warning (state, _("Unknown cell type '%s'"), type);
		g_free (type);
	} else
		state->cell.type = SFODS_TYPE_NONE;

	state->cell.has_text = FALSE;
	if (state->cell.text != NULL)
		g_string_truncate (state->cell.text, 0);
}

static void
sfods_cell_text_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	SFODSReadState *state = xin->user_state;

	if (state->cell.text == NULL)
		state->cell.text = g_string_new (NULL);
	g_string_assign (state->cell.text, xin->content->str);
	state->cell.has_text = (xin->content->len > 0);
}

static void
sfods_cell_end (GsfXMLIn *xin, G_GNUC_UNUSED GsfXMLBlob *blob)
{
	SFODSReadState *state = xin->user_state;
	GnmValue *v = NULL;
	GOFormat *fmt = NULL;
	char const *content = NULL;

	if (state->sheet == NULL)
		goto out;

	/*
	 * "A cell with no value, text or formula is empty, whatever type
	 * says."  value wins over text where both are present, and for a
	 * string cell the text *is* the content.
	 */
	if (state->cell.value != NULL)
		content = state->cell.value;
	else if (state->cell.has_text) {
		content = state->cell.text->str;
		/* Text without a value on a typed cell: text is a rendering
		 * in some locale, not something to parse back into a number,
		 * so all that can be kept is the text itself. */
		state->cell.type = SFODS_TYPE_STRING;
	}

	if (content == NULL && state->cell.formula == NULL)
		goto out;

	if (!sfods_sheet_accommodate (state, state->col, state->row)) {
		sfods_warning (state, _("Ignoring cell outside the maximum "
					"sheet size."));
		goto out;
	}

	if (content != NULL) {
		v = sfods_parse_value (state, content, &fmt);
		if (fmt != NULL) {
			GnmStyle *style = gnm_style_new ();
			gnm_style_set_format (style, fmt);
			sheet_style_apply_pos (state->sheet, state->col,
					       state->row, style);
			go_format_unref (fmt);
		}
	}

	if (v != NULL)
		gnm_cell_set_value (sheet_cell_fetch (state->sheet, state->col,
						      state->row), v);

	if (state->cell.formula != NULL) {
		SFODSFormula *f = g_new (SFODSFormula, 1);
		f->sheet   = state->sheet;
		f->col     = state->col;
		f->row     = state->row;
		f->formula = state->cell.formula;
		state->cell.formula = NULL;
		state->formulas = g_slist_prepend (state->formulas, f);
	}

 out:
	g_free (state->cell.value);
	g_free (state->cell.formula);
	g_free (state->cell.currency);
	state->cell.value = state->cell.formula = state->cell.currency = NULL;
	state->cell.has_text = FALSE;
}

static void
sfods_named_range (GsfXMLIn *xin, xmlChar const **attrs)
{
	SFODSReadState *state = xin->user_state;
	SFODSName *n;
	char *name = NULL, *base = NULL, *range = NULL;

	for (; attrs != NULL && attrs[0] && attrs[1]; attrs += 2) {
		char const *a = CXML2C (attrs[0]);

		if (0 == strcmp (a, "name"))
			name = sfods_attr_dup (attrs[1]);
		else if (0 == strcmp (a, "base-cell-address"))
			base = sfods_attr_dup (attrs[1]);
		else if (0 == strcmp (a, "cell-range-address"))
			range = sfods_attr_dup (attrs[1]);
	}

	if (name == NULL || range == NULL) {
		sfods_warning (state, _("Ignoring a named range without a "
					"name or a cell range address."));
		g_free (name);
		g_free (base);
		g_free (range);
		return;
	}

	n = g_new (SFODSName, 1);
	n->scope = state->sheet;	/* NULL outside a <table> */
	n->name  = name;
	n->base  = base;
	n->range = range;
	state->names = g_slist_prepend (state->names, n);
}

/* ------------------------------------------------------------------------- */

/*
 * The addresses are ODF's, so they parse as OpenFormula once wrapped in the
 * brackets that a reference carries there: "$Values.$A$1" -> "[$Values.$A$1]".
 */
static GnmExprTop const *
sfods_parse_address (SFODSReadState *state, char const *address,
		     GnmParsePos const *pp)
{
	GnmExprTop const *texpr;
	char *bracketed = g_strconcat ("[", address, "]", NULL);

	texpr = gnm_expr_parse_str (bracketed, pp, GNM_EXPR_PARSE_DEFAULT,
				    state->convs, NULL);
	g_free (bracketed);
	return texpr;
}

static void
sfods_add_name (SFODSReadState *state, SFODSName *n)
{
	GnmParsePos pp;
	GnmExprTop const *texpr;
	char *error = NULL;

	parse_pos_init (&pp, state->wb, NULL, 0, 0);

	/* The base cell address only sets the position names are relative
	 * to; it does not have to resolve. */
	if (n->base != NULL) {
		GnmExprTop const *btexpr = sfods_parse_address (state, n->base, &pp);
		GnmCellRef const *ref;

		if (btexpr != NULL) {
			if (NULL != (ref = gnm_expr_top_get_cellref (btexpr)))
				parse_pos_init (&pp, state->wb, ref->sheet,
						ref->col, ref->row);
			else
				sfods_warning (state,
					       _("The base cell address of "
						 "'%s' is not a cell reference: "
						 "'%s'"), n->name, n->base);
			gnm_expr_top_unref (btexpr);
		} else
			sfods_warning (state, _("Unable to parse the base cell "
						"address of '%s': '%s'"),
				       n->name, n->base);
	}

	texpr = sfods_parse_address (state, n->range, &pp);
	if (texpr == NULL) {
		sfods_warning (state, _("Unable to parse the cell range "
					"address of '%s': '%s'"),
			       n->name, n->range);
		return;
	}

	pp.sheet = n->scope;
	if (NULL == expr_name_add (&pp, n->name, texpr, &error, NULL)) {
		sfods_warning (state, _("Unable to define the name '%s': %s"),
			       n->name, error ? error : "");
		g_free (error);
	}
}

static void
sfods_add_formula (SFODSReadState *state, SFODSFormula *f)
{
	GnmParsePos pp;
	GnmExprTop const *texpr;
	GnmParseError perr;
	GnmCell *cell;
	char const *expr = f->formula;

	/* "An OpenFormula expression including its of: prefix." */
	if (g_str_has_prefix (expr, "of:"))
		expr += 3;
	if (*expr == '=')
		expr++;
	if (*expr == '\0')
		return;

	parse_pos_init (&pp, state->wb, f->sheet, f->col, f->row);

	parse_error_init (&perr);
	texpr = gnm_expr_parse_str (expr, &pp, GNM_EXPR_PARSE_DEFAULT,
				    state->convs, &perr);
	if (texpr == NULL) {
		sfods_warning (state, _("Unable to parse '%s' (%s)"),
			       f->formula,
			       perr.err ? perr.err->message : "");
		parse_error_free (&perr);
		return;
	}
	parse_error_free (&perr);

	cell = sheet_cell_fetch (f->sheet, f->col, f->row);
	if (cell->value != NULL && !VALUE_IS_EMPTY (cell->value))
		/* Keep the value the file recorded; a recalculation will
		 * replace it if it disagrees. */
		gnm_cell_set_expr_and_value (cell, texpr,
					     value_dup (cell->value), TRUE);
	else
		gnm_cell_set_expr (cell, texpr);

	gnm_expr_top_unref (texpr);
}

static void
sfods_formula_free (gpointer data)
{
	SFODSFormula *f = data;
	g_free (f->formula);
	g_free (f);
}

static void
sfods_name_free (gpointer data)
{
	SFODSName *n = data;
	g_free (n->name);
	g_free (n->base);
	g_free (n->range);
	g_free (n);
}

/* ------------------------------------------------------------------------- */

static GsfXMLInNode const sfods_dtd[] = {
GSF_XML_IN_NODE (START, START, -1, NULL, GSF_XML_NO_CONTENT, NULL, NULL),
GSF_XML_IN_NODE (START, SPREADSHEET, -1, "spreadsheet", GSF_XML_NO_CONTENT, NULL, NULL),
  GSF_XML_IN_NODE (SPREADSHEET, TABLE, -1, "table", GSF_XML_NO_CONTENT, &sfods_table_start, &sfods_table_end),
    GSF_XML_IN_NODE (TABLE, ROW, -1, "row", GSF_XML_NO_CONTENT, &sfods_row_start, NULL),
      GSF_XML_IN_NODE (ROW, CELL, -1, "cell", GSF_XML_NO_CONTENT, &sfods_cell_start, &sfods_cell_end),
        GSF_XML_IN_NODE (CELL, CELL_TEXT, -1, "text", GSF_XML_CONTENT, NULL, &sfods_cell_text_end),
    GSF_XML_IN_NODE (TABLE, TABLE_NAMES, -1, "named-expressions", GSF_XML_NO_CONTENT, NULL, NULL),
      GSF_XML_IN_NODE (TABLE_NAMES, TABLE_NAME, -1, "named-range", GSF_XML_NO_CONTENT, &sfods_named_range, NULL),
  GSF_XML_IN_NODE (SPREADSHEET, DOC_NAMES, -1, "named-expressions", GSF_XML_NO_CONTENT, NULL, NULL),
    GSF_XML_IN_NODE (DOC_NAMES, DOC_NAME, -1, "named-range", GSF_XML_NO_CONTENT, &sfods_named_range, NULL),
GSF_XML_IN_NODE_END
};

/* ------------------------------------------------------------------------- */

static gboolean
sfods_probe_element (const xmlChar *name,
		     G_GNUC_UNUSED const xmlChar *prefix,
		     const xmlChar *URI,
		     G_GNUC_UNUSED int nb_namespaces,
		     G_GNUC_UNUSED const xmlChar **namespaces,
		     G_GNUC_UNUSED int nb_attributes,
		     G_GNUC_UNUSED int nb_defaulted,
		     G_GNUC_UNUSED const xmlChar **attributes)
{
	/* <spreadsheet> in no namespace at all -- sfods has no schema and no
	 * version marker, so the root element is all there is to go on. */
	return NULL == URI && 0 == strcmp (CXML2C (name), "spreadsheet");
}

G_MODULE_EXPORT gboolean
sfods_file_probe (GOFileOpener const *fo, GsfInput *input, GOFileProbeLevel pl);

gboolean
sfods_file_probe (G_GNUC_UNUSED GOFileOpener const *fo, GsfInput *input,
		  GOFileProbeLevel pl)
{
	if (pl == GO_FILE_PROBE_FILE_NAME) {
		char const *name = gsf_input_name (input);

		/* Only the compound extension: a bare ".xml" belongs to the
		 * Gnumeric opener, and content probing catches the rest. */
		return name != NULL &&
			(g_str_has_suffix (name, ".sfods.xml") ||
			 g_str_has_suffix (name, ".SFODS.XML"));
	}

	return gsf_xml_probe (input, &sfods_probe_element);
}

G_MODULE_EXPORT void
sfods_file_open (GOFileOpener const *fo, GOIOContext *io_context,
		 WorkbookView *wbv, GsfInput *input);

void
sfods_file_open (G_GNUC_UNUSED GOFileOpener const *fo, GOIOContext *io_context,
		 WorkbookView *wbv, GsfInput *input)
{
	SFODSReadState state;
	GsfXMLInDoc *doc;
	GnmLocale *locale;
	GSList *l;

	memset (&state, 0, sizeof (state));
	state.context = io_context;
	state.wbv     = wbv;
	state.wb      = wb_view_get_workbook (wbv);
	state.convs   = odf_conventions_new ();
	state.row     = -1;
	state.col     = -1;

	locale = gnm_push_C_locale ();

	doc = gsf_xml_in_doc_new (sfods_dtd, NULL);
	if (!gsf_xml_in_doc_parse (doc, input, &state))
		go_io_error_string (io_context,
				    _("XML document not well formed!"));
	gsf_xml_in_doc_free (doc);

	/* Names first: the formulas are what refer to them. */
	state.names = g_slist_reverse (state.names);
	for (l = state.names; l != NULL; l = l->next)
		sfods_add_name (&state, l->data);

	state.formulas = g_slist_reverse (state.formulas);
	for (l = state.formulas; l != NULL; l = l->next)
		sfods_add_formula (&state, l->data);

	if (workbook_sheet_count (state.wb) == 0)
		workbook_sheet_add (state.wb, -1, GNM_DEFAULT_COLS,
				    GNM_DEFAULT_ROWS);

	gnm_pop_C_locale (locale);

	g_slist_free_full (state.names, sfods_name_free);
	g_slist_free_full (state.formulas, sfods_formula_free);
	if (state.cell.text != NULL)
		g_string_free (state.cell.text, TRUE);
	g_free (state.cell.value);
	g_free (state.cell.formula);
	g_free (state.cell.currency);
	g_object_unref (state.convs);
}

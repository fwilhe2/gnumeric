/*
 * sfods.h : bits shared between the sfods reader and writer
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
#ifndef GNM_PLUGIN_SFODS_H
#define GNM_PLUGIN_SFODS_H

#include <gnumeric.h>
#include <goffice/goffice.h>

G_BEGIN_DECLS

/* The seven types sfods defines, plus the absence of a type attribute. */
typedef enum {
	SFODS_TYPE_NONE,
	SFODS_TYPE_STRING,
	SFODS_TYPE_FLOAT,
	SFODS_TYPE_CURRENCY,
	SFODS_TYPE_PERCENTAGE,
	SFODS_TYPE_DATE,
	SFODS_TYPE_TIME,
	SFODS_TYPE_BOOLEAN
} SFODSValueType;

SFODSValueType	 sfods_value_type_from_name (char const *name);
GOFormat	*sfods_currency_format	    (char const *code);
char		*sfods_format_currency_code (GOFormat const *fmt);

G_END_DECLS

#endif /* GNM_PLUGIN_SFODS_H */

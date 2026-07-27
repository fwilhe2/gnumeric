/*
 * openoffice.h : bits of the ODF filter shared with the sfods filter
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
#ifndef GNM_PLUGIN_OPENOFFICE_H
#define GNM_PLUGIN_OPENOFFICE_H

#include <gnumeric.h>
#include <parse-util.h>

G_BEGIN_DECLS

/*
 * sfods stores its formulas as OpenFormula, so it wants the same expression
 * conventions -- and in particular the same function name mapping -- as the
 * ODF filter.  These are the ODF ones, usable without an import or export in
 * progress.
 */
GnmConventions *odf_conventions_new	 (void);	/* openoffice-read.c */
GnmConventions *odf_expr_conventions_new (void);	/* openoffice-write.c */

/* An ODF reference prints as "[$Sheet.$A$1]"; sfods wants it without the
 * brackets.  Clips the trailing ']' off @string and returns a pointer past
 * the leading '['. */
char *odf_strip_brackets (char *string);		/* openoffice-write.c */

G_END_DECLS

#endif /* GNM_PLUGIN_OPENOFFICE_H */

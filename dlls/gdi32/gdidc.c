/*
 * GDI Device Context functions
 *
 * Copyright 1993, 1994 Alexandre Julliard
 * Copyright 1997 Bertho A. Stultiens
 *           1999 Huw D M Davies
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "gdi_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(gdi);

static DC_ATTR *get_dc_attr( HDC hdc )
{
    WORD type = gdi_handle_type( hdc );
    DC_ATTR *dc_attr;
    if ((type & 0x1f) != NTGDI_OBJ_DC || !(dc_attr = get_gdi_client_ptr( hdc, 0 )))
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return NULL;
    }
    return dc_attr;
}

/***********************************************************************
 *		GetCurrentPositionEx (GDI32.@)
 */
BOOL WINAPI GetCurrentPositionEx( HDC hdc, POINT *point )
{
    DC_ATTR *dc_attr;
    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    *point = dc_attr->cur_pos;
    return TRUE;
}

/***********************************************************************
 *           LineTo    (GDI32.@)
 */
BOOL WINAPI LineTo( HDC hdc, INT x, INT y )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)\n", hdc, x, y );

    if (is_meta_dc( hdc )) return METADC_LineTo( hdc, x, y );
    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_LineTo( dc_attr, x, y )) return FALSE;
    return NtGdiLineTo( hdc, x, y );
}

/***********************************************************************
 *           MoveToEx    (GDI32.@)
 */
BOOL WINAPI MoveToEx( HDC hdc, INT x, INT y, POINT *pt )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d), %p\n", hdc, x, y, pt );

    if (is_meta_dc( hdc )) return METADC_MoveTo( hdc, x, y );
    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_MoveTo( dc_attr, x, y )) return FALSE;
    return NtGdiMoveTo( hdc, x, y, pt );
}

/***********************************************************************
 *           Arc    (GDI32.@)
 */
BOOL WINAPI Arc( HDC hdc, INT left, INT top, INT right, INT bottom,
                 INT xstart, INT ystart, INT xend, INT yend )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)-(%d, %d), (%d, %d), (%d, %d)\n", hdc, left, top,
           right, bottom, xstart, ystart, xend, yend );

    if (is_meta_dc( hdc ))
        return METADC_Arc( hdc, left, top, right, bottom,
                           xstart, ystart, xend, yend );

    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_ArcChordPie( dc_attr, left, top, right, bottom,
                                            xstart, ystart, xend, yend, EMR_ARC ))
        return FALSE;

    return NtGdiArcInternal( NtGdiArc, hdc, left, top, right, bottom,
                             xstart, ystart, xend, yend );
}

/***********************************************************************
 *           ArcTo    (GDI32.@)
 */
BOOL WINAPI ArcTo( HDC hdc, INT left, INT top, INT right, INT bottom,
                   INT xstart, INT ystart, INT xend, INT yend )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)-(%d, %d), (%d, %d), (%d, %d)\n", hdc, left, top,
           right, bottom, xstart, ystart, xend, yend );

    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_ArcChordPie( dc_attr, left, top, right, bottom,
                                            xstart, ystart, xend, yend, EMR_ARCTO ))
        return FALSE;

    return NtGdiArcInternal( NtGdiArcTo, hdc, left, top, right, bottom,
                             xstart, ystart, xend, yend );
}

/***********************************************************************
 *           Chord    (GDI32.@)
 */
BOOL WINAPI Chord( HDC hdc, INT left, INT top, INT right, INT bottom,
                   INT xstart, INT ystart, INT xend, INT yend )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)-(%d, %d), (%d, %d), (%d, %d)\n", hdc, left, top,
           right, bottom, xstart, ystart, xend, yend );

    if (is_meta_dc( hdc ))
        return METADC_Chord( hdc, left, top, right, bottom,
                             xstart, ystart, xend, yend );

    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_ArcChordPie( dc_attr, left, top, right, bottom,
                                            xstart, ystart, xend, yend, EMR_CHORD ))
        return FALSE;

    return NtGdiArcInternal( NtGdiChord, hdc, left, top, right, bottom,
                             xstart, ystart, xend, yend );
}

/***********************************************************************
 *           Pie   (GDI32.@)
 */
BOOL WINAPI Pie( HDC hdc, INT left, INT top, INT right, INT bottom,
                 INT xstart, INT ystart, INT xend, INT yend )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)-(%d, %d), (%d, %d), (%d, %d)\n", hdc, left, top,
           right, bottom, xstart, ystart, xend, yend );

    if (is_meta_dc( hdc ))
        return METADC_Pie( hdc, left, top, right, bottom,
                           xstart, ystart, xend, yend );

    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_ArcChordPie( dc_attr, left, top, right, bottom,
                                            xstart, ystart, xend, yend, EMR_PIE ))
        return FALSE;

    return NtGdiArcInternal( NtGdiPie, hdc, left, top, right, bottom,
                             xstart, ystart, xend, yend );
}

/***********************************************************************
 *           Ellipse    (GDI32.@)
 */
BOOL WINAPI Ellipse( HDC hdc, INT left, INT top, INT right, INT bottom )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)-(%d, %d)\n", hdc, left, top, right, bottom );

    if (is_meta_dc( hdc )) return METADC_Ellipse( hdc, left, top, right, bottom );
    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_Ellipse( dc_attr, left, top, right, bottom )) return FALSE;
    return NtGdiEllipse( hdc, left, top, right, bottom );
}

/***********************************************************************
 *           RoundRect    (GDI32.@)
 */
BOOL WINAPI RoundRect( HDC hdc, INT left, INT top, INT right,
                       INT bottom, INT ell_width, INT ell_height )
{
    DC_ATTR *dc_attr;

    TRACE( "%p, (%d, %d)-(%d, %d), %dx%d\n", hdc, left, top, right, bottom,
           ell_width, ell_height );

    if (is_meta_dc( hdc ))
        return METADC_RoundRect( hdc, left, top, right, bottom, ell_width, ell_height );

    if (!(dc_attr = get_dc_attr( hdc ))) return FALSE;
    if (dc_attr->emf && !EMFDC_RoundRect( dc_attr, left, top, right, bottom,
                                          ell_width, ell_height ))
        return FALSE;

    return NtGdiRoundRect( hdc, left, top, right, bottom, ell_width, ell_height );
}

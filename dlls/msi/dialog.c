/*
 * Implementation of the Microsoft Installer (msi.dll)
 *
 * Copyright 2005 Mike McCormack for CodeWeavers
 * Copyright 2005 Aric Stewart for CodeWeavers
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

#define COBJMACROS
#define NONAMELESSUNION

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "wingdi.h"
#include "winuser.h"
#include "winnls.h"
#include "msi.h"
#include "msidefs.h"
#include "ocidl.h"
#include "olectl.h"
#include "richedit.h"
#include "commctrl.h"
#include "winreg.h"
#include "shlwapi.h"
#include "shellapi.h"

#include "wine/debug.h"

#include "msipriv.h"
#include "msiserver.h"
#include "resource.h"

WINE_DEFAULT_DEBUG_CHANNEL(msi);

extern HINSTANCE msi_hInstance;

struct msi_control_tag;
typedef struct msi_control_tag msi_control;
typedef UINT (*msi_handler)( msi_dialog *, msi_control *, WPARAM );
typedef void (*msi_update)( msi_dialog *, msi_control * );
typedef UINT (*control_event_handler)( msi_dialog *, const WCHAR *, const WCHAR * );
typedef UINT (*event_handler)( msi_dialog *, const WCHAR * );

struct msi_control_tag
{
    struct list entry;
    HWND hwnd;
    msi_handler handler;
    msi_update update;
    LPWSTR property;
    LPWSTR value;
    HBITMAP hBitmap;
    HICON hIcon;
    HIMAGELIST hImageList;
    LPWSTR tabnext;
    LPWSTR type;
    HMODULE hDll;
    float progress_current;
    float progress_max;
    BOOL  progress_backwards;
    DWORD attributes;
    WCHAR name[1];
};

typedef struct msi_font_tag
{
    struct list entry;
    HFONT hfont;
    COLORREF color;
    WCHAR name[1];
} msi_font;

struct msi_dialog_tag
{
    MSIPACKAGE *package;
    msi_dialog *parent;
    control_event_handler event_handler;
    BOOL finished;
    INT scale;
    DWORD attributes;
    SIZE size;
    HWND hwnd;
    LPWSTR default_font;
    struct list fonts;
    struct list controls;
    HWND hWndFocus;
    LPWSTR control_default;
    LPWSTR control_cancel;
    event_handler pending_event;
    LPWSTR pending_argument;
    INT retval;
    WCHAR name[1];
};

struct subscriber
{
    struct list entry;
    msi_dialog *dialog;
    WCHAR      *event;
    WCHAR      *control;
    WCHAR      *attribute;
};

typedef UINT (*msi_dialog_control_func)( msi_dialog *dialog, MSIRECORD *rec );
struct control_handler
{
    LPCWSTR control_type;
    msi_dialog_control_func func;
};

typedef struct
{
    msi_dialog* dialog;
    msi_control *parent;
    LPWSTR      propval;
} radio_button_group_descr;

/* dialog sequencing */

#define WM_MSI_DIALOG_CREATE  (WM_USER+0x100)
#define WM_MSI_DIALOG_DESTROY (WM_USER+0x101)

#define USER_INSTALLSTATE_ALL 0x1000

static DWORD uiThreadId;
static HWND hMsiHiddenWindow;

static LPWSTR msi_get_window_text( HWND hwnd )
{
    UINT sz, r;
    LPWSTR buf;

    sz = 0x20;
    buf = malloc( sz * sizeof(WCHAR) );
    while ( buf )
    {
        r = GetWindowTextW( hwnd, buf, sz );
        if ( r < (sz - 1) )
            break;
        sz *= 2;
        buf = realloc( buf, sz * sizeof(WCHAR) );
    }

    return buf;
}

static INT msi_dialog_scale_unit( msi_dialog *dialog, INT val )
{
    return MulDiv( val, dialog->scale, 12 );
}

static msi_control *msi_dialog_find_control( msi_dialog *dialog, LPCWSTR name )
{
    msi_control *control;

    if( !name )
        return NULL;
    if( !dialog->hwnd )
        return NULL;
    LIST_FOR_EACH_ENTRY( control, &dialog->controls, msi_control, entry )
        if( !wcscmp( control->name, name ) ) /* FIXME: case sensitive? */
            return control;
    return NULL;
}

static msi_control *msi_dialog_find_control_by_type( msi_dialog *dialog, LPCWSTR type )
{
    msi_control *control;

    if( !type )
        return NULL;
    if( !dialog->hwnd )
        return NULL;
    LIST_FOR_EACH_ENTRY( control, &dialog->controls, msi_control, entry )
        if( !wcscmp( control->type, type ) ) /* FIXME: case sensitive? */
            return control;
    return NULL;
}

static msi_control *msi_dialog_find_control_by_hwnd( msi_dialog *dialog, HWND hwnd )
{
    msi_control *control;

    if( !dialog->hwnd )
        return NULL;
    LIST_FOR_EACH_ENTRY( control, &dialog->controls, msi_control, entry )
        if( hwnd == control->hwnd )
            return control;
    return NULL;
}

static LPWSTR msi_get_deformatted_field( MSIPACKAGE *package, MSIRECORD *rec, int field )
{
    LPCWSTR str = MSI_RecordGetString( rec, field );
    LPWSTR ret = NULL;

    if (str)
        deformat_string( package, str, &ret );
    return ret;
}

static LPWSTR msi_dialog_dup_property( msi_dialog *dialog, LPCWSTR property, BOOL indirect )
{
    LPWSTR prop = NULL;

    if (!property)
        return NULL;

    if (indirect)
        prop = msi_dup_property( dialog->package->db, property );

    if (!prop)
        prop = wcsdup( property );

    return prop;
}

/*
 * msi_dialog_get_style
 *
 * Extract the {\style} string from the front of the text to display and
 * update the pointer.  Only the last style in a list is applied.
 */
static LPWSTR msi_dialog_get_style( LPCWSTR p, LPCWSTR *rest )
{
    LPWSTR ret;
    LPCWSTR q, i, first;
    DWORD len;

    q = NULL;
    *rest = p;
    if( !p )
        return NULL;

    while ((first = wcschr( p, '{' )) && (q = wcschr( first + 1, '}' )))
    {
        p = first + 1;
        if( *p != '\\' && *p != '&' )
            return NULL;

        /* little bit of sanity checking to stop us getting confused with RTF */
        for( i=++p; i<q; i++ )
            if( *i == '}' || *i == '\\' )
                return NULL;
    }

    if (!q)
        return NULL;

    *rest = ++q;
    len = q - p;

    ret = malloc( len * sizeof(WCHAR) );
    if( !ret )
        return ret;
    memcpy( ret, p, len*sizeof(WCHAR) );
    ret[len-1] = 0;
    return ret;
}

static UINT msi_dialog_add_font( MSIRECORD *rec, LPVOID param )
{
    msi_dialog *dialog = param;
    msi_font *font;
    LPCWSTR face, name;
    LOGFONTW lf;
    INT style;
    HDC hdc;

    /* create a font and add it to the list */
    name = MSI_RecordGetString( rec, 1 );
    font = malloc( offsetof( msi_font, name[wcslen( name ) + 1] ) );
    lstrcpyW( font->name, name );
    list_add_head( &dialog->fonts, &font->entry );

    font->color = MSI_RecordGetInteger( rec, 4 );

    memset( &lf, 0, sizeof lf );
    face = MSI_RecordGetString( rec, 2 );
    lf.lfHeight = MSI_RecordGetInteger( rec, 3 );
    style = MSI_RecordGetInteger( rec, 5 );
    if( style & msidbTextStyleStyleBitsBold )
        lf.lfWeight = FW_BOLD;
    if( style & msidbTextStyleStyleBitsItalic )
        lf.lfItalic = TRUE;
    if( style & msidbTextStyleStyleBitsUnderline )
        lf.lfUnderline = TRUE;
    if( style & msidbTextStyleStyleBitsStrike )
        lf.lfStrikeOut = TRUE;
    lstrcpynW( lf.lfFaceName, face, LF_FACESIZE );

    /* adjust the height */
    hdc = GetDC( dialog->hwnd );
    if (hdc)
    {
        lf.lfHeight = -MulDiv(lf.lfHeight, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        ReleaseDC( dialog->hwnd, hdc );
    }

    font->hfont = CreateFontIndirectW( &lf );

    TRACE("Adding font style %s\n", debugstr_w(font->name) );

    return ERROR_SUCCESS;
}

static msi_font *msi_dialog_find_font( msi_dialog *dialog, LPCWSTR name )
{
    msi_font *font = NULL;

    LIST_FOR_EACH_ENTRY( font, &dialog->fonts, msi_font, entry )
        if( !wcscmp( font->name, name ) )  /* FIXME: case sensitive? */
            break;

    return font;
}

static UINT msi_dialog_set_font( msi_dialog *dialog, HWND hwnd, LPCWSTR name )
{
    msi_font *font;

    font = msi_dialog_find_font( dialog, name );
    if( font )
        SendMessageW( hwnd, WM_SETFONT, (WPARAM) font->hfont, TRUE );
    else
        ERR("No font entry for %s\n", debugstr_w(name));
    return ERROR_SUCCESS;
}

static UINT msi_dialog_build_font_list( msi_dialog *dialog )
{
    MSIQUERY *view;
    UINT r;

    TRACE("dialog %p\n", dialog );

    r = MSI_OpenQuery( dialog->package->db, &view, L"SELECT * FROM `TextStyle`" );
    if( r != ERROR_SUCCESS )
        return r;

    r = MSI_IterateRecords( view, NULL, msi_dialog_add_font, dialog );
    msiobj_release( &view->hdr );
    return r;
}

static void msi_destroy_control( msi_control *t )
{
    list_remove( &t->entry );
    /* leave dialog->hwnd - destroying parent destroys child windows */
    free( t->property );
    free( t->value );
    if( t->hBitmap )
        DeleteObject( t->hBitmap );
    if( t->hIcon )
        DestroyIcon( t->hIcon );
    if ( t->hImageList )
        ImageList_Destroy( t->hImageList );
    free( t->tabnext );
    free( t->type );
    if (t->hDll)
        FreeLibrary( t->hDll );
    free( t );
}

static msi_control *dialog_create_window( msi_dialog *dialog, MSIRECORD *rec, DWORD exstyle,
                                          const WCHAR *szCls, const WCHAR *name, const WCHAR *text,
                                          DWORD style, HWND parent )
{
    DWORD x, y, width, height;
    LPWSTR font = NULL, title_font = NULL;
    LPCWSTR title = NULL;
    msi_control *control;

    style |= WS_CHILD;

    control = malloc( offsetof( msi_control, name[wcslen( name ) + 1] ) );
    if (!control)
        return NULL;

    lstrcpyW( control->name, name );
    list_add_tail( &dialog->controls, &control->entry );
    control->handler = NULL;
    control->update = NULL;
    control->property = NULL;
    control->value = NULL;
    control->hBitmap = NULL;
    control->hIcon = NULL;
    control->hImageList = NULL;
    control->hDll = NULL;
    control->tabnext = wcsdup( MSI_RecordGetString( rec, 11 ) );
    control->type = wcsdup( MSI_RecordGetString( rec, 3 ) );
    control->progress_current = 0;
    control->progress_max = 100;
    control->progress_backwards = FALSE;

    x = MSI_RecordGetInteger( rec, 4 );
    y = MSI_RecordGetInteger( rec, 5 );
    width = MSI_RecordGetInteger( rec, 6 );
    height = MSI_RecordGetInteger( rec, 7 );

    x = msi_dialog_scale_unit( dialog, x );
    y = msi_dialog_scale_unit( dialog, y );
    width = msi_dialog_scale_unit( dialog, width );
    height = msi_dialog_scale_unit( dialog, height );

    if( text )
    {
        deformat_string( dialog->package, text, &title_font );
        font = msi_dialog_get_style( title_font, &title );
    }

    control->hwnd = CreateWindowExW( exstyle, szCls, title, style,
                          x, y, width, height, parent, NULL, NULL, NULL );

    TRACE("Dialog %s control %s hwnd %p\n",
           debugstr_w(dialog->name), debugstr_w(text), control->hwnd );

    msi_dialog_set_font( dialog, control->hwnd,
                         font ? font : dialog->default_font );

    free( title_font );
    free( font );

    return control;
}

static LPWSTR msi_dialog_get_uitext( msi_dialog *dialog, LPCWSTR key )
{
    MSIRECORD *rec;
    LPWSTR text;

    rec = MSI_QueryGetRecord( dialog->package->db, L"SELECT * FROM `UIText` WHERE `Key` = '%s'", key );
    if (!rec) return NULL;
    text = wcsdup( MSI_RecordGetString( rec, 2 ) );
    msiobj_release( &rec->hdr );
    return text;
}

static HANDLE msi_load_image( MSIDATABASE *db, LPCWSTR name, UINT type,
                              UINT cx, UINT cy, UINT flags )
{
    MSIRECORD *rec;
    HANDLE himage = NULL;
    LPWSTR tmp;
    UINT r;

    TRACE("%p %s %u %u %08x\n", db, debugstr_w(name), cx, cy, flags);

    if (!(tmp = msi_create_temp_file( db ))) return NULL;

    rec = MSI_QueryGetRecord( db, L"SELECT * FROM `Binary` WHERE `Name` = '%s'", name );
    if( rec )
    {
        r = MSI_RecordStreamToFile( rec, 2, tmp );
        if( r == ERROR_SUCCESS )
        {
            himage = LoadImageW( 0, tmp, type, cx, cy, flags );
        }
        msiobj_release( &rec->hdr );
    }
    DeleteFileW( tmp );

    free( tmp );
    return himage;
}

static HICON msi_load_icon( MSIDATABASE *db, LPCWSTR text, UINT attributes )
{
    DWORD cx = 0, cy = 0, flags;

    flags = LR_LOADFROMFILE | LR_DEFAULTSIZE;
    if( attributes & msidbControlAttributesFixedSize )
    {
        flags &= ~LR_DEFAULTSIZE;
        if( attributes & msidbControlAttributesIconSize16 )
        {
            cx += 16;
            cy += 16;
        }
        if( attributes & msidbControlAttributesIconSize32 )
        {
            cx += 32;
            cy += 32;
        }
        /* msidbControlAttributesIconSize48 handled by above logic */
    }
    return msi_load_image( db, text, IMAGE_ICON, cx, cy, flags );
}

static void msi_dialog_update_controls( msi_dialog *dialog, LPCWSTR property )
{
    msi_control *control;

    LIST_FOR_EACH_ENTRY( control, &dialog->controls, msi_control, entry )
    {
        if ( control->property && !wcscmp( control->property, property ) && control->update )
            control->update( dialog, control );
    }
}

static void msi_dialog_update_all_controls( msi_dialog *dialog )
{
    msi_control *control;

    LIST_FOR_EACH_ENTRY( control, &dialog->controls, msi_control, entry )
    {
        if ( control->property && control->update )
            control->update( dialog, control );
    }
}

static void msi_dialog_set_property( MSIPACKAGE *package, LPCWSTR property, LPCWSTR value )
{
    UINT r = msi_set_property( package->db, property, value, -1 );
    if (r == ERROR_SUCCESS && !wcscmp( property, L"SourceDir" ))
        msi_reset_source_folders( package );
}

static MSIFEATURE *msi_seltree_feature_from_item( HWND hwnd, HTREEITEM hItem )
{
    TVITEMW tvi;

    /* get the feature from the item */
    memset( &tvi, 0, sizeof tvi );
    tvi.hItem = hItem;
    tvi.mask = TVIF_PARAM | TVIF_HANDLE;
    SendMessageW( hwnd, TVM_GETITEMW, 0, (LPARAM)&tvi );
    return (MSIFEATURE *)tvi.lParam;
}

struct msi_selection_tree_info
{
    msi_dialog *dialog;
    HWND hwnd;
    WNDPROC oldproc;
    HTREEITEM selected;
};

static MSIFEATURE *msi_seltree_get_selected_feature( msi_control *control )
{
    struct msi_selection_tree_info *info = GetPropW( control->hwnd, L"MSIDATA" );
    return msi_seltree_feature_from_item( control->hwnd, info->selected );
}

static void dialog_handle_event( msi_dialog *dialog, const WCHAR *control,
                                 const WCHAR *attribute, MSIRECORD *rec )
{
    msi_control* ctrl;

    ctrl = msi_dialog_find_control( dialog, control );
    if (!ctrl)
        return;
    if( !wcscmp( attribute, L"Text" ) )
    {
        const WCHAR *font_text, *text = NULL;
        WCHAR *font, *text_fmt = NULL;

        font_text = MSI_RecordGetString( rec , 1 );
        font = msi_dialog_get_style( font_text, &text );
        deformat_string( dialog->package, text, &text_fmt );
        if (text_fmt) text = text_fmt;
        else text = L"";

        SetWindowTextW( ctrl->hwnd, text );

        free( font );
        free( text_fmt );
        msi_dialog_check_messages( NULL );
    }
    else if( !wcscmp( attribute, L"Progress" ) )
    {
        DWORD func, val1, val2, units;

        func = MSI_RecordGetInteger( rec, 1 );
        val1 = MSI_RecordGetInteger( rec, 2 );
        val2 = MSI_RecordGetInteger( rec, 3 );

        TRACE( "progress: func %lu val1 %lu val2 %lu\n", func, val1, val2 );

        units = val1 / 512;
        switch (func)
        {
        case 0: /* init */
            SendMessageW( ctrl->hwnd, PBM_SETRANGE, 0, MAKELPARAM(0,100) );
            if (val2)
            {
                ctrl->progress_max = units ? units : 100;
                ctrl->progress_current = units;
                ctrl->progress_backwards = TRUE;
                SendMessageW( ctrl->hwnd, PBM_SETPOS, 100, 0 );
            }
            else
            {
                ctrl->progress_max = units ? units : 100;
                ctrl->progress_current = 0;
                ctrl->progress_backwards = FALSE;
                SendMessageW( ctrl->hwnd, PBM_SETPOS, 0, 0 );
            }
            break;
        case 1: /* action data increment */
            if (val2) dialog->package->action_progress_increment = val1;
            else dialog->package->action_progress_increment = 0;
            break;
        case 2: /* move */
            if (ctrl->progress_backwards)
            {
                if (units >= ctrl->progress_current) ctrl->progress_current -= units;
                else ctrl->progress_current = 0;
            }
            else
            {
                if (ctrl->progress_current + units < ctrl->progress_max) ctrl->progress_current += units;
                else ctrl->progress_current = ctrl->progress_max;
            }
            SendMessageW( ctrl->hwnd, PBM_SETPOS, MulDiv(100, ctrl->progress_current, ctrl->progress_max), 0 );
            break;
        case 3: /* add */
            ctrl->progress_max += units;
            break;
        default:
            FIXME( "unknown progress message %lu\n", func );
            break;
        }
    }
    else if ( !wcscmp( attribute, L"Property" ) )
    {
        MSIFEATURE *feature = msi_seltree_get_selected_feature( ctrl );
        if (feature) msi_dialog_set_property( dialog->package, ctrl->property, feature->Directory );
    }
    else if ( !wcscmp( attribute, L"SelectionPath" ) )
    {
        BOOL indirect = ctrl->attributes & msidbControlAttributesIndirect;
        LPWSTR path = msi_dialog_dup_property( dialog, ctrl->property, indirect );
        if (!path) return;
        SetWindowTextW( ctrl->hwnd, path );
        free( path );
    }
    else
    {
        FIXME("Attribute %s not being set\n", debugstr_w(attribute));
        return;
    }
}

static void event_subscribe( msi_dialog *dialog, const WCHAR *event, const WCHAR *control, const WCHAR *attribute )
{
    struct subscriber *sub;

    TRACE("dialog %s event %s control %s attribute %s\n", debugstr_w(dialog->name), debugstr_w(event),
          debugstr_w(control), debugstr_w(attribute));

    LIST_FOR_EACH_ENTRY( sub, &dialog->package->subscriptions, struct subscriber, entry )
    {
        if (sub->dialog == dialog &&
            !wcsicmp( sub->event, event ) &&
            !wcsicmp( sub->control, control ) &&
            !wcsicmp( sub->attribute, attribute ))
        {
            TRACE("already subscribed\n");
            return;
        };
    }
    if (!(sub = malloc( sizeof(*sub) ))) return;
    sub->dialog    = dialog;
    sub->event     = wcsdup( event );
    sub->control   = wcsdup( control );
    sub->attribute = wcsdup( attribute );
    list_add_tail( &dialog->package->subscriptions, &sub->entry );
}

struct dialog_control
{
    msi_dialog  *dialog;
    const WCHAR *control;
};

static UINT map_event( MSIRECORD *row, void *param )
{
    struct dialog_control *dc = param;
    const WCHAR *event = MSI_RecordGetString( row, 3 );
    const WCHAR *attribute = MSI_RecordGetString( row, 4 );

    event_subscribe( dc->dialog, event, dc->control, attribute );
    return ERROR_SUCCESS;
}

static void dialog_map_events( msi_dialog *dialog, const WCHAR *control )
{
    MSIQUERY *view;
    struct dialog_control dialog_control =
    {
        dialog,
        control
    };

    if (!MSI_OpenQuery( dialog->package->db, &view,
                        L"SELECT * FROM `EventMapping` WHERE `Dialog_` = '%s' AND `Control_` = '%s'",
                        dialog->name, control ))
    {
        MSI_IterateRecords( view, NULL, map_event, &dialog_control );
        msiobj_release( &view->hdr );
    }
}

/* everything except radio buttons */
static msi_control *msi_dialog_add_control( msi_dialog *dialog,
                MSIRECORD *rec, LPCWSTR szCls, DWORD style )
{
    DWORD attributes;
    const WCHAR *text = NULL, *name, *control_type;
    DWORD exstyle = 0;

    name = MSI_RecordGetString( rec, 2 );
    control_type = MSI_RecordGetString( rec, 3 );
    attributes = MSI_RecordGetInteger( rec, 8 );
    if (wcscmp( control_type, L"ScrollableText" )) text = MSI_RecordGetString( rec, 10 );

    TRACE( "%s, %s, %#lx, %s, %#lx\n", debugstr_w(szCls), debugstr_w(name), attributes, debugstr_w(text), style );

    if( attributes & msidbControlAttributesVisible )
        style |= WS_VISIBLE;
    if( ~attributes & msidbControlAttributesEnabled )
        style |= WS_DISABLED;
    if( attributes & msidbControlAttributesSunken )
        exstyle |= WS_EX_CLIENTEDGE;

    dialog_map_events( dialog, name );

    return dialog_create_window( dialog, rec, exstyle, szCls, name, text, style, dialog->hwnd );
}

struct msi_text_info
{
    msi_font *font;
    WNDPROC oldproc;
    DWORD attributes;
};

/*
 * we don't erase our own background,
 * so we have to make sure that the parent window redraws first
 */
static void msi_text_on_settext( HWND hWnd )
{
    HWND hParent;
    RECT rc;

    hParent = GetParent( hWnd );
    GetWindowRect( hWnd, &rc );
    MapWindowPoints( NULL, hParent, (LPPOINT) &rc, 2 );
    InvalidateRect( hParent, &rc, TRUE );
}

static LRESULT WINAPI
MSIText_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_text_info *info;
    LRESULT r = 0;

    TRACE( "%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam );

    info = GetPropW(hWnd, L"MSIDATA");

    if( msg == WM_CTLCOLORSTATIC &&
       ( info->attributes & msidbControlAttributesTransparent ) )
    {
        SetBkMode( (HDC)wParam, TRANSPARENT );
        return (LRESULT) GetStockObject(NULL_BRUSH);
    }

    r = CallWindowProcW(info->oldproc, hWnd, msg, wParam, lParam);
    if ( info->font )
        SetTextColor( (HDC)wParam, info->font->color );

    switch( msg )
    {
    case WM_SETTEXT:
        msi_text_on_settext( hWnd );
        break;
    case WM_NCDESTROY:
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
        break;
    }

    return r;
}

static UINT msi_dialog_text_control( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    struct msi_text_info *info;
    LPCWSTR text, ptr, prop, control_name;
    LPWSTR font_name;

    TRACE("%p %p\n", dialog, rec);

    control = msi_dialog_add_control( dialog, rec, L"Static", SS_LEFT | WS_GROUP );
    if( !control )
        return ERROR_FUNCTION_FAILED;

    info = malloc( sizeof *info );
    if( !info )
        return ERROR_SUCCESS;

    control_name = MSI_RecordGetString( rec, 2 );
    control->attributes = MSI_RecordGetInteger( rec, 8 );
    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    text = MSI_RecordGetString( rec, 10 );
    font_name = msi_dialog_get_style( text, &ptr );
    info->font = ( font_name ) ? msi_dialog_find_font( dialog, font_name ) : NULL;
    free( font_name );

    info->attributes = MSI_RecordGetInteger( rec, 8 );
    if( info->attributes & msidbControlAttributesTransparent )
        SetWindowLongPtrW( control->hwnd, GWL_EXSTYLE, WS_EX_TRANSPARENT );

    info->oldproc = (WNDPROC) SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                          (LONG_PTR)MSIText_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    event_subscribe( dialog, L"SelectionPath", control_name, L"SelectionPath" );
    return ERROR_SUCCESS;
}

/* strip any leading text style label from text field */
static WCHAR *msi_get_binary_name( MSIPACKAGE *package, MSIRECORD *rec )
{
    WCHAR *p, *text;

    text = msi_get_deformatted_field( package, rec, 10 );
    if (!text)
        return NULL;

    p = text;
    while (*p && *p != '{') p++;
    if (!*p++) return text;

    while (*p && *p != '}') p++;
    if (!*p++) return text;

    p = wcsdup( p );
    free( text );
    return p;
}

static UINT msi_dialog_set_property_event( msi_dialog *dialog, LPCWSTR event, LPCWSTR arg )
{
    LPWSTR p, prop, arg_fmt = NULL;
    UINT len;

    len = lstrlenW( event );
    prop = malloc( len * sizeof(WCHAR) );
    lstrcpyW( prop, &event[1] );
    p = wcschr( prop, ']' );
    if (p && (p[1] == 0 || p[1] == ' '))
    {
        *p = 0;
        if (wcscmp( L"{}", arg )) deformat_string( dialog->package, arg, &arg_fmt );
        msi_dialog_set_property( dialog->package, prop, arg_fmt );
        msi_dialog_update_controls( dialog, prop );
        free( arg_fmt );
    }
    else ERR("Badly formatted property string - what happens?\n");
    free( prop );
    return ERROR_SUCCESS;
}

static UINT msi_dialog_send_event( msi_dialog *dialog, LPCWSTR event, LPCWSTR arg )
{
    LPWSTR event_fmt = NULL, arg_fmt = NULL;

    TRACE("Sending control event %s %s\n", debugstr_w(event), debugstr_w(arg));

    deformat_string( dialog->package, event, &event_fmt );
    deformat_string( dialog->package, arg, &arg_fmt );

    dialog->event_handler( dialog, event_fmt, arg_fmt );

    free( event_fmt );
    free( arg_fmt );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_control_event( MSIRECORD *rec, LPVOID param )
{
    msi_dialog *dialog = param;
    LPCWSTR condition, event, arg;
    UINT r;

    condition = MSI_RecordGetString( rec, 5 );
    r = MSI_EvaluateConditionW( dialog->package, condition );
    if (r == MSICONDITION_TRUE || r == MSICONDITION_NONE)
    {
        event = MSI_RecordGetString( rec, 3 );
        arg = MSI_RecordGetString( rec, 4 );
        if (event[0] == '[')
            msi_dialog_set_property_event( dialog, event, arg );
        else
            msi_dialog_send_event( dialog, event, arg );
    }
    return ERROR_SUCCESS;
}

static UINT msi_dialog_button_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    MSIQUERY *view;
    UINT r;

    if (HIWORD(param) != BN_CLICKED)
        return ERROR_SUCCESS;

    r = MSI_OpenQuery( dialog->package->db, &view,
                       L"SELECT * FROM `ControlEvent` WHERE `Dialog_` = '%s' AND `Control_` = '%s' ORDER BY `Ordering`",
                       dialog->name, control->name );
    if (r != ERROR_SUCCESS)
    {
        ERR("query failed\n");
        return ERROR_SUCCESS;
    }
    r = MSI_IterateRecords( view, 0, msi_dialog_control_event, dialog );
    msiobj_release( &view->hdr );

    /* dialog control events must be processed last regardless of ordering */
    if (dialog->pending_event)
    {
        r = dialog->pending_event( dialog, dialog->pending_argument );

        free( dialog->pending_argument );
        dialog->pending_event = NULL;
        dialog->pending_argument = NULL;
    }
    return r;
}

static HBITMAP msi_load_picture( MSIDATABASE *db, const WCHAR *name, INT cx, INT cy, DWORD flags )
{
    HBITMAP hOleBitmap = 0, hBitmap = 0, hOldSrcBitmap, hOldDestBitmap;
    MSIRECORD *rec = NULL;
    IStream *stm = NULL;
    IPicture *pic = NULL;
    HDC srcdc, destdc;
    BITMAP bm;
    UINT r;

    rec = MSI_QueryGetRecord( db, L"SELECT * FROM `Binary` WHERE `Name` = '%s'", name );
    if (!rec)
        goto end;

    r = MSI_RecordGetIStream( rec, 2, &stm );
    msiobj_release( &rec->hdr );
    if (r != ERROR_SUCCESS)
        goto end;

    r = OleLoadPicture( stm, 0, TRUE, &IID_IPicture, (void **)&pic );
    IStream_Release( stm );
    if (FAILED( r ))
    {
        ERR("failed to load picture\n");
        goto end;
    }

    r = IPicture_get_Handle( pic, (OLE_HANDLE *)&hOleBitmap );
    if (FAILED( r ))
    {
        ERR("failed to get bitmap handle\n");
        goto end;
    }

    /* make the bitmap the desired size */
    r = GetObjectW( hOleBitmap, sizeof(bm), &bm );
    if (r != sizeof(bm))
    {
        ERR("failed to get bitmap size\n");
        goto end;
    }

    if (flags & LR_DEFAULTSIZE)
    {
        cx = bm.bmWidth;
        cy = bm.bmHeight;
    }

    srcdc = CreateCompatibleDC( NULL );
    hOldSrcBitmap = SelectObject( srcdc, hOleBitmap );
    destdc = CreateCompatibleDC( NULL );
    hBitmap = CreateCompatibleBitmap( srcdc, cx, cy );
    hOldDestBitmap = SelectObject( destdc, hBitmap );
    StretchBlt( destdc, 0, 0, cx, cy, srcdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY );
    SelectObject( srcdc, hOldSrcBitmap );
    SelectObject( destdc, hOldDestBitmap );
    DeleteDC( srcdc );
    DeleteDC( destdc );

end:
    if (pic) IPicture_Release( pic );
    return hBitmap;
}

static UINT msi_dialog_button_control( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    UINT attributes, style, cx = 0, cy = 0, flags = 0;
    WCHAR *name = NULL;

    TRACE("%p %p\n", dialog, rec);

    style = WS_TABSTOP;
    attributes = MSI_RecordGetInteger( rec, 8 );
    if (attributes & msidbControlAttributesIcon) style |= BS_ICON;
    else if (attributes & msidbControlAttributesBitmap)
    {
        style |= BS_BITMAP;
        if (attributes & msidbControlAttributesFixedSize) flags |= LR_DEFAULTSIZE;
        else
        {
            cx = msi_dialog_scale_unit( dialog, MSI_RecordGetInteger(rec, 6) );
            cy = msi_dialog_scale_unit( dialog, MSI_RecordGetInteger(rec, 7) );
        }
    }

    control = msi_dialog_add_control( dialog, rec, L"BUTTON", style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    control->handler = msi_dialog_button_handler;

    if (attributes & msidbControlAttributesIcon)
    {
        name = msi_get_binary_name( dialog->package, rec );
        control->hIcon = msi_load_icon( dialog->package->db, name, attributes );
        if (control->hIcon)
        {
            SendMessageW( control->hwnd, BM_SETIMAGE, IMAGE_ICON, (LPARAM) control->hIcon );
        }
        else ERR("Failed to load icon %s\n", debugstr_w(name));
    }
    else if (attributes & msidbControlAttributesBitmap)
    {
        name = msi_get_binary_name( dialog->package, rec );
        control->hBitmap = msi_load_picture( dialog->package->db, name, cx, cy, flags );
        if (control->hBitmap)
        {
            SendMessageW( control->hwnd, BM_SETIMAGE, IMAGE_BITMAP, (LPARAM) control->hBitmap );
        }
        else ERR("Failed to load bitmap %s\n", debugstr_w(name));
    }

    free( name );
    return ERROR_SUCCESS;
}

static LPWSTR msi_get_checkbox_value( msi_dialog *dialog, LPCWSTR prop )
{
    MSIRECORD *rec = NULL;
    LPWSTR ret = NULL;

    /* find if there is a value associated with the checkbox */
    rec = MSI_QueryGetRecord( dialog->package->db, L"SELECT * FROM `CheckBox` WHERE `Property` = '%s'", prop );
    if (!rec)
        return ret;

    ret = msi_get_deformatted_field( dialog->package, rec, 2 );
    if( ret && !ret[0] )
    {
        free( ret );
        ret = NULL;
    }
    msiobj_release( &rec->hdr );
    if (ret)
        return ret;

    ret = msi_dup_property( dialog->package->db, prop );
    if( ret && !ret[0] )
    {
        free( ret );
        ret = NULL;
    }

    return ret;
}

static UINT msi_dialog_get_checkbox_state( msi_dialog *dialog, msi_control *control )
{
    WCHAR state[2] = {0};
    DWORD sz = 2;

    msi_get_property( dialog->package->db, control->property, state, &sz );
    return state[0] ? 1 : 0;
}

static void msi_dialog_set_checkbox_state( msi_dialog *dialog, msi_control *control, UINT state )
{
    LPCWSTR val;

    /* if uncheck then the property is set to NULL */
    if (!state)
    {
        msi_dialog_set_property( dialog->package, control->property, NULL );
        return;
    }

    /* check for a custom state */
    if (control->value && control->value[0])
        val = control->value;
    else
        val = L"1";

    msi_dialog_set_property( dialog->package, control->property, val );
}

static void msi_dialog_checkbox_sync_state( msi_dialog *dialog, msi_control *control )
{
    UINT state = msi_dialog_get_checkbox_state( dialog, control );
    SendMessageW( control->hwnd, BM_SETCHECK, state ? BST_CHECKED : BST_UNCHECKED, 0 );
}

static UINT msi_dialog_checkbox_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    UINT state;

    if (HIWORD(param) != BN_CLICKED)
        return ERROR_SUCCESS;

    TRACE("clicked checkbox %s, set %s\n", debugstr_w(control->name), debugstr_w(control->property));

    state = msi_dialog_get_checkbox_state( dialog, control );
    state = state ? 0 : 1;
    msi_dialog_set_checkbox_state( dialog, control, state );
    msi_dialog_checkbox_sync_state( dialog, control );

    return msi_dialog_button_handler( dialog, control, param );
}

static UINT msi_dialog_checkbox_control( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop;

    TRACE("%p %p\n", dialog, rec);

    control = msi_dialog_add_control( dialog, rec, L"BUTTON", BS_CHECKBOX | BS_MULTILINE | WS_TABSTOP );
    control->handler = msi_dialog_checkbox_handler;
    control->update = msi_dialog_checkbox_sync_state;
    prop = MSI_RecordGetString( rec, 9 );
    if (prop)
    {
        control->property = wcsdup( prop );
        control->value = msi_get_checkbox_value( dialog, prop );
        TRACE("control %s value %s\n", debugstr_w(control->property), debugstr_w(control->value));
    }
    msi_dialog_checkbox_sync_state( dialog, control );
    return ERROR_SUCCESS;
}

static UINT msi_dialog_line_control( msi_dialog *dialog, MSIRECORD *rec )
{
    DWORD attributes;
    LPCWSTR name;
    DWORD style, exstyle = 0;
    DWORD x, y, width, height;
    msi_control *control;

    TRACE("%p %p\n", dialog, rec);

    style = WS_CHILD | SS_ETCHEDHORZ | SS_SUNKEN;

    name = MSI_RecordGetString( rec, 2 );
    attributes = MSI_RecordGetInteger( rec, 8 );

    if( attributes & msidbControlAttributesVisible )
        style |= WS_VISIBLE;
    if( ~attributes & msidbControlAttributesEnabled )
        style |= WS_DISABLED;
    if( attributes & msidbControlAttributesSunken )
        exstyle |= WS_EX_CLIENTEDGE;

    dialog_map_events( dialog, name );

    control = malloc( offsetof( msi_control, name[wcslen( name ) + 1] ) );
    if (!control)
        return ERROR_OUTOFMEMORY;

    lstrcpyW( control->name, name );
    list_add_head( &dialog->controls, &control->entry );
    control->handler = NULL;
    control->property = NULL;
    control->value = NULL;
    control->hBitmap = NULL;
    control->hIcon = NULL;
    control->hDll = NULL;
    control->tabnext = wcsdup( MSI_RecordGetString( rec, 11 ) );
    control->type = wcsdup( MSI_RecordGetString( rec, 3 ) );
    control->progress_current = 0;
    control->progress_max = 100;
    control->progress_backwards = FALSE;

    x = MSI_RecordGetInteger( rec, 4 );
    y = MSI_RecordGetInteger( rec, 5 );
    width = MSI_RecordGetInteger( rec, 6 );

    x = msi_dialog_scale_unit( dialog, x );
    y = msi_dialog_scale_unit( dialog, y );
    width = msi_dialog_scale_unit( dialog, width );
    height = 2; /* line is exactly 2 units in height */

    control->hwnd = CreateWindowExW( exstyle, L"Static", NULL, style,
                          x, y, width, height, dialog->hwnd, NULL, NULL, NULL );

    TRACE("Dialog %s control %s hwnd %p\n",
           debugstr_w(dialog->name), debugstr_w(name), control->hwnd );

    return ERROR_SUCCESS;
}

/******************** Scroll Text ********************************************/

struct msi_scrolltext_info
{
    msi_dialog *dialog;
    msi_control *control;
    WNDPROC oldproc;
};

static LRESULT WINAPI
MSIScrollText_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_scrolltext_info *info;
    HRESULT r;

    TRACE( "%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam );

    info = GetPropW( hWnd, L"MSIDATA" );

    r = CallWindowProcW( info->oldproc, hWnd, msg, wParam, lParam );

    switch( msg )
    {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_NCDESTROY:
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
        break;
    case WM_PAINT:
        /* native MSI sets a wait cursor here */
        msi_dialog_button_handler( info->dialog, info->control, BN_CLICKED );
        break;
    }
    return r;
}

struct msi_streamin_info
{
    LPSTR string;
    DWORD offset;
    DWORD length;
};

static DWORD CALLBACK
msi_richedit_stream_in( DWORD_PTR arg, LPBYTE buffer, LONG count, LONG *pcb )
{
    struct msi_streamin_info *info = (struct msi_streamin_info*) arg;

    if( (count + info->offset) > info->length )
        count = info->length - info->offset;
    memcpy( buffer, &info->string[ info->offset ], count );
    *pcb = count;
    info->offset += count;

    TRACE( "%lu/%lu\n", info->offset, info->length );

    return 0;
}

static void msi_scrolltext_add_text( msi_control *control, LPCWSTR text )
{
    struct msi_streamin_info info;
    EDITSTREAM es;

    info.string = strdupWtoA( text );
    info.offset = 0;
    info.length = lstrlenA( info.string ) + 1;

    es.dwCookie = (DWORD_PTR) &info;
    es.dwError = 0;
    es.pfnCallback = msi_richedit_stream_in;

    SendMessageW( control->hwnd, EM_STREAMIN, SF_RTF, (LPARAM) &es );

    free( info.string );
}

static UINT msi_dialog_scrolltext_control( msi_dialog *dialog, MSIRECORD *rec )
{
    struct msi_scrolltext_info *info;
    msi_control *control;
    HMODULE hRichedit;
    LPCWSTR text;
    DWORD style;

    info = malloc( sizeof *info );
    if (!info)
        return ERROR_FUNCTION_FAILED;

    hRichedit = LoadLibraryA("riched20");

    style = WS_BORDER | ES_MULTILINE | WS_VSCROLL |
            ES_READONLY | ES_AUTOVSCROLL | WS_TABSTOP;
    control = msi_dialog_add_control( dialog, rec, L"RichEdit20W", style );
    if (!control)
    {
        FreeLibrary( hRichedit );
        free( info );
        return ERROR_FUNCTION_FAILED;
    }

    control->hDll = hRichedit;

    info->dialog = dialog;
    info->control = control;

    /* subclass the static control */
    info->oldproc = (WNDPROC) SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                          (LONG_PTR)MSIScrollText_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    /* add the text into the richedit */
    text = MSI_RecordGetString( rec, 10 );
    if (text)
        msi_scrolltext_add_text( control, text );

    return ERROR_SUCCESS;
}


static UINT msi_dialog_bitmap_control( msi_dialog *dialog, MSIRECORD *rec )
{
    UINT cx, cy, flags, style, attributes;
    msi_control *control;
    LPWSTR name;

    flags = LR_LOADFROMFILE;
    style = SS_BITMAP | SS_LEFT | WS_GROUP;

    attributes = MSI_RecordGetInteger( rec, 8 );
    if( attributes & msidbControlAttributesFixedSize )
    {
        flags |= LR_DEFAULTSIZE;
        style |= SS_CENTERIMAGE;
    }

    control = msi_dialog_add_control( dialog, rec, L"Static", style );
    cx = MSI_RecordGetInteger( rec, 6 );
    cy = MSI_RecordGetInteger( rec, 7 );
    cx = msi_dialog_scale_unit( dialog, cx );
    cy = msi_dialog_scale_unit( dialog, cy );

    name = msi_get_binary_name( dialog->package, rec );
    control->hBitmap = msi_load_picture( dialog->package->db, name, cx, cy, flags );
    if( control->hBitmap )
        SendMessageW( control->hwnd, STM_SETIMAGE,
                      IMAGE_BITMAP, (LPARAM) control->hBitmap );
    else
        ERR("Failed to load bitmap %s\n", debugstr_w(name));

    free( name );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_icon_control( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    DWORD attributes;
    LPWSTR name;

    TRACE("\n");

    control = msi_dialog_add_control( dialog, rec, L"Static",
                            SS_ICON | SS_CENTERIMAGE | WS_GROUP );

    attributes = MSI_RecordGetInteger( rec, 8 );
    name = msi_get_binary_name( dialog->package, rec );
    control->hIcon = msi_load_icon( dialog->package->db, name, attributes );
    if( control->hIcon )
        SendMessageW( control->hwnd, STM_SETICON, (WPARAM) control->hIcon, 0 );
    else
        ERR("Failed to load bitmap %s\n", debugstr_w(name));
    free( name );
    return ERROR_SUCCESS;
}

/******************** Combo Box ***************************************/

struct msi_combobox_info
{
    msi_dialog *dialog;
    HWND hwnd;
    WNDPROC oldproc;
    DWORD num_items;
    DWORD addpos_items;
    LPWSTR *items;
};

static LRESULT WINAPI MSIComboBox_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_combobox_info *info;
    LRESULT r;
    DWORD j;

    TRACE( "%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam );

    info = GetPropW( hWnd, L"MSIDATA" );
    if (!info)
        return 0;

    r = CallWindowProcW( info->oldproc, hWnd, msg, wParam, lParam );

    switch (msg)
    {
    case WM_NCDESTROY:
        for (j = 0; j < info->num_items; j++)
            free( info->items[j] );
        free( info->items );
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
        break;
    }

    return r;
}

static UINT msi_combobox_add_item( MSIRECORD *rec, LPVOID param )
{
    struct msi_combobox_info *info = param;
    LPCWSTR value, text;
    int pos;

    value = MSI_RecordGetString( rec, 3 );
    text = MSI_RecordGetString( rec, 4 );

    info->items[info->addpos_items] = wcsdup( value );

    pos = SendMessageW( info->hwnd, CB_ADDSTRING, 0, (LPARAM)text );
    SendMessageW( info->hwnd, CB_SETITEMDATA, pos, (LPARAM)info->items[info->addpos_items] );
    info->addpos_items++;

    return ERROR_SUCCESS;
}

static UINT msi_combobox_add_items( struct msi_combobox_info *info, LPCWSTR property )
{
    MSIQUERY *view;
    DWORD count;
    UINT r;

    r = MSI_OpenQuery( info->dialog->package->db, &view,
                       L"SELECT * FROM `ComboBox` WHERE `Property` = '%s' ORDER BY `Order`", property );
    if (r != ERROR_SUCCESS)
        return r;

    /* just get the number of records */
    count = 0;
    r = MSI_IterateRecords( view, &count, NULL, NULL );
    if (r != ERROR_SUCCESS)
    {
        msiobj_release( &view->hdr );
        return r;
    }
    info->num_items = count;
    info->items = malloc( sizeof(*info->items) * count );

    r = MSI_IterateRecords( view, NULL, msi_combobox_add_item, info );
    msiobj_release( &view->hdr );
    return r;
}

static UINT msi_dialog_set_control_condition( MSIRECORD *rec, LPVOID param )
{
    msi_dialog *dialog = param;
    msi_control *control;
    LPCWSTR name, action, condition;
    UINT r;

    name = MSI_RecordGetString( rec, 2 );
    action = MSI_RecordGetString( rec, 3 );
    condition = MSI_RecordGetString( rec, 4 );
    r = MSI_EvaluateConditionW( dialog->package, condition );
    control = msi_dialog_find_control( dialog, name );
    if (r == MSICONDITION_TRUE && control)
    {
        TRACE("%s control %s\n", debugstr_w(action), debugstr_w(name));

        /* FIXME: case sensitive? */
        if (!wcscmp( action, L"Hide" ))
            ShowWindow(control->hwnd, SW_HIDE);
        else if (!wcscmp( action, L"Show" ))
            ShowWindow(control->hwnd, SW_SHOW);
        else if (!wcscmp( action, L"Disable" ))
            EnableWindow(control->hwnd, FALSE);
        else if (!wcscmp( action, L"Enable" ))
            EnableWindow(control->hwnd, TRUE);
        else if (!wcscmp( action, L"Default" ))
            SetFocus(control->hwnd);
        else
            FIXME("Unhandled action %s\n", debugstr_w(action));
    }
    return ERROR_SUCCESS;
}

static UINT msi_dialog_evaluate_control_conditions( msi_dialog *dialog )
{
    UINT r;
    MSIQUERY *view;
    MSIPACKAGE *package = dialog->package;

    TRACE("%p %s\n", dialog, debugstr_w(dialog->name));

    /* query the Control table for all the elements of the control */
    r = MSI_OpenQuery( package->db, &view, L"SELECT * FROM `ControlCondition` WHERE `Dialog_` = '%s'", dialog->name );
    if (r != ERROR_SUCCESS)
        return ERROR_SUCCESS;

    r = MSI_IterateRecords( view, 0, msi_dialog_set_control_condition, dialog );
    msiobj_release( &view->hdr );
    return r;
}

static UINT msi_dialog_combobox_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    struct msi_combobox_info *info;
    int index;
    LPWSTR value;

    if (HIWORD(param) != CBN_SELCHANGE && HIWORD(param) != CBN_EDITCHANGE)
        return ERROR_SUCCESS;

    info = GetPropW( control->hwnd, L"MSIDATA" );
    index = SendMessageW( control->hwnd, CB_GETCURSEL, 0, 0 );
    if (index == CB_ERR)
        value = msi_get_window_text( control->hwnd );
    else
        value = (LPWSTR) SendMessageW( control->hwnd, CB_GETITEMDATA, index, 0 );

    msi_dialog_set_property( info->dialog->package, control->property, value );
    msi_dialog_evaluate_control_conditions( info->dialog );

    if (index == CB_ERR)
        free( value );

    return ERROR_SUCCESS;
}

static void msi_dialog_combobox_update( msi_dialog *dialog, msi_control *control )
{
    struct msi_combobox_info *info;
    LPWSTR value, tmp;
    DWORD j;

    info = GetPropW( control->hwnd, L"MSIDATA" );

    value = msi_dup_property( dialog->package->db, control->property );
    if (!value)
    {
        SendMessageW( control->hwnd, CB_SETCURSEL, -1, 0 );
        return;
    }

    for (j = 0; j < info->num_items; j++)
    {
        tmp = (LPWSTR) SendMessageW( control->hwnd, CB_GETITEMDATA, j, 0 );
        if (!wcscmp( value, tmp ))
            break;
    }

    if (j < info->num_items)
    {
        SendMessageW( control->hwnd, CB_SETCURSEL, j, 0 );
    }
    else
    {
        SendMessageW( control->hwnd, CB_SETCURSEL, -1, 0 );
        SetWindowTextW( control->hwnd, value );
    }

    free( value );
}

static UINT msi_dialog_combo_control( msi_dialog *dialog, MSIRECORD *rec )
{
    struct msi_combobox_info *info;
    msi_control *control;
    DWORD attributes, style;
    LPCWSTR prop;

    info = malloc( sizeof *info );
    if (!info)
        return ERROR_FUNCTION_FAILED;

    style = CBS_AUTOHSCROLL | WS_TABSTOP | WS_GROUP | WS_CHILD;
    attributes = MSI_RecordGetInteger( rec, 8 );
    if ( ~attributes & msidbControlAttributesSorted)
        style |= CBS_SORT;
    if ( attributes & msidbControlAttributesComboList)
        style |= CBS_DROPDOWNLIST;
    else
        style |= CBS_DROPDOWN;

    control = msi_dialog_add_control( dialog, rec, WC_COMBOBOXW, style );
    if (!control)
    {
        free( info );
        return ERROR_FUNCTION_FAILED;
    }

    control->handler = msi_dialog_combobox_handler;
    control->update = msi_dialog_combobox_update;

    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    /* subclass */
    info->dialog = dialog;
    info->hwnd = control->hwnd;
    info->items = NULL;
    info->addpos_items = 0;
    info->oldproc = (WNDPROC)SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                                (LONG_PTR)MSIComboBox_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    if (control->property)
        msi_combobox_add_items( info, control->property );

    msi_dialog_combobox_update( dialog, control );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_edit_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    LPWSTR buf;

    if (HIWORD(param) != EN_CHANGE)
        return ERROR_SUCCESS;

    TRACE("edit %s contents changed, set %s\n", debugstr_w(control->name), debugstr_w(control->property));

    buf = msi_get_window_text( control->hwnd );
    msi_dialog_set_property( dialog->package, control->property, buf );
    free( buf );

    return ERROR_SUCCESS;
}

/* length of 2^32 + 1 */
#define MAX_NUM_DIGITS 11

static UINT msi_dialog_edit_control( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop, text;
    LPWSTR val, begin, end;
    WCHAR num[MAX_NUM_DIGITS];
    DWORD limit;

    control = msi_dialog_add_control( dialog, rec, L"Edit",
                                      WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL );
    control->handler = msi_dialog_edit_handler;

    text = MSI_RecordGetString( rec, 10 );
    if ( text )
    {
        begin = wcschr( text, '{' );
        end = wcschr( text, '}' );

        if ( begin && end && end > begin &&
             begin[0] >= '0' && begin[0] <= '9' &&
             end - begin < MAX_NUM_DIGITS)
        {
            lstrcpynW( num, begin + 1, end - begin );
            limit = wcstol( num, NULL, 10 );

            SendMessageW( control->hwnd, EM_SETLIMITTEXT, limit, 0 );
        }
    }

    prop = MSI_RecordGetString( rec, 9 );
    if( prop )
        control->property = wcsdup( prop );

    val = msi_dup_property( dialog->package->db, control->property );
    SetWindowTextW( control->hwnd, val );
    free( val );
    return ERROR_SUCCESS;
}

/******************** Masked Edit ********************************************/

#define MASK_MAX_GROUPS 20

struct msi_mask_group
{
    UINT len;
    UINT ofs;
    WCHAR type;
    HWND hwnd;
};

struct msi_maskedit_info
{
    msi_dialog *dialog;
    WNDPROC oldproc;
    HWND hwnd;
    LPWSTR prop;
    UINT num_chars;
    UINT num_groups;
    struct msi_mask_group group[MASK_MAX_GROUPS];
};

static BOOL msi_mask_editable( WCHAR type )
{
    switch (type)
    {
    case '%':
    case '#':
    case '&':
    case '`':
    case '?':
    case '^':
        return TRUE;
    }
    return FALSE;
}

static void msi_mask_control_change( struct msi_maskedit_info *info )
{
    LPWSTR val;
    UINT i, n, r;

    val = malloc( (info->num_chars + 1) * sizeof(WCHAR) );
    for( i=0, n=0; i<info->num_groups; i++ )
    {
        if (info->group[i].len == ~0u)
        {
            UINT len = SendMessageW( info->group[i].hwnd, WM_GETTEXTLENGTH, 0, 0 );
            val = realloc( val, (len + 1) * sizeof(WCHAR) );
            GetWindowTextW( info->group[i].hwnd, val, len + 1 );
        }
        else
        {
            if (info->group[i].len + n > info->num_chars)
            {
                ERR("can't fit control %d text into template\n",i);
                break;
            }
            if (!msi_mask_editable(info->group[i].type))
            {
                for(r=0; r<info->group[i].len; r++)
                    val[n+r] = info->group[i].type;
                val[n+r] = 0;
            }
            else
            {
                r = GetWindowTextW( info->group[i].hwnd, &val[n], info->group[i].len+1 );
                if( r != info->group[i].len )
                    break;
            }
            n += r;
        }
    }

    TRACE("%d/%d controls were good\n", i, info->num_groups);

    if( i == info->num_groups )
    {
        TRACE("Set property %s to %s\n", debugstr_w(info->prop), debugstr_w(val));
        msi_dialog_set_property( info->dialog->package, info->prop, val );
        msi_dialog_evaluate_control_conditions( info->dialog );
    }
    free( val );
}

/* now move to the next control if necessary */
static VOID msi_mask_next_control( struct msi_maskedit_info *info, HWND hWnd )
{
    HWND hWndNext;
    UINT len, i;

    for( i=0; i<info->num_groups; i++ )
        if( info->group[i].hwnd == hWnd )
            break;

    /* don't move from the last control */
    if( i >= (info->num_groups-1) )
        return;

    len = SendMessageW( hWnd, WM_GETTEXTLENGTH, 0, 0 );
    if( len < info->group[i].len )
        return;

    hWndNext = GetNextDlgTabItem( GetParent( hWnd ), hWnd, FALSE );
    SetFocus( hWndNext );
}

static LRESULT WINAPI
MSIMaskedEdit_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_maskedit_info *info;
    HRESULT r;

    TRACE("%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam);

    info = GetPropW(hWnd, L"MSIDATA");

    r = CallWindowProcW(info->oldproc, hWnd, msg, wParam, lParam);

    switch( msg )
    {
    case WM_COMMAND:
        if (HIWORD(wParam) == EN_CHANGE)
        {
            msi_mask_control_change( info );
            msi_mask_next_control( info, (HWND) lParam );
        }
        break;
    case WM_NCDESTROY:
        free( info->prop );
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
        break;
    }

    return r;
}

/* fish the various bits of the property out and put them in the control */
static void
msi_maskedit_set_text( struct msi_maskedit_info *info, LPCWSTR text )
{
    LPCWSTR p;
    UINT i;

    p = text;
    for( i = 0; i < info->num_groups; i++ )
    {
        if( info->group[i].len < lstrlenW( p ) )
        {
            WCHAR *chunk = wcsdup( p );
            chunk[ info->group[i].len ] = 0;
            SetWindowTextW( info->group[i].hwnd, chunk );
            free( chunk );
        }
        else
        {
            SetWindowTextW( info->group[i].hwnd, p );
            break;
        }
        p += info->group[i].len;
    }
}

static struct msi_maskedit_info * msi_dialog_parse_groups( LPCWSTR mask )
{
    struct msi_maskedit_info *info;
    int i = 0, n = 0, total = 0;
    LPCWSTR p;

    TRACE("masked control, template %s\n", debugstr_w(mask));

    if( !mask )
        return NULL;

    info = calloc( 1, sizeof *info );
    if( !info )
        return info;

    p = wcschr(mask, '<');
    if( p )
        p++;
    else
        p = mask;

    for( i=0; i<MASK_MAX_GROUPS; i++ )
    {
        /* stop at the end of the string */
        if( p[0] == 0 || p[0] == '>' )
        {
            if (!total)
            {
                /* create a group for the empty mask */
                info->group[0].type = '&';
                info->group[0].len = ~0u;
                i = 1;
            }
            break;
        }

        /* count the number of the same identifier */
        for( n=0; p[n] == p[0]; n++ )
            ;
        info->group[i].ofs = total;
        info->group[i].type = p[0];
        if( p[n] == '=' )
        {
            n++;
            total++; /* an extra not part of the group */
        }
        info->group[i].len = n;
        total += n;
        p += n;
    }

    TRACE("%d characters in %d groups\n", total, i );
    if( i == MASK_MAX_GROUPS )
        ERR("too many groups in PIDTemplate %s\n", debugstr_w(mask));

    info->num_chars = total;
    info->num_groups = i;

    return info;
}

static void
msi_maskedit_create_children( struct msi_maskedit_info *info, LPCWSTR font )
{
    DWORD width, height, style, wx, ww;
    RECT rect;
    HWND hwnd;
    UINT i;

    style = WS_CHILD | WS_BORDER | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;

    GetClientRect( info->hwnd, &rect );

    width = rect.right - rect.left;
    height = rect.bottom - rect.top;

    for( i = 0; i < info->num_groups; i++ )
    {
        if (!msi_mask_editable( info->group[i].type ))
            continue;
        if (info->num_chars)
        {
            wx = (info->group[i].ofs * width) / info->num_chars;
            ww = (info->group[i].len * width) / info->num_chars;
        }
        else
        {
            wx = 0;
            ww = width;
        }
        hwnd = CreateWindowW( L"Edit", NULL, style, wx, 0, ww, height,
                              info->hwnd, NULL, NULL, NULL );
        if( !hwnd )
        {
            ERR("failed to create mask edit sub window\n");
            break;
        }

        SendMessageW( hwnd, EM_LIMITTEXT, info->group[i].len, 0 );

        msi_dialog_set_font( info->dialog, hwnd,
                             font?font:info->dialog->default_font );
        info->group[i].hwnd = hwnd;
    }
}

/*
 * office 2003 uses "73931<````=````=````=````=`````>@@@@@"
 * delphi 7 uses "<????-??????-??????-????>" and "<???-???>"
 * filemaker pro 7 uses "<^^^^=^^^^=^^^^=^^^^=^^^^=^^^^=^^^^^>"
 */
static UINT msi_dialog_maskedit_control( msi_dialog *dialog, MSIRECORD *rec )
{
    LPWSTR font_mask, val = NULL, font;
    struct msi_maskedit_info *info = NULL;
    UINT ret = ERROR_SUCCESS;
    msi_control *control;
    LPCWSTR prop, mask;

    TRACE("\n");

    font_mask = msi_get_deformatted_field( dialog->package, rec, 10 );
    font = msi_dialog_get_style( font_mask, &mask );
    if( !mask )
    {
        WARN("mask template is empty\n");
        goto end;
    }

    info = msi_dialog_parse_groups( mask );
    if( !info )
    {
        ERR("template %s is invalid\n", debugstr_w(mask));
        goto end;
    }

    info->dialog = dialog;

    control = msi_dialog_add_control( dialog, rec, L"Static",
                   SS_OWNERDRAW | WS_GROUP | WS_VISIBLE );
    if( !control )
    {
        ERR("Failed to create maskedit container\n");
        ret = ERROR_FUNCTION_FAILED;
        goto end;
    }
    SetWindowLongPtrW( control->hwnd, GWL_EXSTYLE, WS_EX_CONTROLPARENT );

    info->hwnd = control->hwnd;

    /* subclass the static control */
    info->oldproc = (WNDPROC) SetWindowLongPtrW( info->hwnd, GWLP_WNDPROC,
                                          (LONG_PTR)MSIMaskedEdit_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    prop = MSI_RecordGetString( rec, 9 );
    if( prop )
        info->prop = wcsdup( prop );

    msi_maskedit_create_children( info, font );

    if( prop )
    {
        val = msi_dup_property( dialog->package->db, prop );
        if( val )
        {
            msi_maskedit_set_text( info, val );
            free( val );
        }
    }

end:
    if( ret != ERROR_SUCCESS )
        free( info );
    free( font_mask );
    free( font );
    return ret;
}

/******************** Progress Bar *****************************************/

static UINT msi_dialog_progress_bar( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    DWORD attributes, style;

    style = WS_VISIBLE;
    attributes = MSI_RecordGetInteger( rec, 8 );
    if( !(attributes & msidbControlAttributesProgress95) )
        style |= PBS_SMOOTH;

    control = msi_dialog_add_control( dialog, rec, PROGRESS_CLASSW, style );
    if( !control )
        return ERROR_FUNCTION_FAILED;

    event_subscribe( dialog, L"SetProgress", control->name, L"Progress" );
    return ERROR_SUCCESS;
}

/******************** Path Edit ********************************************/

struct msi_pathedit_info
{
    msi_dialog *dialog;
    msi_control *control;
    WNDPROC oldproc;
};

static WCHAR *get_path_property( msi_dialog *dialog, msi_control *control )
{
    WCHAR *prop, *path;
    BOOL indirect = control->attributes & msidbControlAttributesIndirect;
    if (!(prop = msi_dialog_dup_property( dialog, control->property, indirect ))) return NULL;
    path = msi_dialog_dup_property( dialog, prop, TRUE );
    free( prop );
    return path;
}

static void msi_dialog_update_pathedit( msi_dialog *dialog, msi_control *control )
{
    WCHAR *path;

    if (!control && !(control = msi_dialog_find_control_by_type( dialog, L"PathEdit" )))
       return;

    if (!(path = get_path_property( dialog, control ))) return;
    SetWindowTextW( control->hwnd, path );
    SendMessageW( control->hwnd, EM_SETSEL, 0, -1 );
    free( path );
}

/* FIXME: test when this should fail */
static BOOL msi_dialog_verify_path( LPWSTR path )
{
    if ( !path[0] )
        return FALSE;

    if ( PathIsRelativeW( path ) )
        return FALSE;

    return TRUE;
}

/* returns TRUE if the path is valid, FALSE otherwise */
static BOOL msi_dialog_onkillfocus( msi_dialog *dialog, msi_control *control )
{
    LPWSTR buf, prop;
    BOOL indirect;
    BOOL valid;

    indirect = control->attributes & msidbControlAttributesIndirect;
    prop = msi_dialog_dup_property( dialog, control->property, indirect );

    buf = msi_get_window_text( control->hwnd );

    if ( !msi_dialog_verify_path( buf ) )
    {
        /* FIXME: display an error message box */
        ERR("Invalid path %s\n", debugstr_w( buf ));
        valid = FALSE;
        SetFocus( control->hwnd );
    }
    else
    {
        valid = TRUE;
        msi_dialog_set_property( dialog->package, prop, buf );
    }

    msi_dialog_update_pathedit( dialog, control );

    TRACE("edit %s contents changed, set %s\n", debugstr_w(control->name),
          debugstr_w(prop));

    free( buf );
    free( prop );

    return valid;
}

static LRESULT WINAPI MSIPathEdit_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_pathedit_info *info = GetPropW(hWnd, L"MSIDATA");
    LRESULT r = 0;

    TRACE("%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam);

    if ( msg == WM_KILLFOCUS )
    {
        /* if the path is invalid, don't handle this message */
        if ( !msi_dialog_onkillfocus( info->dialog, info->control ) )
            return 0;
    }

    r = CallWindowProcW(info->oldproc, hWnd, msg, wParam, lParam);

    if ( msg == WM_NCDESTROY )
    {
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
    }

    return r;
}

static UINT msi_dialog_pathedit_control( msi_dialog *dialog, MSIRECORD *rec )
{
    struct msi_pathedit_info *info;
    msi_control *control;
    LPCWSTR prop;

    info = malloc( sizeof *info );
    if (!info)
        return ERROR_FUNCTION_FAILED;

    control = msi_dialog_add_control( dialog, rec, L"Edit",
                                      WS_BORDER | WS_TABSTOP );
    control->attributes = MSI_RecordGetInteger( rec, 8 );
    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );
    control->update = msi_dialog_update_pathedit;

    info->dialog = dialog;
    info->control = control;
    info->oldproc = (WNDPROC) SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                                 (LONG_PTR)MSIPathEdit_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    msi_dialog_update_pathedit( dialog, control );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_radiogroup_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    if (HIWORD(param) != BN_CLICKED)
        return ERROR_SUCCESS;

    TRACE("clicked radio button %s, set %s\n", debugstr_w(control->name), debugstr_w(control->property));

    msi_dialog_set_property( dialog->package, control->property, control->name );

    return msi_dialog_button_handler( dialog, control, param );
}

/* radio buttons are a bit different from normal controls */
static UINT msi_dialog_create_radiobutton( MSIRECORD *rec, LPVOID param )
{
    radio_button_group_descr *group = param;
    msi_dialog *dialog = group->dialog;
    msi_control *control;
    LPCWSTR prop, text, name;
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_MULTILINE;

    name = MSI_RecordGetString( rec, 3 );
    text = MSI_RecordGetString( rec, 8 );

    control = dialog_create_window( dialog, rec, 0, L"BUTTON", name, text, style,
                                    group->parent->hwnd );
    if (!control)
        return ERROR_FUNCTION_FAILED;
    control->handler = msi_dialog_radiogroup_handler;

    if (group->propval && !wcscmp( control->name, group->propval ))
        SendMessageW(control->hwnd, BM_SETCHECK, BST_CHECKED, 0);

    prop = MSI_RecordGetString( rec, 1 );
    if( prop )
        control->property = wcsdup( prop );

    return ERROR_SUCCESS;
}

static BOOL CALLBACK msi_radioground_child_enum( HWND hWnd, LPARAM lParam )
{
    EnableWindow( hWnd, lParam );
    return TRUE;
}

static LRESULT WINAPI MSIRadioGroup_WndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    WNDPROC oldproc = (WNDPROC)GetPropW( hWnd, L"MSIDATA" );
    LRESULT r;

    TRACE( "hWnd %p msg %04x wParam %#Ix lParam %#Ix\n", hWnd, msg, wParam, lParam );

    if (msg == WM_COMMAND) /* Forward notifications to dialog */
        SendMessageW( GetParent( hWnd ), msg, wParam, lParam );

    r = CallWindowProcW( oldproc, hWnd, msg, wParam, lParam );

    /* make sure the radio buttons show as disabled if the parent is disabled */
    if (msg == WM_ENABLE)
        EnumChildWindows( hWnd, msi_radioground_child_enum, wParam );

    return r;
}

static UINT msi_dialog_radiogroup_control( msi_dialog *dialog, MSIRECORD *rec )
{
    UINT r;
    LPCWSTR prop;
    msi_control *control;
    MSIQUERY *view;
    radio_button_group_descr group;
    MSIPACKAGE *package = dialog->package;
    WNDPROC oldproc;
    DWORD attr, style = WS_GROUP;

    prop = MSI_RecordGetString( rec, 9 );

    TRACE("%p %p %s\n", dialog, rec, debugstr_w( prop ));

    attr = MSI_RecordGetInteger( rec, 8 );
    if (attr & msidbControlAttributesVisible)
        style |= WS_VISIBLE;
    if (~attr & msidbControlAttributesEnabled)
        style |= WS_DISABLED;
    if (attr & msidbControlAttributesHasBorder)
        style |= BS_GROUPBOX;
    else
        style |= BS_OWNERDRAW;

    /* Create parent group box to hold radio buttons */
    control = msi_dialog_add_control( dialog, rec, L"BUTTON", style );
    if( !control )
        return ERROR_FUNCTION_FAILED;

    oldproc = (WNDPROC) SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                           (LONG_PTR)MSIRadioGroup_WndProc );
    SetPropW(control->hwnd, L"MSIDATA", oldproc);
    SetWindowLongPtrW( control->hwnd, GWL_EXSTYLE, WS_EX_CONTROLPARENT );

    if( prop )
        control->property = wcsdup( prop );

    /* query the Radio Button table for all control in this group */
    r = MSI_OpenQuery( package->db, &view, L"SELECT * FROM `RadioButton` WHERE `Property` = '%s'", prop );
    if( r != ERROR_SUCCESS )
    {
        ERR("query failed for dialog %s radio group %s\n",
            debugstr_w(dialog->name), debugstr_w(prop));
        return ERROR_INVALID_PARAMETER;
    }

    group.dialog = dialog;
    group.parent = control;
    group.propval = msi_dup_property( dialog->package->db, control->property );

    r = MSI_IterateRecords( view, 0, msi_dialog_create_radiobutton, &group );
    msiobj_release( &view->hdr );
    free( group.propval );
    return r;
}

static void
msi_seltree_sync_item_state( HWND hwnd, MSIFEATURE *feature, HTREEITEM hItem )
{
    TVITEMW tvi;
    DWORD index = feature->ActionRequest;

    TRACE("Feature %s -> %d %d %d\n", debugstr_w(feature->Title),
        feature->Installed, feature->Action, feature->ActionRequest);

    if (index == INSTALLSTATE_UNKNOWN)
        index = INSTALLSTATE_ABSENT;

    tvi.mask = TVIF_STATE;
    tvi.hItem = hItem;
    tvi.state = INDEXTOSTATEIMAGEMASK( index );
    tvi.stateMask = TVIS_STATEIMAGEMASK;

    SendMessageW( hwnd, TVM_SETITEMW, 0, (LPARAM) &tvi );
}

static UINT
msi_seltree_popup_menu( HWND hwnd, INT x, INT y )
{
    HMENU hMenu;
    INT r;

    /* create a menu to display */
    hMenu = CreatePopupMenu();

    /* FIXME: load strings from resources */
    AppendMenuA( hMenu, MF_ENABLED, INSTALLSTATE_LOCAL, "Install feature locally");
    AppendMenuA( hMenu, MF_ENABLED, USER_INSTALLSTATE_ALL, "Install entire feature");
    AppendMenuA( hMenu, MF_ENABLED, INSTALLSTATE_ADVERTISED, "Install on demand");
    AppendMenuA( hMenu, MF_ENABLED, INSTALLSTATE_ABSENT, "Don't install");
    r = TrackPopupMenu( hMenu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD,
                        x, y, 0, hwnd, NULL );
    DestroyMenu( hMenu );
    return r;
}

static void
msi_seltree_update_feature_installstate( HWND hwnd, HTREEITEM hItem,
        MSIPACKAGE *package, MSIFEATURE *feature, INSTALLSTATE state )
{
    feature->ActionRequest = state;
    msi_seltree_sync_item_state( hwnd, feature, hItem );
    ACTION_UpdateComponentStates( package, feature );
}

static void
msi_seltree_update_siblings_and_children_installstate( HWND hwnd, HTREEITEM curr,
        MSIPACKAGE *package, INSTALLSTATE state)
{
    /* update all siblings */
    do
    {
        MSIFEATURE *feature;
        HTREEITEM child;

        feature = msi_seltree_feature_from_item( hwnd, curr );
        msi_seltree_update_feature_installstate( hwnd, curr, package, feature, state );

        /* update this sibling's children */
        child = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, (WPARAM)TVGN_CHILD, (LPARAM)curr );
        if (child)
            msi_seltree_update_siblings_and_children_installstate( hwnd, child,
                    package, state );
    }
    while ((curr = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, (WPARAM)TVGN_NEXT, (LPARAM)curr )));
}

static LRESULT
msi_seltree_menu( HWND hwnd, HTREEITEM hItem )
{
    struct msi_selection_tree_info *info;
    MSIFEATURE *feature;
    MSIPACKAGE *package;
    union {
        RECT rc;
        POINT pt[2];
        HTREEITEM hItem;
    } u;
    UINT r;

    info = GetPropW(hwnd, L"MSIDATA");
    package = info->dialog->package;

    feature = msi_seltree_feature_from_item( hwnd, hItem );
    if (!feature)
    {
        ERR("item %p feature was NULL\n", hItem);
        return 0;
    }

    /* get the item's rectangle to put the menu just below it */
    u.hItem = hItem;
    SendMessageW( hwnd, TVM_GETITEMRECT, 0, (LPARAM) &u.rc );
    MapWindowPoints( hwnd, NULL, u.pt, 2 );

    r = msi_seltree_popup_menu( hwnd, u.rc.left, u.rc.top );

    switch (r)
    {
    case USER_INSTALLSTATE_ALL:
        r = INSTALLSTATE_LOCAL;
        /* fall-through */
    case INSTALLSTATE_ADVERTISED:
    case INSTALLSTATE_ABSENT:
        {
            HTREEITEM child;
            child = (HTREEITEM)SendMessageW( hwnd, TVM_GETNEXTITEM, (WPARAM)TVGN_CHILD, (LPARAM)hItem );
            if (child)
                msi_seltree_update_siblings_and_children_installstate( hwnd, child, package, r );
        }
        /* fall-through */
    case INSTALLSTATE_LOCAL:
        msi_seltree_update_feature_installstate( hwnd, hItem, package, feature, r );
        break;
    }

    return 0;
}

static LRESULT WINAPI
MSISelectionTree_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_selection_tree_info *info;
    TVHITTESTINFO tvhti;
    HRESULT r;

    TRACE("%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam);

    info = GetPropW(hWnd, L"MSIDATA");

    switch( msg )
    {
    case WM_LBUTTONDOWN:
        tvhti.pt.x = (short)LOWORD( lParam );
        tvhti.pt.y = (short)HIWORD( lParam );
        tvhti.flags = 0;
        tvhti.hItem = 0;
        CallWindowProcW(info->oldproc, hWnd, TVM_HITTEST, 0, (LPARAM) &tvhti );
        if (tvhti.flags & TVHT_ONITEMSTATEICON)
            return msi_seltree_menu( hWnd, tvhti.hItem );
        break;
    }
    r = CallWindowProcW(info->oldproc, hWnd, msg, wParam, lParam);

    switch( msg )
    {
    case WM_NCDESTROY:
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
        break;
    }
    return r;
}

static void
msi_seltree_add_child_features( MSIPACKAGE *package, HWND hwnd,
                                LPCWSTR parent, HTREEITEM hParent )
{
    struct msi_selection_tree_info *info = GetPropW( hwnd, L"MSIDATA" );
    MSIFEATURE *feature;
    TVINSERTSTRUCTW tvis;
    HTREEITEM hitem, hfirst = NULL;

    LIST_FOR_EACH_ENTRY( feature, &package->features, MSIFEATURE, entry )
    {
        if ( parent && feature->Feature_Parent && wcscmp( parent, feature->Feature_Parent ))
            continue;
        else if ( parent && !feature->Feature_Parent )
            continue;
        else if ( !parent && feature->Feature_Parent )
            continue;

        if ( !feature->Title )
            continue;

        if ( !feature->Display )
            continue;

        memset( &tvis, 0, sizeof tvis );
        tvis.hParent = hParent;
        tvis.hInsertAfter = TVI_LAST;
        tvis.u.item.mask = TVIF_TEXT | TVIF_PARAM;
        tvis.u.item.pszText = feature->Title;
        tvis.u.item.lParam = (LPARAM) feature;

        hitem = (HTREEITEM) SendMessageW( hwnd, TVM_INSERTITEMW, 0, (LPARAM) &tvis );
        if (!hitem)
            continue;

        if (!hfirst)
            hfirst = hitem;

        msi_seltree_sync_item_state( hwnd, feature, hitem );
        msi_seltree_add_child_features( package, hwnd,
                                        feature->Feature, hitem );

        /* the node is expanded if Display is odd */
        if ( feature->Display % 2 != 0 )
            SendMessageW( hwnd, TVM_EXPAND, TVE_EXPAND, (LPARAM) hitem );
    }

    /* select the first item */
    SendMessageW( hwnd, TVM_SELECTITEM, TVGN_CARET | TVGN_DROPHILITE, (LPARAM) hfirst );
    info->selected = hfirst;
}

static void msi_seltree_create_imagelist( HWND hwnd )
{
    const int bm_width = 32, bm_height = 16, bm_count = 3;
    const int bm_resource = 0x1001;
    HIMAGELIST himl;
    int i;
    HBITMAP hbmp;

    himl = ImageList_Create( bm_width, bm_height, FALSE, 4, 0 );
    if (!himl)
    {
        ERR("failed to create image list\n");
        return;
    }

    for (i=0; i<bm_count; i++)
    {
        hbmp = LoadBitmapW( msi_hInstance, MAKEINTRESOURCEW(i+bm_resource) );
        if (!hbmp)
        {
            ERR("failed to load bitmap %d\n", i);
            break;
        }

        /*
         * Add a dummy bitmap at offset zero because the treeview
         * can't use it as a state mask (zero means no user state).
         */
        if (!i)
            ImageList_Add( himl, hbmp, NULL );

        ImageList_Add( himl, hbmp, NULL );
    }

    SendMessageW( hwnd, TVM_SETIMAGELIST, TVSIL_STATE, (LPARAM)himl );
}

static UINT msi_dialog_seltree_handler( msi_dialog *dialog,
                                        msi_control *control, WPARAM param )
{
    struct msi_selection_tree_info *info = GetPropW( control->hwnd, L"MSIDATA" );
    LPNMTREEVIEWW tv = (LPNMTREEVIEWW)param;
    MSIRECORD *row, *rec;
    MSIFOLDER *folder;
    MSIFEATURE *feature;
    LPCWSTR dir, title = NULL;
    UINT r = ERROR_SUCCESS;

    if (tv->hdr.code != TVN_SELCHANGINGW)
        return ERROR_SUCCESS;

    info->selected = tv->itemNew.hItem;

    if (!(tv->itemNew.mask & TVIF_TEXT))
    {
        feature = msi_seltree_feature_from_item( control->hwnd, tv->itemNew.hItem );
        if (feature)
            title = feature->Title;
    }
    else
        title = tv->itemNew.pszText;

    row = MSI_QueryGetRecord( dialog->package->db, L"SELECT * FROM `Feature` WHERE `Title` = '%s'", title );
    if (!row)
        return ERROR_FUNCTION_FAILED;

    rec = MSI_CreateRecord( 1 );

    MSI_RecordSetStringW( rec, 1, MSI_RecordGetString( row, 4 ) );
    msi_event_fire( dialog->package, L"SelectionDescription", rec );

    dir = MSI_RecordGetString( row, 7 );
    if (dir)
    {
        folder = msi_get_loaded_folder( dialog->package, dir );
        if (!folder)
        {
            r = ERROR_FUNCTION_FAILED;
            goto done;
        }
        MSI_RecordSetStringW( rec, 1, folder->ResolvedTarget );
    }
    else
        MSI_RecordSetStringW( rec, 1, NULL );

    msi_event_fire( dialog->package, L"SelectionPath", rec );

done:
    msiobj_release(&row->hdr);
    msiobj_release(&rec->hdr);

    return r;
}

static UINT msi_dialog_selection_tree( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop, control_name;
    MSIPACKAGE *package = dialog->package;
    DWORD style;
    struct msi_selection_tree_info *info;

    info = malloc( sizeof *info );
    if (!info)
        return ERROR_FUNCTION_FAILED;

    /* create the treeview control */
    style = TVS_HASLINES | TVS_HASBUTTONS | TVS_LINESATROOT;
    style |= WS_GROUP | WS_VSCROLL | WS_TABSTOP;
    control = msi_dialog_add_control( dialog, rec, WC_TREEVIEWW, style );
    if (!control)
    {
        free(info);
        return ERROR_FUNCTION_FAILED;
    }

    control->handler = msi_dialog_seltree_handler;
    control_name = MSI_RecordGetString( rec, 2 );
    control->attributes = MSI_RecordGetInteger( rec, 8 );
    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    /* subclass */
    info->dialog = dialog;
    info->hwnd = control->hwnd;
    info->oldproc = (WNDPROC) SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                          (LONG_PTR)MSISelectionTree_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    event_subscribe( dialog, L"SelectionPath", control_name, L"Property" );

    /* initialize it */
    msi_seltree_create_imagelist( control->hwnd );
    msi_seltree_add_child_features( package, control->hwnd, NULL, NULL );

    return ERROR_SUCCESS;
}

/******************** Group Box ***************************************/

static UINT msi_dialog_group_box( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    DWORD style;

    style = BS_GROUPBOX | WS_CHILD | WS_GROUP;
    control = msi_dialog_add_control( dialog, rec, WC_BUTTONW, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    return ERROR_SUCCESS;
}

/******************** List Box ***************************************/

struct msi_listbox_info
{
    msi_dialog *dialog;
    HWND hwnd;
    WNDPROC oldproc;
    DWORD num_items;
    DWORD addpos_items;
    LPWSTR *items;
};

static LRESULT WINAPI MSIListBox_WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    struct msi_listbox_info *info;
    LRESULT r;
    DWORD j;

    TRACE("%p %04x %#Ix %#Ix\n", hWnd, msg, wParam, lParam);

    info = GetPropW( hWnd, L"MSIDATA" );
    if (!info)
        return 0;

    r = CallWindowProcW( info->oldproc, hWnd, msg, wParam, lParam );

    switch( msg )
    {
    case WM_NCDESTROY:
        for (j = 0; j < info->num_items; j++)
            free( info->items[j] );
        free( info->items );
        free( info );
        RemovePropW( hWnd, L"MSIDATA" );
        break;
    }

    return r;
}

static UINT msi_listbox_add_item( MSIRECORD *rec, LPVOID param )
{
    struct msi_listbox_info *info = param;
    LPCWSTR value, text;
    int pos;

    value = MSI_RecordGetString( rec, 3 );
    text = MSI_RecordGetString( rec, 4 );

    info->items[info->addpos_items] = wcsdup( value );

    pos = SendMessageW( info->hwnd, LB_ADDSTRING, 0, (LPARAM)text );
    SendMessageW( info->hwnd, LB_SETITEMDATA, pos, (LPARAM)info->items[info->addpos_items] );
    info->addpos_items++;
    return ERROR_SUCCESS;
}

static UINT msi_listbox_add_items( struct msi_listbox_info *info, LPCWSTR property )
{
    MSIQUERY *view;
    DWORD count;
    UINT r;

    r = MSI_OpenQuery( info->dialog->package->db, &view,
                       L"SELECT * FROM `ListBox` WHERE `Property` = '%s' ORDER BY `Order`", property );
    if ( r != ERROR_SUCCESS )
        return r;

    /* just get the number of records */
    count = 0;
    r = MSI_IterateRecords( view, &count, NULL, NULL );
    if (r != ERROR_SUCCESS)
    {
        msiobj_release( &view->hdr );
        return r;
    }
    info->num_items = count;
    info->items = malloc( sizeof(*info->items) * count );

    r = MSI_IterateRecords( view, NULL, msi_listbox_add_item, info );
    msiobj_release( &view->hdr );
    return r;
}

static UINT msi_dialog_listbox_handler( msi_dialog *dialog,
                                        msi_control *control, WPARAM param )
{
    struct msi_listbox_info *info;
    int index;
    LPCWSTR value;

    if( HIWORD(param) != LBN_SELCHANGE )
        return ERROR_SUCCESS;

    info = GetPropW( control->hwnd, L"MSIDATA" );
    index = SendMessageW( control->hwnd, LB_GETCURSEL, 0, 0 );
    value = (LPCWSTR) SendMessageW( control->hwnd, LB_GETITEMDATA, index, 0 );

    msi_dialog_set_property( info->dialog->package, control->property, value );
    msi_dialog_evaluate_control_conditions( info->dialog );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_list_box( msi_dialog *dialog, MSIRECORD *rec )
{
    struct msi_listbox_info *info;
    msi_control *control;
    DWORD attributes, style;
    LPCWSTR prop;

    info = malloc( sizeof *info );
    if (!info)
        return ERROR_FUNCTION_FAILED;

    style = WS_TABSTOP | WS_GROUP | WS_CHILD | LBS_NOTIFY | WS_VSCROLL | WS_BORDER;
    attributes = MSI_RecordGetInteger( rec, 8 );
    if (~attributes & msidbControlAttributesSorted)
        style |= LBS_SORT;

    control = msi_dialog_add_control( dialog, rec, WC_LISTBOXW, style );
    if (!control)
    {
        free(info);
        return ERROR_FUNCTION_FAILED;
    }

    control->handler = msi_dialog_listbox_handler;

    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    /* subclass */
    info->dialog = dialog;
    info->hwnd = control->hwnd;
    info->items = NULL;
    info->addpos_items = 0;
    info->oldproc = (WNDPROC)SetWindowLongPtrW( control->hwnd, GWLP_WNDPROC,
                                                (LONG_PTR)MSIListBox_WndProc );
    SetPropW( control->hwnd, L"MSIDATA", info );

    if ( control->property )
        msi_listbox_add_items( info, control->property );

    return ERROR_SUCCESS;
}

/******************** Directory Combo ***************************************/

static void msi_dialog_update_directory_combo( msi_dialog *dialog, msi_control *control )
{
    WCHAR *path;

    if (!control && !(control = msi_dialog_find_control_by_type( dialog, L"DirectoryCombo" )))
        return;

    if (!(path = get_path_property( dialog, control ))) return;
    PathStripPathW( path );
    PathRemoveBackslashW( path );

    SendMessageW( control->hwnd, CB_INSERTSTRING, 0, (LPARAM)path );
    SendMessageW( control->hwnd, CB_SETCURSEL, 0, 0 );

    free( path );
}

static UINT msi_dialog_directory_combo( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop;
    DWORD style;

    /* FIXME: use CBS_OWNERDRAWFIXED and add owner draw code */
    style = CBS_DROPDOWNLIST | CBS_HASSTRINGS | WS_CHILD |
            WS_GROUP | WS_TABSTOP | WS_VSCROLL;
    control = msi_dialog_add_control( dialog, rec, WC_COMBOBOXW, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    control->attributes = MSI_RecordGetInteger( rec, 8 );
    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    msi_dialog_update_directory_combo( dialog, control );

    return ERROR_SUCCESS;
}

/******************** Directory List ***************************************/

static void msi_dialog_update_directory_list( msi_dialog *dialog, msi_control *control )
{
    WCHAR dir_spec[MAX_PATH], *path;
    WIN32_FIND_DATAW wfd;
    LVITEMW item;
    HANDLE file;

    if (!control && !(control = msi_dialog_find_control_by_type( dialog, L"DirectoryList" )))
        return;

    /* clear the list-view */
    SendMessageW( control->hwnd, LVM_DELETEALLITEMS, 0, 0 );

    if (!(path = get_path_property( dialog, control ))) return;
    lstrcpyW( dir_spec, path );
    lstrcatW( dir_spec, L"*" );

    file = FindFirstFileW( dir_spec, &wfd );
    if (file == INVALID_HANDLE_VALUE)
    {
        free( path );
        return;
    }

    do
    {
        if ( wfd.dwFileAttributes != FILE_ATTRIBUTE_DIRECTORY )
            continue;

        if ( !wcscmp( wfd.cFileName, L"." ) || !wcscmp( wfd.cFileName, L".." ) )
            continue;

        item.mask = LVIF_TEXT;
        item.cchTextMax = MAX_PATH;
        item.iItem = 0;
        item.iSubItem = 0;
        item.pszText = wfd.cFileName;

        SendMessageW( control->hwnd, LVM_INSERTITEMW, 0, (LPARAM)&item );
    } while ( FindNextFileW( file, &wfd ) );

    free( path );
    FindClose( file );
}

static UINT msi_dialog_directorylist_up( msi_dialog *dialog )
{
    msi_control *control;
    LPWSTR prop, path, ptr;
    BOOL indirect;

    control = msi_dialog_find_control_by_type( dialog, L"DirectoryList" );
    indirect = control->attributes & msidbControlAttributesIndirect;
    prop = msi_dialog_dup_property( dialog, control->property, indirect );
    path = msi_dialog_dup_property( dialog, prop, TRUE );

    /* strip off the last directory */
    ptr = PathFindFileNameW( path );
    if (ptr != path) *(ptr - 1) = '\0';
    PathAddBackslashW( path );

    msi_dialog_set_property( dialog->package, prop, path );

    msi_dialog_update_directory_list( dialog, NULL );
    msi_dialog_update_directory_combo( dialog, NULL );
    msi_dialog_update_pathedit( dialog, NULL );

    free( path );
    free( prop );

    return ERROR_SUCCESS;
}

static WCHAR *get_unique_folder_name( const WCHAR *root, int *ret_len )
{
    WCHAR newfolder[MAX_PATH], *path, *ptr;
    int len, count = 2;

    len = LoadStringW( msi_hInstance, IDS_NEWFOLDER, newfolder, ARRAY_SIZE(newfolder) );
    len += lstrlenW(root) + 1;
    if (!(path = malloc( (len + 4) * sizeof(WCHAR) ))) return NULL;
    lstrcpyW( path, root );
    lstrcatW( path, newfolder );

    for (;;)
    {
        if (GetFileAttributesW( path ) == INVALID_FILE_ATTRIBUTES) break;
        if (count > 99)
        {
            free( path );
            return NULL;
        }
        swprintf( path, len + 4, L"%s%s %u", root, newfolder, count++ );
    }

    ptr = wcsrchr( path, '\\' ) + 1;
    *ret_len = lstrlenW(ptr);
    memmove( path, ptr, *ret_len * sizeof(WCHAR) );
    return path;
}

static UINT msi_dialog_directorylist_new( msi_dialog *dialog )
{
    msi_control *control;
    WCHAR *path;
    LVITEMW item;
    int index;

    control = msi_dialog_find_control_by_type( dialog, L"DirectoryList" );

    if (!(path = get_path_property( dialog, control ))) return ERROR_OUTOFMEMORY;

    item.mask = LVIF_TEXT;
    item.iItem = 0;
    item.iSubItem = 0;
    item.pszText = get_unique_folder_name( path, &item.cchTextMax );

    index = SendMessageW( control->hwnd, LVM_INSERTITEMW, 0, (LPARAM)&item );
    SendMessageW( control->hwnd, LVM_ENSUREVISIBLE, index, 0 );
    SendMessageW( control->hwnd, LVM_EDITLABELW, index, -1 );

    free( path );
    free( item.pszText );
    return ERROR_SUCCESS;
}

static UINT msi_dialog_dirlist_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    NMHDR *nmhdr = (NMHDR *)param;
    WCHAR text[MAX_PATH], *new_path, *path, *prop;
    BOOL indirect;

    switch (nmhdr->code)
    {
    case LVN_ENDLABELEDITW:
    {
        NMLVDISPINFOW *info = (NMLVDISPINFOW *)param;
        if (!info->item.pszText) return ERROR_SUCCESS;
        lstrcpynW( text, info->item.pszText, ARRAY_SIZE(text) );
        text[ARRAY_SIZE(text) - 1] = 0;
        break;
    }
    case LVN_ITEMACTIVATE:
    {
        LVITEMW item;
        int index = SendMessageW( control->hwnd, LVM_GETNEXTITEM, -1, LVNI_SELECTED );
        if (index < 0)
        {
            ERR("no list-view item selected\n");
            return ERROR_FUNCTION_FAILED;
        }

        item.iSubItem = 0;
        item.pszText = text;
        item.cchTextMax = MAX_PATH;
        SendMessageW( control->hwnd, LVM_GETITEMTEXTW, index, (LPARAM)&item );
        text[ARRAY_SIZE(text) - 1] = 0;
        break;
    }
    default:
        return ERROR_SUCCESS;
    }

    indirect = control->attributes & msidbControlAttributesIndirect;
    prop = msi_dialog_dup_property( dialog, control->property, indirect );
    path = msi_dialog_dup_property( dialog, prop, TRUE );

    if (!(new_path = malloc( (wcslen(path) + wcslen(text) + 2) * sizeof(WCHAR) )))
    {
        free( prop );
        free( path );
        return ERROR_OUTOFMEMORY;
    }
    lstrcpyW( new_path, path );
    lstrcatW( new_path, text );
    if (nmhdr->code == LVN_ENDLABELEDITW) CreateDirectoryW( new_path, NULL );
    lstrcatW( new_path, L"\\" );

    msi_dialog_set_property( dialog->package, prop, new_path );

    msi_dialog_update_directory_list( dialog, NULL );
    msi_dialog_update_directory_combo( dialog, NULL );
    msi_dialog_update_pathedit( dialog, NULL );

    free( prop );
    free( path );
    free( new_path );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_directory_list( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop;
    DWORD style;

    style = LVS_LIST | WS_VSCROLL | LVS_SHAREIMAGELISTS | LVS_EDITLABELS |
            LVS_AUTOARRANGE | LVS_SINGLESEL | WS_BORDER |
            LVS_SORTASCENDING | WS_CHILD | WS_GROUP | WS_TABSTOP;
    control = msi_dialog_add_control( dialog, rec, WC_LISTVIEWW, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    control->attributes = MSI_RecordGetInteger( rec, 8 );
    control->handler = msi_dialog_dirlist_handler;
    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    /* double click to activate an item in the list */
    SendMessageW( control->hwnd, LVM_SETEXTENDEDLISTVIEWSTYLE,
                  0, LVS_EX_TWOCLICKACTIVATE );

    msi_dialog_update_directory_list( dialog, control );

    return ERROR_SUCCESS;
}

/******************** VolumeCost List ***************************************/

static BOOL str_is_number( LPCWSTR str )
{
    int i;

    for (i = 0; i < lstrlenW( str ); i++)
        if (!iswdigit(str[i]))
            return FALSE;

    return TRUE;
}

static const WCHAR column_keys[][80] =
{
    L"VolumeCostVolume",
    L"VolumeCostSize",
    L"VolumeCostAvailable",
    L"VolumeCostRequired",
    L"VolumeCostDifference",
};

static void msi_dialog_vcl_add_columns( msi_dialog *dialog, msi_control *control, MSIRECORD *rec )
{
    LPCWSTR text = MSI_RecordGetString( rec, 10 );
    LPCWSTR begin = text, end;
    WCHAR *num;
    LVCOLUMNW lvc;
    DWORD count = 0;

    if (!text) return;

    while ((begin = wcschr( begin, '{' )) && count < 5)
    {
        if (!(end = wcschr( begin, '}' )))
            return;

        num = malloc( (end - begin + 1) * sizeof(WCHAR) );
        if (!num)
            return;

        lstrcpynW( num, begin + 1, end - begin );
        begin += end - begin + 1;

        /* empty braces or '0' hides the column */
        if ( !num[0] || !wcscmp( num, L"0" ) )
        {
            count++;
            free( num );
            continue;
        }

        /* the width must be a positive number
         * if a width is invalid, all remaining columns are hidden
         */
        if ( !wcsncmp( num, L"-", 1 ) || !str_is_number( num ) ) {
            free( num );
            return;
        }

        ZeroMemory( &lvc, sizeof(lvc) );
        lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        lvc.cx = wcstol( num, NULL, 10 );
        lvc.pszText = msi_dialog_get_uitext( dialog, column_keys[count] );

        SendMessageW( control->hwnd,  LVM_INSERTCOLUMNW, count++, (LPARAM)&lvc );
        free( lvc.pszText );
        free( num );
    }
}

static LONGLONG msi_vcl_get_cost( msi_dialog *dialog )
{
    MSIFEATURE *feature;
    INT each_cost;
    LONGLONG total_cost = 0;

    LIST_FOR_EACH_ENTRY( feature, &dialog->package->features, MSIFEATURE, entry )
    {
        if (ERROR_SUCCESS == (MSI_GetFeatureCost(dialog->package, feature,
                MSICOSTTREE_SELFONLY, INSTALLSTATE_LOCAL, &each_cost)))
        {
            /* each_cost is in 512-byte units */
            total_cost += ((LONGLONG)each_cost) * 512;
        }
        if (ERROR_SUCCESS == (MSI_GetFeatureCost(dialog->package, feature,
                MSICOSTTREE_SELFONLY, INSTALLSTATE_ABSENT, &each_cost)))
        {
            /* each_cost is in 512-byte units */
            total_cost -= ((LONGLONG)each_cost) * 512;
        }
    }
    return total_cost;
}

static void msi_dialog_vcl_add_drives( msi_dialog *dialog, msi_control *control )
{
    ULARGE_INTEGER total, unused;
    LONGLONG difference, cost;
    WCHAR size_text[MAX_PATH];
    WCHAR cost_text[MAX_PATH];
    LPWSTR drives, ptr;
    LVITEMW lvitem;
    DWORD size, flags;
    int i = 0;

    cost = msi_vcl_get_cost(dialog);
    StrFormatByteSizeW(cost, cost_text, MAX_PATH);

    size = GetLogicalDriveStringsW( 0, NULL );
    if ( !size ) return;

    drives = malloc( (size + 1) * sizeof(WCHAR) );
    if ( !drives ) return;

    GetLogicalDriveStringsW( size, drives );

    ptr = drives;
    while (*ptr)
    {
        if (GetVolumeInformationW(ptr, NULL, 0, NULL, 0, &flags, NULL, 0) &&
            flags & FILE_READ_ONLY_VOLUME)
        {
            ptr += lstrlenW(ptr) + 1;
            continue;
        }

        lvitem.mask = LVIF_TEXT;
        lvitem.iItem = i;
        lvitem.iSubItem = 0;
        lvitem.pszText = ptr;
        lvitem.cchTextMax = lstrlenW(ptr) + 1;
        SendMessageW( control->hwnd, LVM_INSERTITEMW, 0, (LPARAM)&lvitem );

        GetDiskFreeSpaceExW(ptr, &unused, &total, NULL);
        difference = unused.QuadPart - cost;

        StrFormatByteSizeW(total.QuadPart, size_text, MAX_PATH);
        lvitem.iSubItem = 1;
        lvitem.pszText = size_text;
        lvitem.cchTextMax = lstrlenW(size_text) + 1;
        SendMessageW( control->hwnd, LVM_SETITEMW, 0, (LPARAM)&lvitem );

        StrFormatByteSizeW(unused.QuadPart, size_text, MAX_PATH);
        lvitem.iSubItem = 2;
        lvitem.pszText = size_text;
        lvitem.cchTextMax = lstrlenW(size_text) + 1;
        SendMessageW( control->hwnd, LVM_SETITEMW, 0, (LPARAM)&lvitem );

        lvitem.iSubItem = 3;
        lvitem.pszText = cost_text;
        lvitem.cchTextMax = lstrlenW(cost_text) + 1;
        SendMessageW( control->hwnd, LVM_SETITEMW, 0, (LPARAM)&lvitem );

        StrFormatByteSizeW(difference, size_text, MAX_PATH);
        lvitem.iSubItem = 4;
        lvitem.pszText = size_text;
        lvitem.cchTextMax = lstrlenW(size_text) + 1;
        SendMessageW( control->hwnd, LVM_SETITEMW, 0, (LPARAM)&lvitem );

        ptr += lstrlenW(ptr) + 1;
        i++;
    }

    free( drives );
}

static UINT msi_dialog_volumecost_list( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    DWORD style;

    style = LVS_REPORT | WS_VSCROLL | WS_HSCROLL | LVS_SHAREIMAGELISTS |
            LVS_AUTOARRANGE | LVS_SINGLESEL | WS_BORDER |
            WS_CHILD | WS_TABSTOP | WS_GROUP;
    control = msi_dialog_add_control( dialog, rec, WC_LISTVIEWW, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    msi_dialog_vcl_add_columns( dialog, control, rec );
    msi_dialog_vcl_add_drives( dialog, control );

    return ERROR_SUCCESS;
}

/******************** VolumeSelect Combo ***************************************/

static UINT msi_dialog_volsel_handler( msi_dialog *dialog,
                                       msi_control *control, WPARAM param )
{
    WCHAR text[MAX_PATH];
    LPWSTR prop;
    BOOL indirect;
    int index;

    if (HIWORD(param) != CBN_SELCHANGE)
        return ERROR_SUCCESS;

    index = SendMessageW( control->hwnd, CB_GETCURSEL, 0, 0 );
    if ( index == CB_ERR )
    {
        ERR("No ComboBox item selected!\n");
        return ERROR_FUNCTION_FAILED;
    }

    SendMessageW( control->hwnd, CB_GETLBTEXT, index, (LPARAM)text );

    indirect = control->attributes & msidbControlAttributesIndirect;
    prop = msi_dialog_dup_property( dialog, control->property, indirect );

    msi_dialog_set_property( dialog->package, prop, text );

    free( prop );
    return ERROR_SUCCESS;
}

static void msi_dialog_vsc_add_drives( msi_dialog *dialog, msi_control *control )
{
    LPWSTR drives, ptr;
    DWORD size;

    size = GetLogicalDriveStringsW( 0, NULL );
    if ( !size ) return;

    drives = malloc( (size + 1) * sizeof(WCHAR) );
    if ( !drives ) return;

    GetLogicalDriveStringsW( size, drives );

    ptr = drives;
    while (*ptr)
    {
        SendMessageW( control->hwnd, CB_ADDSTRING, 0, (LPARAM)ptr );
        ptr += lstrlenW(ptr) + 1;
    }

    free( drives );
}

static UINT msi_dialog_volumeselect_combo( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop;
    DWORD style;

    /* FIXME: CBS_OWNERDRAWFIXED */
    style = WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP |
            CBS_DROPDOWNLIST | CBS_SORT | CBS_HASSTRINGS |
            WS_EX_LEFT | WS_EX_LTRREADING | WS_EX_RIGHTSCROLLBAR;
    control = msi_dialog_add_control( dialog, rec, WC_COMBOBOXW, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    control->attributes = MSI_RecordGetInteger( rec, 8 );
    control->handler = msi_dialog_volsel_handler;
    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    msi_dialog_vsc_add_drives( dialog, control );

    return ERROR_SUCCESS;
}

static UINT msi_dialog_hyperlink_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    int len, len_href = ARRAY_SIZE( L"href" ) - 1;
    const WCHAR *p, *q;
    WCHAR quote = 0;
    LITEM item;

    item.mask     = LIF_ITEMINDEX | LIF_URL;
    item.iLink    = 0;
    item.szUrl[0] = 0;

    SendMessageW( control->hwnd, LM_GETITEM, 0, (LPARAM)&item );

    p = item.szUrl;
    while (*p && *p != '<') p++;
    if (!*p++) return ERROR_SUCCESS;
    if (towupper( *p++ ) != 'A' || !iswspace( *p++ )) return ERROR_SUCCESS;
    while (*p && iswspace( *p )) p++;

    len = lstrlenW( p );
    if (len > len_href && !wcsnicmp( p, L"href", len_href ))
    {
        p += len_href;
        while (*p && iswspace( *p )) p++;
        if (!*p || *p++ != '=') return ERROR_SUCCESS;
        while (*p && iswspace( *p )) p++;

        if (*p == '\"' || *p == '\'') quote = *p++;
        q = p;
        if (quote)
        {
            while (*q && *q != quote) q++;
            if (*q != quote) return ERROR_SUCCESS;
        }
        else
        {
            while (*q && *q != '>' && !iswspace( *q )) q++;
            if (!*q) return ERROR_SUCCESS;
        }
        item.szUrl[q - item.szUrl] = 0;
        ShellExecuteW( NULL, L"open", p, NULL, NULL, SW_SHOWNORMAL );
    }
    return ERROR_SUCCESS;
}

static UINT msi_dialog_hyperlink( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    DWORD style = WS_CHILD | WS_TABSTOP | WS_GROUP;
    const WCHAR *text = MSI_RecordGetString( rec, 10 );
    int len = lstrlenW( text );
    LITEM item;

    control = msi_dialog_add_control( dialog, rec, WC_LINK, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    control->attributes = MSI_RecordGetInteger( rec, 8 );
    control->handler    = msi_dialog_hyperlink_handler;

    item.mask      = LIF_ITEMINDEX | LIF_STATE | LIF_URL;
    item.iLink     = 0;
    item.state     = LIS_ENABLED;
    item.stateMask = LIS_ENABLED;
    if (len < L_MAX_URL_LENGTH) lstrcpyW( item.szUrl, text );
    else item.szUrl[0] = 0;

    SendMessageW( control->hwnd, LM_SETITEM, 0, (LPARAM)&item );

    return ERROR_SUCCESS;
}

/******************** ListView *****************************************/

struct listview_param
{
    msi_dialog *dialog;
    msi_control *control;
};

static UINT msi_dialog_listview_handler( msi_dialog *dialog, msi_control *control, WPARAM param )
{
    NMHDR *nmhdr = (NMHDR *)param;

    FIXME("code %#x (%d)\n", nmhdr->code, nmhdr->code);

    return ERROR_SUCCESS;
}

static UINT msi_listview_add_item( MSIRECORD *rec, LPVOID param )
{
    struct listview_param *lv_param = (struct listview_param *)param;
    LPCWSTR text, binary;
    LVITEMW item;
    HICON hIcon;

    text = MSI_RecordGetString( rec, 4 );
    binary = MSI_RecordGetString( rec, 5 );
    hIcon = msi_load_icon( lv_param->dialog->package->db, binary, 0 );

    TRACE("Adding: text %s, binary %s, icon %p\n", debugstr_w(text), debugstr_w(binary), hIcon);

    memset( &item, 0, sizeof(item) );
    item.mask = LVIF_TEXT | LVIF_IMAGE;
    deformat_string( lv_param->dialog->package, text, &item.pszText );
    item.iImage = ImageList_AddIcon( lv_param->control->hImageList, hIcon );
    item.iItem = item.iImage;
    SendMessageW( lv_param->control->hwnd, LVM_INSERTITEMW, 0, (LPARAM)&item );

    DestroyIcon( hIcon );

    return ERROR_SUCCESS;
}

static UINT msi_listview_add_items( msi_dialog *dialog, msi_control *control )
{
    MSIQUERY *view;
    struct listview_param lv_param = { dialog, control };

    if (MSI_OpenQuery( dialog->package->db, &view, L"SELECT * FROM `ListView` WHERE `Property` = '%s' ORDER BY `Order`",
                       control->property ) == ERROR_SUCCESS)
    {
        MSI_IterateRecords( view, NULL, msi_listview_add_item, &lv_param );
        msiobj_release( &view->hdr );
    }

    return ERROR_SUCCESS;
}

static UINT msi_dialog_listview( msi_dialog *dialog, MSIRECORD *rec )
{
    msi_control *control;
    LPCWSTR prop;
    DWORD style, attributes;
    LVCOLUMNW col;
    RECT rc;

    style = LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SHAREIMAGELISTS | LVS_SINGLESEL |
            LVS_SHOWSELALWAYS | WS_VSCROLL | WS_HSCROLL | WS_BORDER | WS_TABSTOP | WS_CHILD;
    attributes = MSI_RecordGetInteger( rec, 8 );
    if ( ~attributes & msidbControlAttributesSorted )
        style |= LVS_SORTASCENDING;
    control = msi_dialog_add_control( dialog, rec, WC_LISTVIEWW, style );
    if (!control)
        return ERROR_FUNCTION_FAILED;

    prop = MSI_RecordGetString( rec, 9 );
    control->property = msi_dialog_dup_property( dialog, prop, FALSE );

    control->hImageList = ImageList_Create( 16, 16, ILC_COLOR32, 0, 1);
    SendMessageW( control->hwnd, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)control->hImageList );

    col.mask = LVCF_FMT | LVCF_WIDTH;
    col.fmt = LVCFMT_LEFT;
    col.cx = 16;
    SendMessageW( control->hwnd, LVM_INSERTCOLUMNW, 0, (LPARAM)&col );

    GetClientRect( control->hwnd, &rc );
    col.cx = rc.right - 16;
    SendMessageW( control->hwnd, LVM_INSERTCOLUMNW, 0, (LPARAM)&col );

    if (control->property)
        msi_listview_add_items( dialog, control );

    control->handler = msi_dialog_listview_handler;

    return ERROR_SUCCESS;
}

static const struct control_handler msi_dialog_handler[] =
{
    { L"Text", msi_dialog_text_control },
    { L"PushButton", msi_dialog_button_control },
    { L"Line", msi_dialog_line_control },
    { L"Bitmap", msi_dialog_bitmap_control },
    { L"CheckBox", msi_dialog_checkbox_control },
    { L"ScrollableText", msi_dialog_scrolltext_control },
    { L"ComboBox", msi_dialog_combo_control },
    { L"Edit", msi_dialog_edit_control },
    { L"MaskedEdit", msi_dialog_maskedit_control },
    { L"PathEdit", msi_dialog_pathedit_control },
    { L"ProgressBar", msi_dialog_progress_bar },
    { L"RadioButtonGroup", msi_dialog_radiogroup_control },
    { L"Icon", msi_dialog_icon_control },
    { L"SelectionTree", msi_dialog_selection_tree },
    { L"GroupBox", msi_dialog_group_box },
    { L"ListBox", msi_dialog_list_box },
    { L"DirectoryCombo", msi_dialog_directory_combo },
    { L"DirectoryList", msi_dialog_directory_list },
    { L"VolumeCostList", msi_dialog_volumecost_list },
    { L"VolumeSelectCombo", msi_dialog_volumeselect_combo },
    { L"HyperLink", msi_dialog_hyperlink },
    { L"ListView", msi_dialog_listview }
};

static UINT msi_dialog_create_controls( MSIRECORD *rec, LPVOID param )
{
    msi_dialog *dialog = param;
    LPCWSTR control_type;
    UINT i;

    /* find and call the function that can create this type of control */
    control_type = MSI_RecordGetString( rec, 3 );
    for( i = 0; i < ARRAY_SIZE( msi_dialog_handler ); i++ )
        if (!wcsicmp( msi_dialog_handler[i].control_type, control_type ))
            break;
    if( i != ARRAY_SIZE( msi_dialog_handler  ))
        msi_dialog_handler[i].func( dialog, rec );
    else
        ERR("no handler for element type %s\n", debugstr_w(control_type));

    return ERROR_SUCCESS;
}

static UINT msi_dialog_fill_controls( msi_dialog *dialog )
{
    UINT r;
    MSIQUERY *view;
    MSIPACKAGE *package = dialog->package;

    TRACE("%p %s\n", dialog, debugstr_w(dialog->name) );

    /* query the Control table for all the elements of the control */
    r = MSI_OpenQuery( package->db, &view, L"SELECT * FROM `Control` WHERE `Dialog_` = '%s'", dialog->name );
    if( r != ERROR_SUCCESS )
    {
        ERR("query failed for dialog %s\n", debugstr_w(dialog->name));
        return ERROR_INVALID_PARAMETER;
    }

    r = MSI_IterateRecords( view, 0, msi_dialog_create_controls, dialog );
    msiobj_release( &view->hdr );
    return r;
}

static UINT msi_dialog_reset( msi_dialog *dialog )
{
    /* FIXME: should restore the original values of any properties we changed */
    return msi_dialog_evaluate_control_conditions( dialog );
}

/* figure out the height of 10 point MS Sans Serif */
static INT msi_dialog_get_sans_serif_height( HWND hwnd )
{
    LOGFONTW lf;
    TEXTMETRICW tm;
    BOOL r;
    LONG height = 0;
    HFONT hFont, hOldFont;
    HDC hdc;

    hdc = GetDC( hwnd );
    if (hdc)
    {
        memset( &lf, 0, sizeof lf );
        lf.lfHeight = MulDiv(12, GetDeviceCaps(hdc, LOGPIXELSY), 72);
        lstrcpyW( lf.lfFaceName, L"MS Sans Serif" );
        hFont = CreateFontIndirectW(&lf);
        if (hFont)
        {
            hOldFont = SelectObject( hdc, hFont );
            r = GetTextMetricsW( hdc, &tm );
            if (r)
                height = tm.tmHeight;
            SelectObject( hdc, hOldFont );
            DeleteObject( hFont );
        }
        ReleaseDC( hwnd, hdc );
    }
    return height;
}

/* fetch the associated record from the Dialog table */
static MSIRECORD *msi_get_dialog_record( msi_dialog *dialog )
{
    MSIPACKAGE *package = dialog->package;
    MSIRECORD *rec = NULL;

    TRACE("%p %s\n", dialog, debugstr_w(dialog->name) );

    rec = MSI_QueryGetRecord( package->db, L"SELECT * FROM `Dialog` WHERE `Dialog` = '%s'", dialog->name );
    if( !rec )
        WARN("query failed for dialog %s\n", debugstr_w(dialog->name));

    return rec;
}

static void msi_dialog_adjust_dialog_pos( msi_dialog *dialog, MSIRECORD *rec, LPRECT pos )
{
    UINT xres, yres;
    POINT center;
    SIZE sz;
    LONG style;

    center.x = MSI_RecordGetInteger( rec, 2 );
    center.y = MSI_RecordGetInteger( rec, 3 );

    sz.cx = MSI_RecordGetInteger( rec, 4 );
    sz.cy = MSI_RecordGetInteger( rec, 5 );

    sz.cx = msi_dialog_scale_unit( dialog, sz.cx );
    sz.cy = msi_dialog_scale_unit( dialog, sz.cy );

    xres = msi_get_property_int( dialog->package->db, L"ScreenX", 0 );
    yres = msi_get_property_int( dialog->package->db, L"ScreenY", 0 );

    center.x = MulDiv( center.x, xres, 100 );
    center.y = MulDiv( center.y, yres, 100 );

    /* turn the client pos into the window rectangle */
    if (dialog->package->center_x && dialog->package->center_y)
    {
        pos->left = dialog->package->center_x - sz.cx / 2.0;
        pos->right = pos->left + sz.cx;
        pos->top = dialog->package->center_y - sz.cy / 2.0;
        pos->bottom = pos->top + sz.cy;
    }
    else
    {
        pos->left = center.x - sz.cx/2;
        pos->right = pos->left + sz.cx;
        pos->top = center.y - sz.cy/2;
        pos->bottom = pos->top + sz.cy;

        /* save the center */
        dialog->package->center_x = center.x;
        dialog->package->center_y = center.y;
    }

    dialog->size.cx = sz.cx;
    dialog->size.cy = sz.cy;

    TRACE("%s\n", wine_dbgstr_rect(pos));

    style = GetWindowLongPtrW( dialog->hwnd, GWL_STYLE );
    AdjustWindowRect( pos, style, FALSE );
}

static void msi_dialog_set_tab_order( msi_dialog *dialog, LPCWSTR first )
{
    struct list tab_chain;
    msi_control *control;
    HWND prev = HWND_TOP;

    list_init( &tab_chain );
    if (!(control = msi_dialog_find_control( dialog, first ))) return;

    dialog->hWndFocus = control->hwnd;
    while (control)
    {
        list_remove( &control->entry );
        list_add_tail( &tab_chain, &control->entry );
        if (!control->tabnext) break;
        control = msi_dialog_find_control( dialog, control->tabnext );
    }

    LIST_FOR_EACH_ENTRY( control, &tab_chain, msi_control, entry )
    {
        SetWindowPos( control->hwnd, prev, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOOWNERZORDER | SWP_NOREDRAW |
                      SWP_NOREPOSITION | SWP_NOSENDCHANGING | SWP_NOSIZE );
        prev = control->hwnd;
    }

    /* put them back on the main list */
    list_move_head( &dialog->controls, &tab_chain );
}

static LRESULT msi_dialog_oncreate( HWND hwnd, LPCREATESTRUCTW cs )
{
    msi_dialog *dialog = cs->lpCreateParams;
    MSIRECORD *rec = NULL;
    LPWSTR title = NULL;
    RECT pos;

    TRACE("%p %p\n", dialog, dialog->package);

    dialog->hwnd = hwnd;
    SetWindowLongPtrW( hwnd, GWLP_USERDATA, (LONG_PTR) dialog );

    rec = msi_get_dialog_record( dialog );
    if( !rec )
    {
        TRACE("No record found for dialog %s\n", debugstr_w(dialog->name));
        return -1;
    }

    dialog->scale = msi_dialog_get_sans_serif_height(dialog->hwnd);

    msi_dialog_adjust_dialog_pos( dialog, rec, &pos );

    dialog->attributes = MSI_RecordGetInteger( rec, 6 );

    dialog->default_font = msi_dup_property( dialog->package->db, L"DefaultUIFont" );
    if (!dialog->default_font)
    {
        dialog->default_font = wcsdup( L"MS Shell Dlg" );
        if (!dialog->default_font)
        {
            msiobj_release( &rec->hdr );
            return -1;
        }
    }

    title = msi_get_deformatted_field( dialog->package, rec, 7 );
    SetWindowTextW( hwnd, title );
    free( title );

    SetWindowPos( hwnd, 0, pos.left, pos.top,
                  pos.right - pos.left, pos.bottom - pos.top,
                  SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW );

    msi_dialog_build_font_list( dialog );
    msi_dialog_fill_controls( dialog );
    msi_dialog_evaluate_control_conditions( dialog );
    msi_dialog_set_tab_order( dialog, MSI_RecordGetString( rec, 8 ) );
    msiobj_release( &rec->hdr );

    return 0;
}

static LRESULT msi_dialog_oncommand( msi_dialog *dialog, WPARAM param, HWND hwnd )
{
    msi_control *control = NULL;

    TRACE( "%p, %#Ix, %p\n", dialog, param, hwnd );

    switch (param)
    {
    case 1: /* enter */
        control = msi_dialog_find_control( dialog, dialog->control_default );
        break;
    case 2: /* escape */
        control = msi_dialog_find_control( dialog, dialog->control_cancel );
        break;
    default:
        control = msi_dialog_find_control_by_hwnd( dialog, hwnd );
    }

    if( control )
    {
        if( control->handler )
        {
            control->handler( dialog, control, param );
            msi_dialog_evaluate_control_conditions( dialog );
        }
    }

    return 0;
}

static LRESULT msi_dialog_onnotify( msi_dialog *dialog, LPARAM param )
{
    LPNMHDR nmhdr = (LPNMHDR) param;
    msi_control *control = msi_dialog_find_control_by_hwnd( dialog, nmhdr->hwndFrom );

    TRACE("%p %p\n", dialog, nmhdr->hwndFrom);

    if ( control && control->handler )
        control->handler( dialog, control, param );

    return 0;
}

static void dialog_setfocus( msi_dialog *dialog )
{
    HWND hwnd = dialog->hWndFocus;

    hwnd = GetNextDlgTabItem( dialog->hwnd, hwnd, TRUE);
    hwnd = GetNextDlgTabItem( dialog->hwnd, hwnd, FALSE);
    SetFocus( hwnd );
    dialog->hWndFocus = hwnd;
}

static LRESULT WINAPI MSIDialog_WndProc( HWND hwnd, UINT msg,
                WPARAM wParam, LPARAM lParam )
{
    msi_dialog *dialog = (LPVOID) GetWindowLongPtrW( hwnd, GWLP_USERDATA );

    TRACE("0x%04x\n", msg);

    switch (msg)
    {
    case WM_MOVE:
        dialog->package->center_x = LOWORD(lParam) + dialog->size.cx / 2.0;
        dialog->package->center_y = HIWORD(lParam) + dialog->size.cy / 2.0;
        break;

    case WM_CREATE:
        return msi_dialog_oncreate( hwnd, (LPCREATESTRUCTW)lParam );

    case WM_COMMAND:
        return msi_dialog_oncommand( dialog, wParam, (HWND)lParam );

    case WM_CLOSE:
        /* Simulate escape press */
        return msi_dialog_oncommand(dialog, 2, NULL);

    case WM_ACTIVATE:
        if( LOWORD(wParam) == WA_INACTIVE )
            dialog->hWndFocus = GetFocus();
        else
            dialog_setfocus( dialog );
        return 0;

    case WM_SETFOCUS:
        dialog_setfocus( dialog );
        return 0;

    /* bounce back to our subclassed static control */
    case WM_CTLCOLORSTATIC:
        return SendMessageW( (HWND) lParam, WM_CTLCOLORSTATIC, wParam, lParam );

    case WM_DESTROY:
        dialog->hwnd = NULL;
        return 0;
    case WM_NOTIFY:
        return msi_dialog_onnotify( dialog, lParam );
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void process_pending_messages( HWND hdlg )
{
    MSG msg;

    while (PeekMessageW( &msg, 0, 0, 0, PM_REMOVE ))
    {
        if (hdlg && IsDialogMessageW( hdlg, &msg )) continue;
        TranslateMessage( &msg );
        DispatchMessageW( &msg );
    }
}

static UINT dialog_run_message_loop( msi_dialog *dialog )
{
    DWORD style;
    HWND hwnd, parent;

    if( uiThreadId != GetCurrentThreadId() )
        return SendMessageW( hMsiHiddenWindow, WM_MSI_DIALOG_CREATE, 0, (LPARAM) dialog );

    /* create the dialog window, don't show it yet */
    style = WS_OVERLAPPED | WS_SYSMENU;
    if( dialog->attributes & msidbDialogAttributesVisible )
        style |= WS_VISIBLE;

    if (dialog->parent == NULL && (dialog->attributes & msidbDialogAttributesMinimize))
        style |= WS_MINIMIZEBOX;

    parent = dialog->parent ? dialog->parent->hwnd : 0;

    hwnd = CreateWindowW( L"MsiDialogCloseClass", dialog->name, style,
                     CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                     parent, NULL, NULL, dialog );
    if( !hwnd )
    {
        ERR("Failed to create dialog %s\n", debugstr_w( dialog->name ));
        return ERROR_FUNCTION_FAILED;
    }

    ShowWindow( hwnd, SW_SHOW );
    /* UpdateWindow( hwnd ); - and causes the transparent static controls not to paint */

    if( dialog->attributes & msidbDialogAttributesModal )
    {
        while( !dialog->finished )
        {
            MsgWaitForMultipleObjects( 0, NULL, 0, INFINITE, QS_ALLINPUT );
            process_pending_messages( dialog->hwnd );
        }
    }
    else
        return ERROR_IO_PENDING;

    return ERROR_SUCCESS;
}

static LRESULT WINAPI MSIHiddenWindowProc( HWND hwnd, UINT msg,
                WPARAM wParam, LPARAM lParam )
{
    msi_dialog *dialog = (msi_dialog*) lParam;

    TRACE("%d %p\n", msg, dialog);

    switch (msg)
    {
    case WM_MSI_DIALOG_CREATE:
        return dialog_run_message_loop( dialog );
    case WM_MSI_DIALOG_DESTROY:
        msi_dialog_destroy( dialog );
        return 0;
    }
    return DefWindowProcW( hwnd, msg, wParam, lParam );
}

static BOOL dialog_register_class( void )
{
    WNDCLASSW cls;

    ZeroMemory( &cls, sizeof cls );
    cls.lpfnWndProc   = MSIDialog_WndProc;
    cls.hInstance     = NULL;
    cls.hIcon         = LoadIconW(0, (LPWSTR)IDI_APPLICATION);
    cls.hCursor       = LoadCursorW(0, (LPWSTR)IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    cls.lpszMenuName  = NULL;
    cls.lpszClassName = L"MsiDialogCloseClass";

    if( !RegisterClassW( &cls ) )
        return FALSE;

    cls.lpfnWndProc   = MSIHiddenWindowProc;
    cls.lpszClassName = L"MsiHiddenWindow";

    if( !RegisterClassW( &cls ) )
        return FALSE;

    uiThreadId = GetCurrentThreadId();

    hMsiHiddenWindow = CreateWindowW( L"MsiHiddenWindow", NULL, WS_OVERLAPPED,
                                   0, 0, 100, 100, NULL, NULL, NULL, NULL );
    if( !hMsiHiddenWindow )
        return FALSE;

    return TRUE;
}

static msi_dialog *dialog_create( MSIPACKAGE *package, const WCHAR *name, msi_dialog *parent,
                                  control_event_handler event_handler )
{
    MSIRECORD *rec = NULL;
    msi_dialog *dialog;

    TRACE("%s\n", debugstr_w(name));

    if (!hMsiHiddenWindow) dialog_register_class();

    /* allocate the structure for the dialog to use */
    dialog = calloc( 1, offsetof( msi_dialog, name[wcslen( name ) + 1] ) );
    if( !dialog )
        return NULL;
    lstrcpyW( dialog->name, name );
    dialog->parent = parent;
    dialog->package = package;
    dialog->event_handler = event_handler;
    dialog->finished = 0;
    list_init( &dialog->controls );
    list_init( &dialog->fonts );

    /* verify that the dialog exists */
    rec = msi_get_dialog_record( dialog );
    if( !rec )
    {
        free( dialog );
        return NULL;
    }
    dialog->attributes = MSI_RecordGetInteger( rec, 6 );
    dialog->control_default = wcsdup( MSI_RecordGetString( rec, 9 ) );
    dialog->control_cancel = wcsdup( MSI_RecordGetString( rec, 10 ) );
    msiobj_release( &rec->hdr );

    rec = MSI_CreateRecord(2);
    if (!rec)
    {
        msi_dialog_destroy(dialog);
        return NULL;
    }
    MSI_RecordSetStringW(rec, 1, name);
    MSI_RecordSetStringW(rec, 2, L"Dialog created");
    MSI_ProcessMessage(package, INSTALLMESSAGE_ACTIONSTART, rec);
    msiobj_release(&rec->hdr);

    return dialog;
}

static void msi_dialog_end_dialog( msi_dialog *dialog )
{
    TRACE("%p\n", dialog);
    dialog->finished = 1;
    PostMessageW(dialog->hwnd, WM_NULL, 0, 0);
}

void msi_dialog_check_messages( HANDLE handle )
{
    DWORD r;

    /* in threads other than the UI thread, block */
    if( uiThreadId != GetCurrentThreadId() )
    {
        if (!handle) return;
        while (MsgWaitForMultipleObjectsEx( 1, &handle, INFINITE, QS_ALLINPUT, 0 ) == WAIT_OBJECT_0 + 1)
        {
            MSG msg;
            while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE ))
            {
                TranslateMessage( &msg );
                DispatchMessageW( &msg );
            }
        }
        return;
    }

    /* there are two choices for the UI thread */
    while (1)
    {
        process_pending_messages( NULL );

        if( !handle )
            break;

        /*
         * block here until somebody creates a new dialog or
         * the handle we're waiting on becomes ready
         */
        r = MsgWaitForMultipleObjects( 1, &handle, 0, INFINITE, QS_ALLINPUT );
        if( r == WAIT_OBJECT_0 )
            break;
    }
}

static void dialog_do_preview( msi_dialog *dialog )
{
    TRACE("\n");
    dialog->attributes |= msidbDialogAttributesVisible;
    dialog->attributes &= ~msidbDialogAttributesModal;
    dialog_run_message_loop( dialog );
}

static void free_subscriber( struct subscriber *sub )
{
    free( sub->event );
    free( sub->control );
    free( sub->attribute );
    free( sub );
}

static void event_cleanup_subscriptions( MSIPACKAGE *package, const WCHAR *dialog )
{
    struct list *item, *next;

    LIST_FOR_EACH_SAFE( item, next, &package->subscriptions )
    {
        struct subscriber *sub = LIST_ENTRY( item, struct subscriber, entry );

        if (wcscmp( sub->dialog->name, dialog )) continue;
        list_remove( &sub->entry );
        free_subscriber( sub );
    }
}

void msi_dialog_destroy( msi_dialog *dialog )
{
    msi_font *font, *next;

    if( uiThreadId != GetCurrentThreadId() )
    {
        SendMessageW( hMsiHiddenWindow, WM_MSI_DIALOG_DESTROY, 0, (LPARAM) dialog );
        return;
    }

    if( dialog->hwnd )
    {
        ShowWindow( dialog->hwnd, SW_HIDE );
        DestroyWindow( dialog->hwnd );
    }

    /* unsubscribe events */
    event_cleanup_subscriptions( dialog->package, dialog->name );

    /* destroy the list of controls */
    while( !list_empty( &dialog->controls ) )
    {
        msi_control *t;

        t = LIST_ENTRY( list_head( &dialog->controls ),
                        msi_control, entry );
        msi_destroy_control( t );
    }

    /* destroy the list of fonts */
    LIST_FOR_EACH_ENTRY_SAFE( font, next, &dialog->fonts, msi_font, entry )
    {
        list_remove( &font->entry );
        DeleteObject( font->hfont );
        free( font );
    }
    free( dialog->default_font );

    free( dialog->control_default );
    free( dialog->control_cancel );
    dialog->package = NULL;
    free( dialog );
}

void msi_dialog_unregister_class( void )
{
    DestroyWindow( hMsiHiddenWindow );
    hMsiHiddenWindow = NULL;
    UnregisterClassW( L"MsiDialogCloseClass", NULL );
    UnregisterClassW( L"MsiHiddenWindow", NULL );
    uiThreadId = 0;
}

void msi_event_cleanup_all_subscriptions( MSIPACKAGE *package )
{
    struct list *item, *next;

    LIST_FOR_EACH_SAFE( item, next, &package->subscriptions )
    {
        struct subscriber *sub = LIST_ENTRY( item, struct subscriber, entry );
        list_remove( &sub->entry );
        free_subscriber( sub );
    }
}

static void MSI_ClosePreview( MSIOBJECTHDR *arg )
{
    MSIPREVIEW *preview = (MSIPREVIEW *)arg;
    msiobj_release( &preview->package->hdr );
}

static MSIPREVIEW *MSI_EnableUIPreview( MSIDATABASE *db )
{
    MSIPREVIEW *preview = NULL;
    MSIPACKAGE *package;

    package = MSI_CreatePackage( db );
    if (package)
    {
        preview = alloc_msiobject( MSIHANDLETYPE_PREVIEW, sizeof(MSIPREVIEW), MSI_ClosePreview );
        if (preview)
        {
            preview->package = package;
            msiobj_addref( &package->hdr );
        }
        msiobj_release( &package->hdr );
    }
    return preview;
}

UINT WINAPI MsiEnableUIPreview( MSIHANDLE hdb, MSIHANDLE *phPreview )
{
    MSIDATABASE *db;
    MSIPREVIEW *preview;
    UINT r = ERROR_FUNCTION_FAILED;

    TRACE( "%lu %p\n", hdb, phPreview );

    if (!(db = msihandle2msiinfo(hdb, MSIHANDLETYPE_DATABASE)))
        return ERROR_INVALID_HANDLE;

    preview = MSI_EnableUIPreview( db );
    if (preview)
    {
        *phPreview = alloc_msihandle( &preview->hdr );
        msiobj_release( &preview->hdr );
        r = ERROR_SUCCESS;
        if (!*phPreview)
            r = ERROR_NOT_ENOUGH_MEMORY;
    }
    msiobj_release( &db->hdr );
    return r;
}

static UINT preview_event_handler( msi_dialog *dialog, const WCHAR *event, const WCHAR *argument )
{
    MESSAGE("Preview dialog event '%s' (arg='%s')\n", debugstr_w(event), debugstr_w(argument));
    return ERROR_SUCCESS;
}

static UINT MSI_PreviewDialogW( MSIPREVIEW *preview, LPCWSTR szDialogName )
{
    msi_dialog *dialog = NULL;
    UINT r = ERROR_SUCCESS;

    if (preview->dialog)
        msi_dialog_destroy( preview->dialog );

    /* an empty name means we should just destroy the current preview dialog */
    if (szDialogName)
    {
        dialog = dialog_create( preview->package, szDialogName, NULL, preview_event_handler );
        if (dialog)
            dialog_do_preview( dialog );
        else
            r = ERROR_FUNCTION_FAILED;
    }
    preview->dialog = dialog;
    return r;
}

UINT WINAPI MsiPreviewDialogW( MSIHANDLE hPreview, LPCWSTR szDialogName )
{
    MSIPREVIEW *preview;
    UINT r;

    TRACE( "%lu %s\n", hPreview, debugstr_w(szDialogName) );

    preview = msihandle2msiinfo( hPreview, MSIHANDLETYPE_PREVIEW );
    if (!preview)
        return ERROR_INVALID_HANDLE;

    r = MSI_PreviewDialogW( preview, szDialogName );
    msiobj_release( &preview->hdr );
    return r;
}

UINT WINAPI MsiPreviewDialogA( MSIHANDLE hPreview, LPCSTR szDialogName )
{
    UINT r;
    LPWSTR strW = NULL;

    TRACE( "%lu %s\n", hPreview, debugstr_a(szDialogName) );

    if (szDialogName)
    {
        strW = strdupAtoW( szDialogName );
        if (!strW)
            return ERROR_OUTOFMEMORY;
    }
    r = MsiPreviewDialogW( hPreview, strW );
    free( strW );
    return r;
}

UINT WINAPI MsiPreviewBillboardW( MSIHANDLE hPreview, const WCHAR *szControlName, const WCHAR *szBillboard )
{
    FIXME( "%lu %s %s\n", hPreview, debugstr_w(szControlName), debugstr_w(szBillboard) );
    return ERROR_CALL_NOT_IMPLEMENTED;
}

UINT WINAPI MsiPreviewBillboardA( MSIHANDLE hPreview, const char *szControlName, const char *szBillboard )
{
    FIXME( "%lu %s %s\n", hPreview, debugstr_a(szControlName), debugstr_a(szBillboard) );
    return ERROR_CALL_NOT_IMPLEMENTED;
}

struct control_event
{
    const WCHAR  *event;
    event_handler handler;
};

static UINT dialog_event_handler( msi_dialog *, const WCHAR *, const WCHAR * );

/* create a dialog box and run it if it's modal */
static INT event_do_dialog( MSIPACKAGE *package, const WCHAR *name, msi_dialog *parent, BOOL destroy_modeless )
{
    msi_dialog *dialog;
    UINT r;
    INT retval;

    /* create a new dialog */
    dialog = dialog_create( package, name, parent, dialog_event_handler );
    if (dialog)
    {
        /* kill the current modeless dialog */
        if (destroy_modeless && package->dialog)
        {
            msi_dialog_destroy( package->dialog );
            package->dialog = NULL;
        }

        /* modeless dialogs return an error message */
        r = dialog_run_message_loop( dialog );
        if (r == ERROR_SUCCESS)
        {
            retval = dialog->retval;
            msi_dialog_destroy( dialog );
            return retval;
        }
        else
        {
            package->dialog = dialog;
            return IDOK;
        }
    }
    else return 0;
}

/* end a modal dialog box */
static UINT event_end_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    if (!wcscmp( argument, L"Exit" ))
        dialog->retval = IDCANCEL;
    else if (!wcscmp( argument, L"Retry" ))
        dialog->retval = IDRETRY;
    else if (!wcscmp( argument, L"Ignore" ))
        dialog->retval = IDOK;
    else if (!wcscmp( argument, L"Return" ))
        dialog->retval = 0;
    else
    {
        ERR("Unknown argument string %s\n", debugstr_w(argument));
        dialog->retval = IDABORT;
    }
    event_cleanup_subscriptions( dialog->package, dialog->name );
    msi_dialog_end_dialog( dialog );
    return ERROR_SUCCESS;
}

static UINT pending_event_end_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    dialog->pending_event = event_end_dialog;
    free( dialog->pending_argument );
    dialog->pending_argument = wcsdup( argument );
    return ERROR_SUCCESS;
}

/* transition from one modal dialog to another modal dialog */
static UINT event_new_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    /* store the name of the next dialog, and signal this one to end */
    dialog->package->next_dialog = wcsdup( argument );
    msi_event_cleanup_all_subscriptions( dialog->package );
    msi_dialog_end_dialog( dialog );
    return ERROR_SUCCESS;
}

static UINT pending_event_new_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    dialog->pending_event = event_new_dialog;
    free( dialog->pending_argument );
    dialog->pending_argument = wcsdup( argument );
    return ERROR_SUCCESS;
}

/* create a new child dialog of an existing modal dialog */
static UINT event_spawn_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    INT r;
    /* don't destroy a modeless dialogs that might be our parent */
    r = event_do_dialog( dialog->package, argument, dialog, FALSE );
    if (r != 0)
    {
        dialog->retval = r;
        msi_dialog_end_dialog( dialog );
    }
    else
        msi_dialog_update_all_controls(dialog);

    return ERROR_SUCCESS;
}

static UINT pending_event_spawn_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    dialog->pending_event = event_spawn_dialog;
    free( dialog->pending_argument );
    dialog->pending_argument = wcsdup( argument );
    return ERROR_SUCCESS;
}

/* creates a dialog that remains up for a period of time based on a condition */
static UINT event_spawn_wait_dialog( msi_dialog *dialog, const WCHAR *argument )
{
    FIXME("doing nothing\n");
    return ERROR_SUCCESS;
}

static UINT event_do_action( msi_dialog *dialog, const WCHAR *argument )
{
    ACTION_PerformAction(dialog->package, argument);
    return ERROR_SUCCESS;
}

static UINT event_add_local( msi_dialog *dialog, const WCHAR *argument )
{
    MSIFEATURE *feature;

    LIST_FOR_EACH_ENTRY( feature, &dialog->package->features, MSIFEATURE, entry )
    {
        if (!wcscmp( argument, feature->Feature ) || !wcscmp( argument, L"ALL" ))
        {
            if (feature->ActionRequest != INSTALLSTATE_LOCAL)
                msi_set_property( dialog->package->db, L"Preselected", L"1", -1 );
            MSI_SetFeatureStateW( dialog->package, feature->Feature, INSTALLSTATE_LOCAL );
        }
    }
    return ERROR_SUCCESS;
}

static UINT event_remove( msi_dialog *dialog, const WCHAR *argument )
{
    MSIFEATURE *feature;

    LIST_FOR_EACH_ENTRY( feature, &dialog->package->features, MSIFEATURE, entry )
    {
        if (!wcscmp( argument, feature->Feature ) || !wcscmp( argument, L"ALL" ))
        {
            if (feature->ActionRequest != INSTALLSTATE_ABSENT)
                msi_set_property( dialog->package->db, L"Preselected", L"1", -1 );
            MSI_SetFeatureStateW( dialog->package, feature->Feature, INSTALLSTATE_ABSENT );
        }
    }
    return ERROR_SUCCESS;
}

static UINT event_add_source( msi_dialog *dialog, const WCHAR *argument )
{
    MSIFEATURE *feature;

    LIST_FOR_EACH_ENTRY( feature, &dialog->package->features, MSIFEATURE, entry )
    {
        if (!wcscmp( argument, feature->Feature ) || !wcscmp( argument, L"ALL" ))
        {
            if (feature->ActionRequest != INSTALLSTATE_SOURCE)
                msi_set_property( dialog->package->db, L"Preselected", L"1", -1 );
            MSI_SetFeatureStateW( dialog->package, feature->Feature, INSTALLSTATE_SOURCE );
        }
    }
    return ERROR_SUCCESS;
}

void msi_event_fire( MSIPACKAGE *package, const WCHAR *event, MSIRECORD *rec )
{
    struct subscriber *sub;

    TRACE("firing event %s\n", debugstr_w(event));

    LIST_FOR_EACH_ENTRY( sub, &package->subscriptions, struct subscriber, entry )
    {
        if (wcsicmp( sub->event, event )) continue;
        dialog_handle_event( sub->dialog, sub->control, sub->attribute, rec );
    }
}

static UINT event_set_target_path( msi_dialog *dialog, const WCHAR *argument )
{
    WCHAR *path = msi_dup_property( dialog->package->db, argument );
    MSIRECORD *rec = MSI_CreateRecord( 1 );
    UINT r = ERROR_SUCCESS;

    MSI_RecordSetStringW( rec, 1, path );
    msi_event_fire( dialog->package, L"SelectionPath", rec );
    if (path)
    {
        /* failure to set the path halts the executing of control events */
        r = MSI_SetTargetPathW( dialog->package, argument, path );
        free( path );
    }
    free( &rec->hdr );
    return r;
}

static UINT event_reset( msi_dialog *dialog, const WCHAR *argument )
{
    msi_dialog_reset( dialog );
    return ERROR_SUCCESS;
}

INT ACTION_ShowDialog( MSIPACKAGE *package, const WCHAR *dialog )
{
    MSIRECORD *row;
    INT rc;

    if (!TABLE_Exists(package->db, L"Dialog")) return 0;

    row = MSI_CreateRecord(0);
    if (!row) return -1;
    MSI_RecordSetStringW(row, 0, dialog);
    rc = MSI_ProcessMessage(package, INSTALLMESSAGE_SHOWDIALOG, row);
    msiobj_release(&row->hdr);

    if (rc == -2) rc = 0;

    if (!rc)
    {
        MSIRECORD *row = MSI_CreateRecord(2);
        if (!row) return -1;
        MSI_RecordSetInteger(row, 1, 2726);
        MSI_RecordSetStringW(row, 2, dialog);
        MSI_ProcessMessage(package, INSTALLMESSAGE_INFO, row);

        msiobj_release(&row->hdr);
    }
    return rc;
}

INT ACTION_DialogBox( MSIPACKAGE *package, const WCHAR *dialog )
{
    INT r;

    if (package->next_dialog) ERR("Already got next dialog... ignoring it\n");
    package->next_dialog = NULL;

    /* Dialogs are chained through NewDialog, which sets the next_dialog member.
     * We fall out of the loop if we reach a modeless dialog, which immediately
     * returns IDOK, or an EndDialog event, which returns the value corresponding
     * to its argument.
     */
    r = event_do_dialog( package, dialog, NULL, TRUE );
    while (package->next_dialog)
    {
        WCHAR *name = package->next_dialog;

        package->next_dialog = NULL;
        r = event_do_dialog( package, name, NULL, TRUE );
        free( name );
    }
    return r;
}

static UINT event_set_install_level( msi_dialog *dialog, const WCHAR *argument )
{
    int level = wcstol( argument, NULL, 10 );

    TRACE("setting install level to %d\n", level);
    return MSI_SetInstallLevel( dialog->package, level );
}

static UINT event_directory_list_up( msi_dialog *dialog, const WCHAR *argument )
{
    return msi_dialog_directorylist_up( dialog );
}

static UINT event_directory_list_new( msi_dialog *dialog, const WCHAR *argument )
{
    return msi_dialog_directorylist_new( dialog );
}

static UINT event_reinstall_mode( msi_dialog *dialog, const WCHAR *argument )
{
    return msi_set_property( dialog->package->db, L"REINSTALLMODE", argument, -1 );
}

static UINT event_reinstall( msi_dialog *dialog, const WCHAR *argument )
{
    return msi_set_property( dialog->package->db, L"REINSTALL", argument, -1 );
}

static UINT event_validate_product_id( msi_dialog *dialog, const WCHAR *argument )
{
    return msi_validate_product_id( dialog->package );
}

static const struct control_event control_events[] =
{
    { L"EndDialog", pending_event_end_dialog },
    { L"NewDialog", pending_event_new_dialog },
    { L"SpawnDialog", pending_event_spawn_dialog },
    { L"SpawnWaitDialog", event_spawn_wait_dialog },
    { L"DoAction", event_do_action },
    { L"AddLocal", event_add_local },
    { L"Remove", event_remove },
    { L"AddSource", event_add_source },
    { L"SetTargetPath", event_set_target_path },
    { L"Reset", event_reset },
    { L"SetInstallLevel", event_set_install_level },
    { L"DirectoryListUp", event_directory_list_up },
    { L"DirectoryListNew", event_directory_list_new },
    { L"SelectionBrowse", event_spawn_dialog },
    { L"ReinstallMode", event_reinstall_mode },
    { L"Reinstall", event_reinstall },
    { L"ValidateProductID", event_validate_product_id },
    { NULL, NULL }
};

static UINT dialog_event_handler( msi_dialog *dialog, const WCHAR *event, const WCHAR *argument )
{
    unsigned int i;

    TRACE("handling event %s\n", debugstr_w(event));

    if (!event) return ERROR_SUCCESS;

    for (i = 0; control_events[i].event; i++)
    {
        if (!wcscmp( control_events[i].event, event ))
            return control_events[i].handler( dialog, argument );
    }
    FIXME("unhandled event %s arg(%s)\n", debugstr_w(event), debugstr_w(argument));
    return ERROR_SUCCESS;
}

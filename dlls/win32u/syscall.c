/*
 * Unix interface for Win32 syscalls
 *
 * Copyright (C) 2021 Alexandre Julliard
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

#if 0
#pragma makedep unix
#endif

#include <stdarg.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "ntgdi_private.h"
#include "ntuser.h"
#include "wine/unixlib.h"


static void * const syscalls[] =
{
    NtGdiAddFontMemResourceEx,
    NtGdiAddFontResourceW,
    NtGdiCombineRgn,
    NtGdiCreateBitmap,
    NtGdiCreateClientObj,
    NtGdiCreateDIBBrush,
    NtGdiCreateDIBSection,
    NtGdiCreateEllipticRgn,
    NtGdiCreateHalftonePalette,
    NtGdiCreateHatchBrushInternal,
    NtGdiCreatePaletteInternal,
    NtGdiCreatePatternBrushInternal,
    NtGdiCreatePen,
    NtGdiCreateRectRgn,
    NtGdiCreateRoundRectRgn,
    NtGdiCreateSolidBrush,
    NtGdiDdDDICreateDevice,
    NtGdiDdDDIOpenAdapterFromHdc,
    NtGdiDdDDIQueryStatistics,
    NtGdiDdDDISetQueuedLimit,
    NtGdiDeleteClientObj,
    NtGdiDescribePixelFormat,
    NtGdiDrawStream,
    NtGdiEqualRgn,
    NtGdiExtCreatePen,
    NtGdiExtCreateRegion,
    NtGdiExtGetObjectW,
    NtGdiFlattenPath,
    NtGdiFlush,
    NtGdiGetBitmapBits,
    NtGdiGetBitmapDimension,
    NtGdiGetColorAdjustment,
    NtGdiGetDCDword,
    NtGdiGetDCObject,
    NtGdiGetDCPoint,
    NtGdiGetFontFileData,
    NtGdiGetFontFileInfo,
    NtGdiGetNearestPaletteIndex,
    NtGdiGetPath,
    NtGdiGetRegionData,
    NtGdiGetRgnBox,
    NtGdiGetSpoolMessage,
    NtGdiGetSystemPaletteUse,
    NtGdiGetTransform,
    NtGdiHfontCreate,
    NtGdiInitSpool,
    NtGdiOffsetRgn,
    NtGdiPathToRegion,
    NtGdiPtInRegion,
    NtGdiRectInRegion,
    NtGdiRemoveFontMemResourceEx,
    NtGdiRemoveFontResourceW,
    NtGdiSaveDC,
    NtGdiSetBitmapBits,
    NtGdiSetBitmapDimension,
    NtGdiSetBrushOrg,
    NtGdiSetColorAdjustment,
    NtGdiSetMagicColors,
    NtGdiSetMetaRgn,
    NtGdiSetPixelFormat,
    NtGdiSetRectRgn,
    NtGdiSetTextJustification,
    NtGdiSetVirtualResolution,
    NtGdiSwapBuffers,
    NtGdiTransformPoints,
    NtUserActivateKeyboardLayout,
    NtUserAddClipboardFormatListener,
    NtUserAssociateInputContext,
    NtUserAttachThreadInput,
    NtUserBuildHwndList,
    NtUserCallMsgFilter,
    NtUserCheckMenuItem,
    NtUserChildWindowFromPointEx,
    NtUserCloseDesktop,
    NtUserCloseWindowStation,
    NtUserCopyAcceleratorTable,
    NtUserCreateAcceleratorTable,
    NtUserCreateDesktopEx,
    NtUserCreateInputContext,
    NtUserCreateWindowStation,
    NtUserDeleteMenu,
    NtUserDestroyAcceleratorTable,
    NtUserDestroyInputContext,
    NtUserDispatchMessage,
    NtUserEndMenu,
    NtUserFindExistingCursorIcon,
    NtUserFindWindowEx,
    NtUserGetAncestor,
    NtUserGetAsyncKeyState,
    NtUserGetAtomName,
    NtUserGetCaretBlinkTime,
    NtUserGetCaretPos,
    NtUserGetClassName,
    NtUserGetClipboardFormatName,
    NtUserGetClipboardOwner,
    NtUserGetClipboardSequenceNumber,
    NtUserGetClipboardViewer,
    NtUserGetCursor,
    NtUserGetCursorFrameInfo,
    NtUserGetCursorInfo,
    NtUserGetDoubleClickTime,
    NtUserGetDpiForMonitor,
    NtUserGetForegroundWindow,
    NtUserGetGUIThreadInfo,
    NtUserGetIconSize,
    NtUserGetKeyNameText,
    NtUserGetKeyState,
    NtUserGetKeyboardLayout,
    NtUserGetKeyboardLayoutList,
    NtUserGetKeyboardLayoutName,
    NtUserGetKeyboardState,
    NtUserGetLayeredWindowAttributes,
    NtUserGetMenuItemRect,
    NtUserGetMessage,
    NtUserGetMouseMovePointsEx,
    NtUserGetObjectInformation,
    NtUserGetOpenClipboardWindow,
    NtUserGetProcessDpiAwarenessContext,
    NtUserGetProcessWindowStation,
    NtUserGetProp,
    NtUserGetQueueStatus,
    NtUserGetRawInputBuffer,
    NtUserGetRawInputData,
    NtUserGetRawInputDeviceInfo,
    NtUserGetRawInputDeviceList,
    NtUserGetRegisteredRawInputDevices,
    NtUserGetSystemDpiForProcess,
    NtUserGetThreadDesktop,
    NtUserGetTitleBarInfo,
    NtUserGetWindowRgnEx,
    NtUserInitializeClientPfnArrays,
    NtUserInternalGetWindowText,
    NtUserKillTimer,
    NtUserLockWindowUpdate,
    NtUserMapVirtualKeyEx,
    NtUserMenuItemFromPoint,
    NtUserMessageCall,
    NtUserMsgWaitForMultipleObjectsEx,
    NtUserNotifyWinEvent,
    NtUserOpenDesktop,
    NtUserOpenInputDesktop,
    NtUserOpenWindowStation,
    NtUserPeekMessage,
    NtUserPostMessage,
    NtUserPostThreadMessage,
    NtUserQueryInputContext,
    NtUserRegisterHotKey,
    NtUserRegisterRawInputDevices,
    NtUserRemoveClipboardFormatListener,
    NtUserRemoveMenu,
    NtUserRemoveProp,
    NtUserSendInput,
    NtUserSetCursorPos,
    NtUserSetKeyboardState,
    NtUserSetMenuContextHelpId,
    NtUserSetMenuDefaultItem,
    NtUserSetObjectInformation,
    NtUserSetProcessDpiAwarenessContext,
    NtUserSetProcessWindowStation,
    NtUserSetProp,
    NtUserSetSystemTimer,
    NtUserSetThreadDesktop,
    NtUserSetTimer,
    NtUserSetWinEventHook,
    NtUserSetWindowsHookEx,
    NtUserThunkedMenuInfo,
    NtUserThunkedMenuItemInfo,
    NtUserToUnicodeEx,
    NtUserTrackPopupMenuEx,
    NtUserTranslateMessage,
    NtUserUnhookWinEvent,
    NtUserUnhookWindowsHookEx,
    NtUserUnregisterHotKey,
    NtUserUpdateInputContext,
    NtUserVkKeyScanEx,
    NtUserWaitForInputIdle,
    NtUserWindowFromDC,
};

static BYTE arguments[ARRAY_SIZE(syscalls)];

static SYSTEM_SERVICE_TABLE syscall_table =
{
    (ULONG_PTR *)syscalls,
    0,
    ARRAY_SIZE(syscalls),
    arguments
};

static NTSTATUS init( void *dispatcher )
{
    NTSTATUS status;
    if ((status = ntdll_init_syscalls( 1, &syscall_table, dispatcher ))) return status;
    if ((status = gdi_init())) return status;
    winstation_init();
    sysparams_init();
    return STATUS_SUCCESS;
}

unixlib_entry_t __wine_unix_call_funcs[] =
{
    init,
    callbacks_init,
};

#ifdef _WIN64

WINE_DEFAULT_DEBUG_CHANNEL(win32u);

static NTSTATUS wow64_init( void *args )
{
    FIXME( "\n" );
    return STATUS_NOT_SUPPORTED;
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    init,
    wow64_init,
};

#endif /* _WIN64 */

/* **********************************************************
 * Copyright (c) 2011-2012 Google, Inc.  All rights reserved.
 * **********************************************************/

/* Dr. Memory: the memory debugger
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; 
 * version 2.1 of the License, and no later version.

 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Library General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* Need this defined and to the latest to get the latest defines and types */
#define _WIN32_WINNT 0x0601 /* == _WIN32_WINNT_WIN7 */
#define WINVER _WIN32_WINNT

#include "dr_api.h"
#include "drmemory.h"
#include "syscall.h"
#include "syscall_os.h"
#include "syscall_windows.h"
#include "readwrite.h"
#include "shadow.h"
#include <stddef.h> /* offsetof */

/* for NtGdi* syscalls */
#include <wingdi.h> /* usually from windows.h; required by winddi.h + ENUMLOGFONTEXDVW */
#define NT_BUILD_ENVIRONMENT 1 /* for d3dnthal.h */
#include <d3dnthal.h>
#include <winddi.h> /* required by ntgdityp.h and prntfont.h */
#include <prntfont.h>
#include "../wininc/ntgdityp.h"
#include <ntgdi.h>
#include <winspool.h> /* for DRIVER_INFO_2W */
#include <dxgiformat.h> /* for DXGI_FORMAT */

/* for NtUser* syscalls */
#include "../wininc/ndk_extypes.h" /* required by ntuser.h */
#include "../wininc/ntuser.h"

#define OK (SYSINFO_ALL_PARAMS_KNOWN)
#define UNKNOWN 0
#define W (SYSARG_WRITE)
#define R (SYSARG_READ)
#define CT (SYSARG_COMPLEX_TYPE)
#define WI (SYSARG_WRITE | SYSARG_LENGTH_INOUT)
#define IB (SYSARG_INLINED_BOOLEAN)
#define RET (SYSARG_POST_SIZE_RETVAL)

/***************************************************************************/
/* System calls with wrappers in kernel32.dll (on win7 these are duplicated
 * in kernelbase.dll as well but w/ the same syscall number)
 * Not all wrappers are exported: xref i#388.
 */
syscall_info_t syscall_kernel32_info[] = {
    /* wchar_t *locale OUT, size_t locale_sz (assuming size in bytes) */
    {0,"NtWow64CsrBasepNlsGetUserInfo", OK, 2, {{0,-1,W|CT,SYSARG_TYPE_CSTRING_WIDE}, }},

    /* Takes a single param that's a pointer to a struct that has a PHANDLE at offset
     * 0x7c where the base of a new mmap is stored by the kernel.  We handle that by
     * waiting for RtlCreateActivationContext (i#352).  We don't know of any written
     * values in the rest of the struct or its total size so we ignore it for now and
     * use this entry to avoid "unknown syscall" warnings.
     *
     * XXX: there are 4+ wchar_t* input strings in the struct: should check them.
     */
    {0,"NtWow64CsrBasepCreateActCtx", OK, 1, },
};
#define NUM_KERNEL32_SYSCALLS \
    (sizeof(syscall_kernel32_info)/sizeof(syscall_kernel32_info[0]))

size_t
num_kernel32_syscalls(void)
{
    return NUM_KERNEL32_SYSCALLS;
}

/***************************************************************************/
/* System calls with wrappers in user32.dll.
 * Not all wrappers are exported: xref i#388.
 *
 * Initially obtained via mksystable.pl on ntuser.h.
 * That version was checked in separately to track manual changes.
 *
 * When adding new entries, use the NtUser prefix.
 * When we try to find the wrapper via symbol lookup we try with
 * and without the prefix.
 *
 * Unresolved issues are marked w/ FIXME in the table.
 */

static int sysnum_UserSystemParametersInfo = -1;
static int sysnum_UserMenuInfo = -1;
static int sysnum_UserMenuItemInfo = -1;
static int sysnum_UserGetAltTabInfo = -1;
static int sysnum_UserGetRawInputBuffer = -1;
static int sysnum_UserGetRawInputData = -1;
static int sysnum_UserGetRawInputDeviceInfo = -1;
static int sysnum_UserTrackMouseEvent = -1;
static int sysnum_UserLoadKeyboardLayoutEx = -1;
static int sysnum_UserCreateWindowStation = -1;
static int sysnum_UserGetDC = -1;
static int sysnum_UserGetDCEx = -1;
static int sysnum_UserGetWindowDC = -1;
static int sysnum_UserBeginPaint = -1;
static int sysnum_UserEndPaint = -1;
static int sysnum_UserReleaseDC = -1;

/* forward decl so "extern" */
extern syscall_info_t syscall_usercall_info[];

/* Table that maps usercall names to (un-combined) numbers.
 * Number can be 0 so we store +1.
 */
#define USERCALL_TABLE_HASH_BITS 8
static hashtable_t usercall_table;


syscall_info_t syscall_user32_info[] = {
    {0,"NtUserActivateKeyboardLayout", OK, 2, },
    {0,"NtUserAlterWindowStyle", OK, 3, },
    {0,"NtUserAssociateInputContext", OK|SYSINFO_IMM32_DLL, 3, },
    {0,"NtUserAttachThreadInput", OK, 3, },
    {0,"NtUserBeginPaint", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 2, {{1,sizeof(PAINTSTRUCT),W,}, }, &sysnum_UserBeginPaint},
    {0,"NtUserBitBltSysBmp", OK, 8, },
    {0,"NtUserBlockInput", OK, 1, },
    {0,"NtUserBuildHimcList", OK|SYSINFO_IMM32_DLL, 4, {{2,-1,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(HIMC)}, {3,sizeof(UINT),W}, }},
    {0,"NtUserBuildHwndList", OK, 7, {{2,0,IB,}, {5,-6,WI|SYSARG_SIZE_IN_ELEMENTS,sizeof(HWND)}, {6,sizeof(ULONG),R|W,}, }},
    {0,"NtUserBuildMenuItemList", OK, 4, {{1,-2,W,}, }},
    {0,"NtUserBuildNameList", OK, 4, {{2,-1,W,}, {2,-3,WI,}, {3,sizeof(ULONG),W,}, }},
    {0,"NtUserBuildPropList", OK, 4, {{1,-2,W,}, {1,-3,WI,}, {3,sizeof(DWORD),W,}, }},
    {0,"NtUserCalcMenuBar", OK, 5, },
    /* FIXME i#389: NtUserCall* take in a code and perform a variety of tasks */
    {0,"NtUserCallHwnd", OK|SYSINFO_SECONDARY_TABLE, 2, {{1,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallHwndLock", OK|SYSINFO_SECONDARY_TABLE, 2, {{1,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallHwndOpt", OK|SYSINFO_SECONDARY_TABLE, 2, {{1,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallHwndParam", OK|SYSINFO_SECONDARY_TABLE, 3, {{2,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallHwndParamLock", OK|SYSINFO_SECONDARY_TABLE, 3, {{2,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallMsgFilter", UNKNOWN, 2, {{0,sizeof(MSG),R|W,}, }},
    {0,"NtUserCallNextHookEx", UNKNOWN, 4, },
    {0,"NtUserCallNoParam", OK|SYSINFO_SECONDARY_TABLE, 1, {{0,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallOneParam", OK|SYSINFO_SECONDARY_TABLE, 2, {{1,}}, (int*)syscall_usercall_info},
    {0,"NtUserCallTwoParam", OK|SYSINFO_SECONDARY_TABLE, 3, {{2,}}, (int*)syscall_usercall_info},
    {0,"NtUserChangeClipboardChain", OK, 2, },
    {0,"NtUserChangeDisplaySettings", OK, 5, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {1,sizeof(DEVMODEW)/*really var-len*/,R|CT,SYSARG_TYPE_DEVMODEW}, {4,-5,W,}, }},
    {0,"NtUserCheckDesktopByThreadId", OK, 1, },
    {0,"NtUserCheckImeHotKey", OK, 2, },
    {0,"NtUserCheckMenuItem", OK, 3, },
    {0,"NtUserCheckWindowThreadDesktop", OK, 3, },
    {0,"NtUserChildWindowFromPointEx", OK, 4, },
    {0,"NtUserClipCursor", OK, 1, {{0,sizeof(RECT),R,}, }},
    {0,"NtUserCloseClipboard", OK, 0, },
    {0,"NtUserCloseDesktop", OK, 1, },
    {0,"NtUserCloseWindowStation", OK, 1, },
    {0,"NtUserConsoleControl", OK, 3, },
    {0,"NtUserConvertMemHandle", OK, 2, },
    {0,"NtUserCopyAcceleratorTable", OK, 3, {{1,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(ACCEL)}, }},
    {0,"NtUserCountClipboardFormats", OK, 0, },
    {0,"NtUserCreateAcceleratorTable", OK|SYSINFO_CREATE_HANDLE, 2, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(ACCEL)}, }},
    {0,"NtUserCreateCaret", OK, 4, },
    {0,"NtUserCreateDesktop", OK|SYSINFO_CREATE_HANDLE, 5, {{0,sizeof(OBJECT_ATTRIBUTES),R|CT,SYSARG_TYPE_OBJECT_ATTRIBUTES}, {1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,sizeof(DEVMODEW)/*really var-len*/,R|CT,SYSARG_TYPE_DEVMODEW}, }},
    {0,"NtUserCreateInputContext", OK|SYSINFO_CREATE_HANDLE|SYSINFO_IMM32_DLL, 1, },
    {0,"NtUserCreateLocalMemHandle", OK|SYSINFO_CREATE_HANDLE, 4, {{1,-2,W}, {3,sizeof(UINT),W}, }},
    {0,"NtUserCreateWindowEx", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 15, {{1,sizeof(LARGE_STRING),R|CT,SYSARG_TYPE_LARGE_STRING}, {2,sizeof(LARGE_STRING),R|CT,SYSARG_TYPE_LARGE_STRING}, {3,sizeof(LARGE_STRING),R|CT,SYSARG_TYPE_LARGE_STRING}, }},
    {0,"NtUserCreateWindowStation", OK|SYSINFO_CREATE_HANDLE, 7, {{0,sizeof(OBJECT_ATTRIBUTES),R|CT,SYSARG_TYPE_OBJECT_ATTRIBUTES}, }, &sysnum_UserCreateWindowStation},
    {0,"NtUserCtxDisplayIOCtl", OK, 3, },
    {0,"NtUserDdeGetQualityOfService", OK, 3, {{2,sizeof(SECURITY_QUALITY_OF_SERVICE),W,}, }},
    {0,"NtUserDdeInitialize", OK, 5, },
    {0,"NtUserDdeSetQualityOfService", OK, 3, {{1,sizeof(SECURITY_QUALITY_OF_SERVICE),R,}, {2,sizeof(SECURITY_QUALITY_OF_SERVICE),W,}, }},
    {0,"NtUserDefSetText", OK, 2, {{1,sizeof(LARGE_STRING),R|CT,SYSARG_TYPE_LARGE_STRING}, }},
    {0,"NtUserDeferWindowPos", OK, 8, },
    {0,"NtUserDeleteMenu", OK, 3, },
    {0,"NtUserDestroyAcceleratorTable", OK, 1, },
    {0,"NtUserDestroyCursor", OK, 2, },
    {0,"NtUserDestroyInputContext", OK|SYSINFO_IMM32_DLL, 1, },
    {0,"NtUserDestroyMenu", OK, 1, },
    {0,"NtUserDestroyWindow", OK, 1, },
    {0,"NtUserDisableThreadIme", OK|SYSINFO_IMM32_DLL, 1, },
    {0,"NtUserDispatchMessage", OK, 1, {{0,sizeof(MSG),R,}, }},
    {0,"NtUserDragDetect", OK, 2, },
    {0,"NtUserDragObject", OK, 5, },
    {0,"NtUserDrawAnimatedRects", OK, 4, {{2,sizeof(RECT),R,}, {3,sizeof(RECT),R,}, }},
    {0,"NtUserDrawCaption", OK, 4, {{2,sizeof(RECT),R,}, }},
    {0,"NtUserDrawCaptionTemp", OK, 7, {{2,sizeof(RECT),R,}, {5,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserDrawIconEx", OK, 11, /*XXX: 10th arg is pointer?*/ },
    {0,"NtUserDrawMenuBarTemp", OK, 5, {{2,sizeof(RECT),R,}, }},
    {0,"NtUserEmptyClipboard", OK, 0, },
    {0,"NtUserEnableMenuItem", OK, 3, },
    {0,"NtUserEnableScrollBar", OK, 3, },
    {0,"NtUserEndDeferWindowPosEx", OK, 2, },
    {0,"NtUserEndMenu", OK, 0, },
    {0,"NtUserEndPaint", OK, 2, {{1,sizeof(PAINTSTRUCT),R,}, }},
    {0,"NtUserEnumDisplayDevices", OK, 4, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,SYSARG_SIZE_IN_FIELD,W,offsetof(DISPLAY_DEVICEW,cb)}, }},
    {0,"NtUserEnumDisplayMonitors", OK, 5, {{1,sizeof(RECT),R,},/*experimentally this matches win32 API version so no more mem args*/ }},
    {0,"NtUserEnumDisplaySettings", OK, 4, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,sizeof(DEVMODEW)/*really var-len*/,W|CT,SYSARG_TYPE_DEVMODEW}, }},
    {0,"NtUserEvent", OK, 1, },
    {0,"NtUserExcludeUpdateRgn", OK, 2, },
    {0,"NtUserFillWindow", OK, 4, },
    {0,"NtUserFindExistingCursorIcon", OK, 4, },
    {0,"NtUserFindWindowEx", OK, 5, {{2,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {3,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserFlashWindowEx", OK, 1, {{0,SYSARG_SIZE_IN_FIELD,R,offsetof(FLASHWINFO,cbSize)}, }},
    {0,"NtUserGetAltTabInfo", OK, 6, {{2,SYSARG_SIZE_IN_FIELD,W,offsetof(ALTTABINFO,cbSize)}, /*buffer is ansi or unicode so special-cased*/}, &sysnum_UserGetAltTabInfo},
    {0,"NtUserGetAncestor", OK, 2, },
    {0,"NtUserGetAppImeLevel", OK|SYSINFO_IMM32_DLL, 1, },
    {0,"NtUserGetAsyncKeyState", OK, 1, },
    {0,"NtUserGetAtomName", OK, 2, {{1,sizeof(UNICODE_STRING),W|CT,SYSARG_TYPE_UNICODE_STRING_NOLEN/*i#490*/}, }},
    {0,"NtUserGetCPD", OK, 3, },
    {0,"NtUserGetCaretBlinkTime", OK, 0, },
    {0,"NtUserGetCaretPos", OK, 1, {{0,sizeof(POINT),W,}, }},
    {0,"NtUserGetClassInfo", OK, 5, {{1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,sizeof(WNDCLASSEXW),W|CT,SYSARG_TYPE_WNDCLASSEXW}, {3,sizeof(PWSTR)/*pointer to existing string (ansi or unicode) is copied*/,W,}, }},
    {0,"NtUserGetClassInfoEx", OK, 5, {{1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,sizeof(WNDCLASSEXW),W|CT,SYSARG_TYPE_WNDCLASSEXW}, {3,sizeof(PWSTR)/*pointer to existing string (ansi or unicode) is copied*/,W,}, }},
    {0,"NtUserGetClassLong", OK, 3, },
    {0,"NtUserGetClassName", OK, 3, {{2,sizeof(UNICODE_STRING),W|CT,SYSARG_TYPE_UNICODE_STRING_NOLEN/*i#490*/}, }},
    {0,"NtUserGetClipCursor", OK, 1, {{0,sizeof(RECT),W,}, }},
    /* FIXME i#487: exact layout of returned struct is not known */
    {0,"NtUserGetClipboardData", OK, 2, {{1,12,W,}, }},
    {0,"NtUserGetClipboardFormatName", OK, 3, {{1,sizeof(UNICODE_STRING),W|CT,SYSARG_TYPE_UNICODE_STRING}, /*3rd param is max count but should be able to ignore*/}},
    {0,"NtUserGetClipboardOwner", OK, 0, },
    {0,"NtUserGetClipboardSequenceNumber", OK, 0, },
    {0,"NtUserGetClipboardViewer", OK, 0, },
    {0,"NtUserGetComboBoxInfo", OK, 2, {{1,SYSARG_SIZE_IN_FIELD,W,offsetof(COMBOBOXINFO,cbSize)}, }},
    {0,"NtUserGetControlBrush", OK, 3, },
    {0,"NtUserGetControlColor", OK, 4, },
    {0,"NtUserGetCursorFrameInfo", OK, 4, },
    {0,"NtUserGetCursorInfo", OK, 1, {{0,SYSARG_SIZE_IN_FIELD,W,offsetof(CURSORINFO,cbSize)}, }},
    {0,"NtUserGetDC", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 1, {{0,}}, &sysnum_UserGetDC},
    {0,"NtUserGetDCEx", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 3, {{0,}}, &sysnum_UserGetDCEx},
    {0,"NtUserGetDoubleClickTime", OK, 0, },
    {0,"NtUserGetForegroundWindow", OK, 0, },
    {0,"NtUserGetGUIThreadInfo", OK, 2, {{1,SYSARG_SIZE_IN_FIELD,W,offsetof(GUITHREADINFO,cbSize)}, }},
    {0,"NtUserGetGuiResources", OK, 2, },
    {0,"NtUserGetIconInfo", OK, 6, {{1,sizeof(ICONINFO),W,}, {2,sizeof(UNICODE_STRING),W|CT,SYSARG_TYPE_UNICODE_STRING_NOLEN/*i#490*/}, {3,sizeof(UNICODE_STRING),W|CT,SYSARG_TYPE_UNICODE_STRING}, {4,sizeof(DWORD),W,}, }},
    {0,"NtUserGetIconSize", OK, 4, {{2,sizeof(LONG),W,}, {3,sizeof(LONG),W,}, }},
    {0,"NtUserGetImeHotKey", OK, 4, },
    /* FIXME i#487: 1st param is OUT but shape is unknown */
    {0,"NtUserGetImeInfoEx", UNKNOWN|SYSINFO_IMM32_DLL, 2, },
    {0,"NtUserGetInternalWindowPos", OK, 3, {{1,sizeof(RECT),W,}, {2,sizeof(POINT),W,}, }},
    {0,"NtUserGetKeyNameText", OK, 3, {{1,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {1,RET,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, }},
    {0,"NtUserGetKeyState", OK, 1, },
    {0,"NtUserGetKeyboardLayout", OK, 1, },
    {0,"NtUserGetKeyboardLayoutList", OK, 2, {{1,-0,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(HKL)}, {1,RET,W|SYSARG_NO_WRITE_IF_COUNT_0|SYSARG_SIZE_IN_ELEMENTS,sizeof(HKL)}, }},
    {0,"NtUserGetKeyboardLayoutName", OK, 1, {{0,KL_NAMELENGTH*sizeof(wchar_t),W|CT,SYSARG_TYPE_CSTRING_WIDE}, }},
    {0,"NtUserGetKeyboardState", OK, 1, {{0,sizeof(BYTE),W,}, }},
    {0,"NtUserGetKeyboardType", OK, 1, },
    {0,"NtUserGetLastInputInfo", OK, 1, {{0,SYSARG_SIZE_IN_FIELD,W,offsetof(LASTINPUTINFO,cbSize)}, }},
    {0,"NtUserGetLayeredWindowAttributes", OK, 4, {{1,sizeof(COLORREF),W,}, {2,sizeof(BYTE),W,}, {3,sizeof(DWORD),W,}, }},
    {0,"NtUserGetListBoxInfo", OK, 1, },
    {0,"NtUserGetMenuBarInfo", OK, 4, {{3,SYSARG_SIZE_IN_FIELD,W,offsetof(MENUBARINFO,cbSize)}, }},
    {0,"NtUserGetMenuDefaultItem", OK, 3, },
    {0,"NtUserGetMenuIndex", OK, 2, },
    {0,"NtUserGetMenuItemRect", OK, 4, {{3,sizeof(RECT),W,}, }},
    {0,"NtUserGetMessage", OK, 4, {{0,sizeof(MSG),W,}, }},
    {0,"NtUserGetMinMaxInfo", OK, 3, {{1,sizeof(MINMAXINFO),W,}, }},
    {0,"NtUserGetMonitorInfo", OK, 2, {{1,SYSARG_SIZE_IN_FIELD,W,offsetof(MONITORINFO,cbSize)}, }},
    {0,"NtUserGetMouseMovePointsEx", OK, 5, {{1,-0,R,}, {2,-3,W|SYSARG_SIZE_IN_ELEMENTS,-0}, }},
    {0,"NtUserGetObjectInformation", OK|SYSINFO_RET_SMALL_WRITE_LAST, 5, {{2,-3,W}, {2,-4,WI}, {4,sizeof(DWORD),W}, }},
    {0,"NtUserGetOpenClipboardWindow", OK, 0, },
    {0,"NtUserGetPriorityClipboardFormat", OK, 2, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(UINT)}, }},
    {0,"NtUserGetProcessWindowStation", OK, 0, },
    {0,"NtUserGetRawInputBuffer", OK, 3, {{0,}}, /*special-cased; FIXME: i#485: see handler*/ &sysnum_UserGetRawInputBuffer},
    {0,"NtUserGetRawInputData", OK, 5, {{2,-3,WI,}, {2,RET,W}, /*arg 3 is R or W => special-cased*/ }, &sysnum_UserGetRawInputData},
    {0,"NtUserGetRawInputDeviceInfo", OK, 4, {{0,}}, &sysnum_UserGetRawInputDeviceInfo},
    {0,"NtUserGetRawInputDeviceList", OK, 3, {{0,-1,WI|SYSARG_SIZE_IN_ELEMENTS,-2}, {1,sizeof(UINT),R|W,/*really not written when #0!=NULL but harmless; ditto below and probably elsewhere in table*/}, }},
    {0,"NtUserGetRegisteredRawInputDevices", OK, 3, {{0,-1,WI|SYSARG_SIZE_IN_ELEMENTS,-2}, {1,sizeof(UINT),R|W,}, }},
    {0,"NtUserGetScrollBarInfo", OK, 3, {{2,SYSARG_SIZE_IN_FIELD,W,offsetof(SCROLLBARINFO,cbSize)}, }},
    {0,"NtUserGetSystemMenu", OK, 2, },
    /* FIXME i#487: on WOW64 XP and Vista (but not win7) this makes a 0x2xxx syscall
     * instead of invoking NtUserGetThreadDesktop: is it really different?
     */
    {0,"NtUserGetThreadDesktop", OK|SYSINFO_REQUIRES_PREFIX, 2, },
    {0,"GetThreadDesktop", OK, 2, },
    {0,"NtUserGetThreadState", OK, 1, },
    {0,"NtUserGetTitleBarInfo", OK, 2, {{1,SYSARG_SIZE_IN_FIELD,W,offsetof(TITLEBARINFO,cbSize)}, }},
    {0,"NtUserGetUpdateRect", OK, 3, {{1,sizeof(RECT),W,}, }},
    {0,"NtUserGetUpdateRgn", OK, 3, },
    {0,"NtUserGetWOWClass", OK, 2, {{1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserGetWindowDC", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 1, {{0,}}, &sysnum_UserGetWindowDC},
    {0,"NtUserGetWindowPlacement", OK, 2, {{1,SYSARG_SIZE_IN_FIELD,W,offsetof(WINDOWPLACEMENT,length)}, }},
    {0,"NtUserHardErrorControl", OK, 3, },
    {0,"NtUserHideCaret", OK, 1, },
    {0,"NtUserHiliteMenuItem", OK, 4, },
    {0,"NtUserImpersonateDdeClientWindow", OK, 2, },
    {0,"NtUserInitTask", OK, 12, },
    {0,"NtUserInitialize", OK, 3, },
    /* FIXME i#487: not sure whether these are arrays and if so how long they are */
    {0,"NtUserInitializeClientPfnArrays", UNKNOWN, 4, {{0,sizeof(PFNCLIENT),R,}, {1,sizeof(PFNCLIENT),R,}, {2,sizeof(PFNCLIENTWORKER),R,}, }},
    {0,"NtUserInternalGetWindowText", OK, 3, {{1,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)},{1,0,W|CT,SYSARG_TYPE_CSTRING_WIDE}, }},
    {0,"NtUserInvalidateRect", OK, 3, {{1,sizeof(RECT),R,}, }},
    {0,"NtUserInvalidateRgn", OK, 3, },
    {0,"NtUserIsClipboardFormatAvailable", OK, 1, },
    {0,"NtUserKillTimer", OK, 2, },
    {0,"NtUserLoadKeyboardLayoutEx", OK, 7, {{2,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {4,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }, &sysnum_UserLoadKeyboardLayoutEx},
    {0,"NtUserLockWindowStation", OK, 1, },
    {0,"NtUserLockWindowUpdate", OK, 1, },
    {0,"NtUserLockWorkStation", OK, 0, },
    {0,"NtUserMNDragLeave", OK, 0, },
    {0,"NtUserMNDragOver", OK, 2, },
    {0,"NtUserMapVirtualKeyEx", OK, 4, },
    {0,"NtUserMenuInfo", OK, 3, {{0,}/*can be R or W*/}, &sysnum_UserMenuInfo },
    {0,"NtUserMenuItemFromPoint", OK, 4, },
    {0,"NtUserMenuItemInfo", OK, 5, {{0,}/*can be R or W*/}, &sysnum_UserMenuItemInfo },
    {0,"NtUserMessageCall", OK, 7, },
    {0,"NtUserMinMaximize", OK, 3, },
    {0,"NtUserModifyUserStartupInfoFlags", OK, 2, },
    {0,"NtUserMonitorFromPoint", OK, 2, },
    {0,"NtUserMonitorFromRect", OK, 2, {{0,sizeof(RECT),R,}, }},
    {0,"NtUserMonitorFromWindow", OK, 2, },
    {0,"NtUserMoveWindow", OK, 6, },
    {0,"NtUserNotifyIMEStatus", OK, 3, },
    {0,"NtUserNotifyProcessCreate", OK, 4, },
    {0,"NtUserNotifyWinEvent", OK, 4, },
    {0,"NtUserOpenClipboard", OK, 2, },
    {0,"NtUserOpenDesktop", OK, 3, {{0,sizeof(OBJECT_ATTRIBUTES),R|CT,SYSARG_TYPE_OBJECT_ATTRIBUTES}, }},
    {0,"NtUserOpenInputDesktop", OK, 3, },
    {0,"NtUserOpenWindowStation", OK, 2, {{0,sizeof(OBJECT_ATTRIBUTES),R|CT,SYSARG_TYPE_OBJECT_ATTRIBUTES}, }},
    {0,"NtUserPaintDesktop", OK, 1, },
    {0,"NtUserPaintMenuBar", OK, 6, },
    {0,"NtUserPeekMessage", OK, 5, {{0,sizeof(MSG),W,}, }},
    {0,"NtUserPostMessage", OK, 4, },
    {0,"NtUserPostThreadMessage", OK, 4, },
    {0,"NtUserPrintWindow", OK, 3, },
    /* FIXME i#487: lots of pointers inside USERCONNECT */
    {0,"NtUserProcessConnect", UNKNOWN, 3, {{1,sizeof(USERCONNECT),W,}, }},
    {0,"NtUserQueryInformationThread", OK, 5, },
    {0,"NtUserQueryInputContext", OK|SYSINFO_IMM32_DLL, 2, },
    {0,"NtUserQuerySendMessage", OK, 1, },
    {0,"NtUserQueryUserCounters", OK, 5, },
    {0,"NtUserQueryWindow", OK, 2, },
    {0,"NtUserRealChildWindowFromPoint", OK, 3, },
    {0,"NtUserRealInternalGetMessage", OK, 6, {{0,sizeof(MSG),W,}, }},
    {0,"NtUserRealWaitMessageEx", OK, 2, },
    {0,"NtUserRedrawWindow", OK, 4, {{1,sizeof(RECT),R,}, }},
    {0,"NtUserRegisterClassExWOW", OK|SYSINFO_RET_ZERO_FAIL, 7, {{0,sizeof(WNDCLASSEXW),R|CT,SYSARG_TYPE_WNDCLASSEXW}, {1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {3,sizeof(CLSMENUNAME),R|CT,SYSARG_TYPE_CLSMENUNAME}, {6,sizeof(DWORD),R,}, }},
    {0,"NtUserRegisterHotKey", OK, 4, },
    {0,"NtUserRegisterRawInputDevices", OK, 3, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,-2}, }},
    {0,"NtUserRegisterTasklist", OK, 1, },
    {0,"NtUserRegisterUserApiHook", OK, 4, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserRegisterWindowMessage", OK, 1, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserRemoteConnect", OK, 3, },
    {0,"NtUserRemoteRedrawRectangle", OK, 4, },
    {0,"NtUserRemoteRedrawScreen", OK, 0, },
    {0,"NtUserRemoteStopScreenUpdates", OK, 0, },
    {0,"NtUserRemoveMenu", OK, 3, },
    {0,"NtUserRemoveProp", OK, 2, },
    {0,"NtUserResolveDesktop", OK, 4, },
    {0,"NtUserResolveDesktopForWOW", OK, 1, },
    /* FIXME i#487: not sure whether #2 is in or out */
    {0,"NtUserSBGetParms", OK, 4, {{2,sizeof(SBDATA),W,}, {3,SYSARG_SIZE_IN_FIELD,W,offsetof(SCROLLINFO,cbSize)}, }},
    {0,"NtUserScrollDC", OK, 7, {{3,sizeof(RECT),R,}, {4,sizeof(RECT),R,}, {6,sizeof(RECT),W,}, }},
    {0,"NtUserScrollWindowEx", OK, 8, {{3,sizeof(RECT),R,}, {4,sizeof(RECT),R,}, {6,sizeof(RECT),W,}, }},
    {0,"NtUserSelectPalette", OK, 3, },
    {0,"NtUserSendInput", OK, 3, {{1,-0,R|SYSARG_SIZE_IN_ELEMENTS,-2}, }},
    {0,"NtUserSetActiveWindow", OK, 1, },
    {0,"NtUserSetAppImeLevel", OK|SYSINFO_IMM32_DLL, 2, },
    {0,"NtUserSetCapture", OK, 1, },
    {0,"NtUserSetClassLong", OK, 4, },
    {0,"NtUserSetClassWord", OK, 3, },
    {0,"NtUserSetClipboardData", OK, 3, },
    {0,"NtUserSetClipboardViewer", OK, 1, },
    {0,"NtUserSetConsoleReserveKeys", OK, 2, },
    {0,"NtUserSetCursor", OK, 1, },
    {0,"NtUserSetCursorContents", OK, 2, {{1,sizeof(ICONINFO),R,}, }},
    {0,"NtUserSetCursorIconData", OK, 6, {{1,sizeof(BOOL),R,}, {2,sizeof(POINT),R,}, }},
    {0,"NtUserSetDbgTag", OK, 2, },
    {0,"NtUserSetFocus", OK, 1, },
    {0,"NtUserSetImeHotKey", OK, 5, },
    {0,"NtUserSetImeInfoEx", OK|SYSINFO_IMM32_DLL, 1, },
    {0,"NtUserSetImeOwnerWindow", OK, 2, },
    {0,"NtUserSetInformationProcess", OK, 4, },
    {0,"NtUserSetInformationThread", OK, 4, },
    {0,"NtUserSetInternalWindowPos", OK, 4, {{2,sizeof(RECT),R,}, {3,sizeof(POINT),R,}, }},
    {0,"NtUserSetKeyboardState", OK, 1, {{0,256*sizeof(BYTE),R,}, }},
    {0,"NtUserSetLayeredWindowAttributes", OK, 4, },
    {0,"NtUserSetLogonNotifyWindow", OK, 1, },
    {0,"NtUserSetMenu", OK, 3, },
    {0,"NtUserSetMenuContextHelpId", OK, 2, },
    {0,"NtUserSetMenuDefaultItem", OK, 3, },
    {0,"NtUserSetMenuFlagRtoL", OK, 1, },
    {0,"NtUserSetObjectInformation", OK, 4, {{2,-3,R,}, }},
    {0,"NtUserSetParent", OK, 2, },
    {0,"NtUserSetProcessWindowStation", OK, 1, },
    {0,"NtUserSetProp", OK, 3, },
    {0,"NtUserSetRipFlags", OK, 2, },
    {0,"NtUserSetScrollBarInfo", OK, 3, {{2,sizeof(SETSCROLLBARINFO),R,}, }},
    {0,"NtUserSetScrollInfo", OK, 4, {{2,SYSARG_SIZE_IN_FIELD,R,offsetof(SCROLLINFO,cbSize)}, }},
    {0,"NtUserSetShellWindowEx", OK, 2, },
    {0,"NtUserSetSysColors", OK, 4, {{1,-0,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(INT)}, {2,-0,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(COLORREF)}, }},
    {0,"NtUserSetSystemCursor", OK, 2, },
    {0,"NtUserSetSystemMenu", OK, 2, },
    {0,"NtUserSetSystemTimer", OK, 4, },
    {0,"NtUserSetThreadDesktop", OK, 1, },
    {0,"NtUserSetThreadLayoutHandles", OK|SYSINFO_IMM32_DLL, 2, },
    {0,"NtUserSetThreadState", OK, 2, },
    {0,"NtUserSetTimer", OK, 4, },
    {0,"NtUserSetWinEventHook", OK, 8, {{3,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserSetWindowFNID", OK, 2, },
    {0,"NtUserSetWindowLong", OK, 4, },
    {0,"NtUserSetWindowPlacement", OK, 2, {{1,SYSARG_SIZE_IN_FIELD,R,offsetof(WINDOWPLACEMENT,length)}, }},
    {0,"NtUserSetWindowPos", OK, 7, },
    {0,"NtUserSetWindowRgn", OK, 3, },
    {0,"NtUserSetWindowStationUser", OK, 4, },
    {0,"NtUserSetWindowWord", OK, 3, },
    {0,"NtUserSetWindowsHookAW", OK, 3, },
    {0,"NtUserSetWindowsHookEx", OK, 6, {{1,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserShowCaret", OK, 1, },
    {0,"NtUserShowScrollBar", OK, 3, },
    {0,"NtUserShowWindow", OK, 2, },
    {0,"NtUserShowWindowAsync", OK, 2, },
    {0,"NtUserSoundSentry", OK, 0, },
    {0,"NtUserSwitchDesktop", OK, 1, },
    {0,"NtUserSystemParametersInfo", OK, 1/*rest are optional*/, {{0,},/*special-cased*/ }, &sysnum_UserSystemParametersInfo},
    {0,"NtUserTestForInteractiveUser", OK, 1, },
    /* there is a pointer in MENUINFO but it's user-defined */
    {0,"NtUserThunkedMenuInfo", OK, 2, {{1,sizeof(MENUINFO),R,}, }},
    {0,"NtUserThunkedMenuItemInfo", OK, 6, {{4,0,R|CT,SYSARG_TYPE_MENUITEMINFOW}, {5,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, }},
    {0,"NtUserToUnicodeEx", OK, 7, {{2,0x100*sizeof(BYTE),R,}, {3,-4,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, }},
    {0,"NtUserTrackMouseEvent", OK, 1, {{0,}}, &sysnum_UserTrackMouseEvent},
    {0,"NtUserTrackPopupMenuEx", OK, 6, {{5,SYSARG_SIZE_IN_FIELD,R,offsetof(TPMPARAMS,cbSize)}, }},
    {0,"NtUserTranslateAccelerator", OK, 3, {{2,sizeof(MSG),R,}, }},
    {0,"NtUserTranslateMessage", OK, 2, {{0,sizeof(MSG),R,}, }},
    {0,"NtUserUnhookWinEvent", OK, 1, },
    {0,"NtUserUnhookWindowsHookEx", OK, 1, },
    {0,"NtUserUnloadKeyboardLayout", OK, 1, },
    {0,"NtUserUnlockWindowStation", OK, 1, },
    /* FIXME i#487: CLSMENUNAME format is not fully known */
    {0,"NtUserUnregisterClass", UNKNOWN, 3, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING}, {2,sizeof(CLSMENUNAME),W|CT,SYSARG_TYPE_CLSMENUNAME,}, }},
    {0,"NtUserUnregisterHotKey", OK, 2, },
    {0,"NtUserUnregisterUserApiHook", OK, 0, },
    {0,"NtUserUpdateInputContext", OK, 3, },
    {0,"NtUserUpdateInstance", OK, 3, },
    {0,"NtUserUpdateLayeredWindow", OK, 10, {{2,sizeof(POINT),R,}, {3,sizeof(SIZE),R,}, {5,sizeof(POINT),R,}, {7,sizeof(BLENDFUNCTION),R,}, {9,sizeof(RECT),R,}, }},
    {0,"NtUserUpdatePerUserSystemParameters", OK, 2, },
    {0,"NtUserUserHandleGrantAccess", OK, 3, },
    {0,"NtUserValidateHandleSecure", OK, 2, },
    {0,"NtUserValidateRect", OK, 2, {{1,sizeof(RECT),R,}, }},
    {0,"NtUserValidateTimerCallback", OK, 3, },
    {0,"NtUserVkKeyScanEx", OK, 3, },
    {0,"NtUserWaitForInputIdle", OK, 3, },
    {0,"NtUserWaitForMsgAndEvent", OK, 1, },
    {0,"NtUserWaitMessage", OK, 0, },
    {0,"NtUserWin32PoolAllocationStats", OK, 6, },
    {0,"NtUserWindowFromPhysicalPoint", OK, 1, },
    {0,"NtUserWindowFromPoint", OK, 2, },
    {0,"NtUserYieldTask", OK, 0, },

    {0,"NtUserUserConnectToServer", OK, 3, {{0,0,R|CT,SYSARG_TYPE_CSTRING_WIDE}, {1,-2,WI}, {2,sizeof(ULONG),R}, }},
    {0,"NtUserGetProp", OK, 2, },

};
#define NUM_USER32_SYSCALLS \
    (sizeof(syscall_user32_info)/sizeof(syscall_user32_info[0]))

size_t
num_user32_syscalls(void)
{
    return NUM_USER32_SYSCALLS;
}

/***************************************************************************
 * NtUserCall* secondary system call numbers
 */

#define NONE -1

static const char * const usercall_names[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   #type"."#name,
#include "syscall_usercallx.h"
#undef USERCALL
};
#define NUM_USERCALL_NAMES (sizeof(usercall_names)/sizeof(usercall_names[0]))

static const char * const usercall_primary[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   #type,
#include "syscall_usercallx.h"
#undef USERCALL
};

static const int win7_usercall_nums[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   w7,
#include "syscall_usercallx.h"
#undef USERCALL
};

static const int winvistaSP2_usercall_nums[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   vistaSP2,
#include "syscall_usercallx.h"
#undef USERCALL
};

static const int winvistaSP01_usercall_nums[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   vistaSP01,
#include "syscall_usercallx.h"
#undef USERCALL
};

static const int win2003_usercall_nums[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   w2003,
#include "syscall_usercallx.h"
#undef USERCALL
};

static const int winxp_usercall_nums[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   xp,
#include "syscall_usercallx.h"
#undef USERCALL
};

static const int win2k_usercall_nums[] = {
#define USERCALL(type, name, w7, vistaSP2, vistaSP01, w2003, xp, w2k)   w2k,
#include "syscall_usercallx.h"
#undef USERCALL
};

/* Secondary system calls for NtUserCall{No,One,Two}Param */
syscall_info_t syscall_usercall_info[] = {
    {0,"NtUserCallNoParam.CREATEMENU", OK, 1, },
    {0,"NtUserCallNoParam.CREATEMENUPOPUP", OK, 1, },
    {0,"NtUserCallNoParam.DISABLEPROCWNDGHSTING", OK, 1, },
    {0,"NtUserCallNoParam.MSQCLEARWAKEMASK", OK, 1, },
    {0,"NtUserCallNoParam.ALLOWFOREGNDACTIVATION", OK, 1, },
    {0,"NtUserCallNoParam.CREATESYSTEMTHREADS", OK, 1, },
    {0,"NtUserCallNoParam.UNKNOWN", UNKNOWN, 1, },
    {0,"NtUserCallNoParam.DESTROY_CARET", OK, 1, },
    {0,"NtUserCallNoParam.GETDEVICECHANGEINFO", OK, 1, },
    {0,"NtUserCallNoParam.GETIMESHOWSTATUS", OK, 1, },
    {0,"NtUserCallNoParam.GETINPUTDESKTOP", OK, 1, },
    {0,"NtUserCallNoParam.GETMSESSAGEPOS", OK, 1, },
    {0,"NtUserCallNoParam.GETREMOTEPROCID", OK, 1, },
    {0,"NtUserCallNoParam.HIDECURSORNOCAPTURE", OK, 1, },
    {0,"NtUserCallNoParam.LOADCURSANDICOS", OK, 1, },
    {0,"NtUserCallNoParam.PREPAREFORLOGOFF", OK, 1, },
    {0,"NtUserCallNoParam.RELEASECAPTURE", OK, 1, },
    {0,"NtUserCallNoParam.RESETDBLCLICK", OK, 1, },
    {0,"NtUserCallNoParam.ZAPACTIVEANDFOUS", OK, 1, },
    {0,"NtUserCallNoParam.REMOTECONSHDWSTOP", OK, 1, },
    {0,"NtUserCallNoParam.REMOTEDISCONNECT", OK, 1, },
    {0,"NtUserCallNoParam.REMOTELOGOFF", OK, 1, },
    {0,"NtUserCallNoParam.REMOTENTSECURITY", OK, 1, },
    {0,"NtUserCallNoParam.REMOTESHDWSETUP", OK, 1, },
    {0,"NtUserCallNoParam.REMOTESHDWSTOP", OK, 1, },
    {0,"NtUserCallNoParam.REMOTEPASSTHRUENABLE", OK, 1, },
    {0,"NtUserCallNoParam.REMOTEPASSTHRUDISABLE", OK, 1, },
    {0,"NtUserCallNoParam.REMOTECONNECTSTATE", OK, 1, },
    {0,"NtUserCallNoParam.UPDATEPERUSERIMMENABLING", OK, 1, },
    {0,"NtUserCallNoParam.USERPWRCALLOUTWORKER", OK, 1, },
    {0,"NtUserCallNoParam.WAKERITFORSHTDWN", OK, 1, },
    {0,"NtUserCallNoParam.INIT_MESSAGE_PUMP", OK, 1, },
    {0,"NtUserCallNoParam.UNINIT_MESSAGE_PUMP", OK, 1, },
    {0,"NtUserCallNoParam.LOADUSERAPIHOOK", OK, 1, },

    {0,"NtUserCallOneParam.BEGINDEFERWNDPOS", OK, 2, /*int count.  allocates memory but in the kernel*/},
    {0,"NtUserCallOneParam.GETSENDMSGRECVR", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.WINDOWFROMDC", OK, 2, /*HDC*/},
    {0,"NtUserCallOneParam.ALLOWSETFOREGND", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.CREATEEMPTYCUROBJECT", OK, 2, /*unused*/},
    {0,"NtUserCallOneParam.CREATESYSTEMTHREADS", OK, 2, /*UINT*/},
    {0,"NtUserCallOneParam.CSDDEUNINITIALIZE", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.DIRECTEDYIELD", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.ENUMCLIPBOARDFORMATS", OK, 2, /*UINT*/},
    {0,"NtUserCallOneParam.GETCURSORPOS", OK, 2, {{0,sizeof(POINTL),W},}},
    {0,"NtUserCallOneParam.GETINPUTEVENT", OK, 2, /*DWORD*/},
    {0,"NtUserCallOneParam.GETKEYBOARDLAYOUT", OK, 2, /*DWORD*/},
    {0,"NtUserCallOneParam.GETKEYBOARDTYPE", OK, 2, /*DWORD*/},
    {0,"NtUserCallOneParam.GETPROCDEFLAYOUT", OK, 2, {{0,sizeof(DWORD),W},}},
    {0,"NtUserCallOneParam.GETQUEUESTATUS", OK, 2, /*DWORD*/},
    {0,"NtUserCallOneParam.GETWINSTAINFO", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.HANDLESYSTHRDCREATFAIL", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.LOCKFOREGNDWINDOW", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.LOADFONTS", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.MAPDEKTOPOBJECT", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.MESSAGEBEEP", OK, 2, /*LPARAM*/},
    {0,"NtUserCallOneParam.PLAYEVENTSOUND", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.POSTQUITMESSAGE", OK, 2, /*int exit code*/},
    {0,"NtUserCallOneParam.PREPAREFORLOGOFF", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.REALIZEPALETTE", OK, 2, /*HDC*/},
    {0,"NtUserCallOneParam.REGISTERLPK", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.REGISTERSYSTEMTHREAD", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.REMOTERECONNECT", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.REMOTETHINWIRESTATUS", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.RELEASEDC", OK|SYSINFO_DELETE_HANDLE, 2, /*HDC*/{{0,}}, &sysnum_UserReleaseDC},
    {0,"NtUserCallOneParam.REMOTENOTIFY", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.REPLYMESSAGE", OK, 2, /*LRESULT*/},
    {0,"NtUserCallOneParam.SETCARETBLINKTIME", OK, 2, /*UINT*/},
    {0,"NtUserCallOneParam.SETDBLCLICKTIME", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.SETIMESHOWSTATUS", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.SETMESSAGEEXTRAINFO", OK, 2, /*LPARAM*/},
    {0,"NtUserCallOneParam.SETPROCDEFLAYOUT", OK, 2, /*DWORD for PROCESSINFO.dwLayout*/},
    {0,"NtUserCallOneParam.SETWATERMARKSTRINGS", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.SHOWCURSOR", OK, 2, /*BOOL*/},
    {0,"NtUserCallOneParam.SHOWSTARTGLASS", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.SWAPMOUSEBUTTON", OK, 2, /*BOOL*/},

    {0,"NtUserCallOneParam.UNKNOWN", UNKNOWN, 2, },
    {0,"NtUserCallOneParam.UNKNOWN", UNKNOWN, 2, },

    {0,"NtUserCallHwnd.DEREGISTERSHELLHOOKWINDOW", OK, 2, /*HWND*/},
    {0,"NtUserCallHwnd.DWP_GETENABLEDPOPUP", UNKNOWN, 2, },
    {0,"NtUserCallHwnd.GETWNDCONTEXTHLPID", OK, 2, /*HWND*/},
    {0,"NtUserCallHwnd.REGISTERSHELLHOOKWINDOW", OK, 2, /*HWND*/},
    {0,"NtUserCallHwnd.UNKNOWN", UNKNOWN, 2, },

    {0,"NtUserCallHwndOpt.SETPROGMANWINDOW", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndOpt.SETTASKMANWINDOW", OK, 2, /*HWND*/},

    {0,"NtUserCallHwndParam.GETCLASSICOCUR", UNKNOWN, 3, },
    {0,"NtUserCallHwndParam.CLEARWINDOWSTATE", UNKNOWN, 3, },
    {0,"NtUserCallHwndParam.KILLSYSTEMTIMER", OK, 3, /*HWND, timer id*/},
    {0,"NtUserCallHwndParam.SETDIALOGPOINTER", OK, 3, /*HWND, BOOL*/ },
    {0,"NtUserCallHwndParam.SETVISIBLE", UNKNOWN, 3, },
    {0,"NtUserCallHwndParam.SETWNDCONTEXTHLPID", OK, 3, /*HWND, HANDLE*/},
    {0,"NtUserCallHwndParam.SETWINDOWSTATE", UNKNOWN, 3, },

    /* XXX: confirm the rest: assuming for now all just take HWND */
    {0,"NtUserCallHwndLock.WINDOWHASSHADOW", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.ARRANGEICONICWINDOWS", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.DRAWMENUBAR", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.CHECKIMESHOWSTATUSINTHRD", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.GETSYSMENUHANDLE", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.REDRAWFRAME", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.REDRAWFRAMEANDHOOK", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.SETDLGSYSMENU", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.SETFOREGROUNDWINDOW", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.SETSYSMENU", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.UPDATECKIENTRECT", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.UPDATEWINDOW", OK, 2, /*HWND*/},
    {0,"NtUserCallHwndLock.UNKNOWN", UNKNOWN, 2, },

    {0,"NtUserCallTwoParam.ENABLEWINDOW", OK, 3, /*HWND, BOOL*/},
    {0,"NtUserCallTwoParam.REDRAWTITLE", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.SHOWOWNEDPOPUPS", OK, 3, /*HWND, BOOL*/},
    {0,"NtUserCallTwoParam.SWITCHTOTHISWINDOW", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.UPDATEWINDOWS", UNKNOWN, 3, },

    {0,"NtUserCallHwndParamLock.VALIDATERGN", OK, 3, /*HWND, HRGN*/},

    {0,"NtUserCallTwoParam.CHANGEWNDMSGFILTER", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.GETCURSORPOS", OK, 3, {{0,sizeof(POINTL),W},}/*other param is hardcoded as 0x1*/},
    /* XXX i#996: not 100% sure there's not more nuanced behavior to
     * this syscall.  First param looks like flags and 3rd looks like
     * size of buffer.
     */
    {0,"NtUserCallTwoParam.GETHDEVNAME", OK, 3, {{1,-2,W},}},
    {0,"NtUserCallTwoParam.INITANSIOEM", OK, 3, {{1,0,W|CT,SYSARG_TYPE_CSTRING_WIDE},}},
    {0,"NtUserCallTwoParam.NLSSENDIMENOTIFY", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.REGISTERGHSTWND", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.REGISTERLOGONPROCESS", OK, 3, /*HANDLE, BOOL*/},
    {0,"NtUserCallTwoParam.REGISTERSYSTEMTHREAD", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.REGISTERSBLFROSTWND", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.REGISTERUSERHUNGAPPHANDLERS", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.SHADOWCLEANUP", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.REMOTESHADOWSTART", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.SETCARETPOS", OK, 3, /*int, int*/},
    {0,"NtUserCallTwoParam.SETCURSORPOS", OK, 3, /*int, int*/},
    {0,"NtUserCallTwoParam.SETPHYSCURSORPOS", UNKNOWN, 3, },
    {0,"NtUserCallTwoParam.UNHOOKWINDOWSHOOK", OK, 3, /*int, HOOKPROC*/},
    {0,"NtUserCallTwoParam.WOWCLEANUP", UNKNOWN, 3, },
};
#define NUM_USERCALL_SYSCALLS \
    (sizeof(syscall_usercall_info)/sizeof(syscall_usercall_info[0]))

size_t
num_usercall_syscalls(void)
{
    return NUM_USERCALL_SYSCALLS;
}

void
syscall_wingdi_init(void *drcontext, app_pc ntdll_base, dr_os_version_info_t *ver)
{
    uint i;
    const int *usercalls;
    LOG(1, "Windows version is %d.%d.%d\n", ver->version, ver->service_pack_major,
        ver->service_pack_minor);
    switch (ver->version) {
    case DR_WINDOWS_VERSION_7:     usercalls = win7_usercall_nums;     break;
    case DR_WINDOWS_VERSION_VISTA: {
        if (ver->service_pack_major >= 2)
            usercalls = winvistaSP2_usercall_nums;
        else
            usercalls = winvistaSP01_usercall_nums;
        break;
    }
    case DR_WINDOWS_VERSION_2003:  usercalls = win2003_usercall_nums;  break;
    case DR_WINDOWS_VERSION_XP:    usercalls = winxp_usercall_nums;    break;
    case DR_WINDOWS_VERSION_2000:  usercalls = win2k_usercall_nums;    break;
    case DR_WINDOWS_VERSION_NT:
    default:
        usage_error("This version of Windows is not supported", "");
    }

    /* Set up hashtable to translate usercall names to numbers */
    hashtable_init(&usercall_table, USERCALL_TABLE_HASH_BITS,
                   HASH_STRING, false/*!strdup*/);
    for (i = 0; i < NUM_USERCALL_NAMES; i++) {
        if (usercalls[i] != NONE) {
            IF_DEBUG(bool ok =)
                hashtable_add(&usercall_table, (void *)usercall_names[i],
                              (void *)(usercalls[i] + 1/*avoid 0*/));
            ASSERT(ok, "no dup entries in usercall_table");
        }
    }
    ASSERT(NUM_USERCALL_NAMES == NUM_USERCALL_SYSCALLS, "mismatch in usercall tables");

    if (options.check_gdi)
        gdicheck_init();
}

void
syscall_wingdi_exit(void)
{
    if (options.check_gdi)
        gdicheck_exit();

    hashtable_delete(&usercall_table);
}

void
syscall_wingdi_thread_init(void *drcontext)
{
    if (options.check_gdi)
        gdicheck_thread_init(drcontext);
}

void
syscall_wingdi_thread_exit(void *drcontext)
{
    if (options.check_gdi)
        gdicheck_thread_exit(drcontext);
}

void
syscall_wingdi_user32_load(void *drcontext, const module_data_t *info)
{
    uint i;
    for (i = 0; i < NUM_USERCALL_SYSCALLS; i++) {
        syscall_info_t *syslist = &syscall_usercall_info[i];
        uint secondary = (uint)
            hashtable_lookup(&usercall_table, (void *)syslist->name);
        if (secondary != 0) {
            /* no reason to support syscall_num_from_name() and it's simple
             * enough to directly add rather than add more complexity to
             * add_syscall_entry()
             */
            uint primary = get_syscall_num(drcontext, info, usercall_primary[i]);
            syslist->num = SYSNUM_COMBINE(primary, secondary - 1/*+1 in table*/);
            hashtable_add(&systable, (void *) syslist->num, (void *) syslist);
            if (syslist->num_out != NULL)
                *syslist->num_out = syslist->num;
            LOG(SYSCALL_VERBOSE, "usercall %-35s = %3d (0x%04x)\n",
                syslist->name, syslist->num, syslist->num);
        } else {
            LOG(SYSCALL_VERBOSE, "WARNING: could not find usercall %s\n", syslist->name);
        }
    }
}

/***************************************************************************/
/* System calls with wrappers in gdi32.dll.
 * Not all wrappers are exported: xref i#388.
 *
 * When adding new entries, use the NtGdi prefix.
 * When we try to find the wrapper via symbol lookup we try with
 * and without the prefix.
 *
 * Initially obtained via mksystable.pl on VS2008 ntgdi.h.
 * That version was checked in separately to track manual changes.
 *
 * FIXME i#485: issues with table that are not yet resolved:
 *
 * + OUT params with no size where size comes from prior syscall
 *   return value (see FIXMEs in table below): so have to watch pairs
 *   of calls (but what if app is able to compute max size some other
 *   way, maybe caching older call?), unless willing to only check for
 *   unaddr in post-syscall and thus after potential write to
 *   unaddressable memory by kernel (which is what we do today).
 *   Update: there are some of these in NtUser table as well.
 *
 * + missing ", return" annotations: NtGdiExtGetObjectW was missing one,
 *   and I'm afraid other ones that return int or UINT may also.
 *
 * + __out PVOID: for NtGdiGetUFIPathname and NtGdiDxgGenericThunk,
 *   is the PVOID that's written supposed to have a bcount (or ecount)
 *   annotation?  for now treated as PVOID*.
 *
 * + bcount in, ecount out for NtGdiSfmGetNotificationTokens (which is
 *   missing annotations)?  but what is size of token?
 *
 * + the REALIZATION_INFO struct is much larger on win7
 */

static int sysnum_GdiCreatePaletteInternal = -1;
static int sysnum_GdiCheckBitmapBits = -1;
static int sysnum_GdiCreateDIBSection = -1;
static int sysnum_GdiHfontCreate = -1;
static int sysnum_GdiDoPalette = -1;
static int sysnum_GdiExtTextOutW = -1;
static int sysnum_GdiOpenDCW = -1;
static int sysnum_GdiGetDCforBitmap = -1;
static int sysnum_GdiDdGetDC = -1;
static int sysnum_GdiDeleteObjectApp = -1;
static int sysnum_GdiCreateMetafileDC = -1;
static int sysnum_GdiCreateCompatibleDC = -1;

syscall_info_t syscall_gdi32_info[] = {
    {0,"NtGdiInit", OK, 0, },
    {0,"NtGdiSetDIBitsToDeviceInternal", OK, 16, {{9,-12,R,}, {10,sizeof(BITMAPINFO),R,}, }},
    {0,"NtGdiGetFontResourceInfoInternalW", OK, 7, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {4,sizeof(DWORD),W,}, {5,-3,W,}, }},
    {0,"NtGdiGetGlyphIndicesW", OK, 5, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {3,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(WORD)}, }},
    {0,"NtGdiGetGlyphIndicesWInternal", OK, 6, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {3,sizeof(WORD),W,}, }},
    {0,"NtGdiCreatePaletteInternal", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 2, {{0,},}/*too complex: special-cased*/, &sysnum_GdiCreatePaletteInternal},
    {0,"NtGdiArcInternal", OK, 10, },
    {0,"NtGdiGetOutlineTextMetricsInternalW", OK, 4, {{2,-1,W,}, {3,sizeof(TMDIFF),W,}, }},
    {0,"NtGdiGetAndSetDCDword", OK, 4, {{3,sizeof(DWORD),W,}, }},
    {0,"NtGdiGetDCObject", OK, 2, },
    {0,"NtGdiGetDCforBitmap", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 1, {{0,}}, &sysnum_GdiGetDCforBitmap},
    {0,"NtGdiGetMonitorID", OK, 3, {{2,-1,W,}, }},
    {0,"NtGdiGetLinkedUFIs", OK, 3, {{1,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(UNIVERSAL_FONT_ID)}, }},
    {0,"NtGdiSetLinkedUFIs", OK, 3, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(UNIVERSAL_FONT_ID)}, }},
    {0,"NtGdiGetUFI", OK, 6, {{1,sizeof(UNIVERSAL_FONT_ID),W,}, {2,sizeof(DESIGNVECTOR),W,}, {3,sizeof(ULONG),W,}, {4,sizeof(ULONG),W,}, {5,sizeof(FLONG),W,}, }},
    {0,"NtGdiForceUFIMapping", OK, 2, {{1,sizeof(UNIVERSAL_FONT_ID),R,}, }},
    {0,"NtGdiGetUFIPathname", OK, 10, {{0,sizeof(UNIVERSAL_FONT_ID),R,}, {1,sizeof(ULONG),W,}, {2,MAX_PATH * 3,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {2,-1,WI|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {3,sizeof(ULONG),W,}, {5,sizeof(BOOL),W,}, {6,sizeof(ULONG),W,}, {7,sizeof(PVOID),W,}, {8,sizeof(BOOL),W,}, {9,sizeof(ULONG),W,}, }},
    {0,"NtGdiAddRemoteFontToDC", OK, 4, {{3,sizeof(UNIVERSAL_FONT_ID),R,}, }},
    {0,"NtGdiAddFontMemResourceEx", OK, 5, {{2,-3,R,}, {4,sizeof(DWORD),W,}, }},
    {0,"NtGdiRemoveFontMemResourceEx", OK, 1, },
    {0,"NtGdiUnmapMemFont", OK, 1, },
    {0,"NtGdiRemoveMergeFont", OK, 2, {{1,sizeof(UNIVERSAL_FONT_ID),R,}, }},
    {0,"NtGdiAnyLinkedFonts", OK, 0, },
    {0,"NtGdiGetEmbUFI", OK, 7, {{1,sizeof(UNIVERSAL_FONT_ID),W,}, {2,sizeof(DESIGNVECTOR),W,}, {3,sizeof(ULONG),W,}, {4,sizeof(ULONG),W,}, {5,sizeof(FLONG),W,}, {6,sizeof(KERNEL_PVOID),W,}, }},
    {0,"NtGdiGetEmbedFonts", OK, 0, },
    {0,"NtGdiChangeGhostFont", OK, 2, {{0,sizeof(KERNEL_PVOID),R,}, }},
    {0,"NtGdiAddEmbFontToDC", OK, 2, {{1,sizeof(PVOID),R,}, }},
    {0,"NtGdiFontIsLinked", OK, 1, },
    {0,"NtGdiPolyPolyDraw", OK, 5, {{1,sizeof(POINT),R,}, {2,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(ULONG)}, }},
    {0,"NtGdiDoPalette", OK, 6, {{0,},},/*special-cased: R or W depending*/ &sysnum_GdiDoPalette},
    {0,"NtGdiComputeXformCoefficients", OK, 1, },
    {0,"NtGdiGetWidthTable", OK, 7, {{2,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(WCHAR)}, {4,-3,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(USHORT)}, {5,sizeof(WIDTHDATA),W,}, {6,sizeof(FLONG),W,}, }},
    {0,"NtGdiDescribePixelFormat", OK, 4, {{3,-2,W,}, }},
    {0,"NtGdiSetPixelFormat", OK, 2, },
    {0,"NtGdiSwapBuffers", OK, 1, },
    {0,"NtGdiDxgGenericThunk", OK, 6, {{2,sizeof(SIZE_T),R|W,}, {3,sizeof(PVOID),R|W,}, {4,sizeof(SIZE_T),R|W,}, {5,sizeof(PVOID),R|W,}, }},
    {0,"NtGdiDdAddAttachedSurface", OK, 3, {{2,sizeof(DD_ADDATTACHEDSURFACEDATA),R|W,}, }},
    {0,"NtGdiDdAttachSurface", OK, 2, },
    {0,"NtGdiDdBlt", OK, 3, {{2,sizeof(DD_BLTDATA),R|W,}, }},
    {0,"NtGdiDdCanCreateSurface", OK, 2, {{1,sizeof(DD_CANCREATESURFACEDATA),R|W,}, }},
    {0,"NtGdiDdColorControl", OK, 2, {{1,sizeof(DD_COLORCONTROLDATA),R|W,}, }},
    {0,"NtGdiDdCreateDirectDrawObject", OK, 1, },
    {0,"NtGdiDdCreateSurface", OK|SYSINFO_CREATE_HANDLE, 8, {{1,sizeof(HANDLE),SYSARG_IS_HANDLE|R,}, {2,sizeof(DDSURFACEDESC),R|W,}, {3,sizeof(DD_SURFACE_GLOBAL),R|W,}, {4,sizeof(DD_SURFACE_LOCAL),R|W,}, {5,sizeof(DD_SURFACE_MORE),R|W,}, {6,sizeof(DD_CREATESURFACEDATA),R|W,}, {7,sizeof(HANDLE),SYSARG_IS_HANDLE|W,}, }},
    {0,"NtGdiDdChangeSurfacePointer", OK, 2, },
    {0,"NtGdiDdCreateSurfaceObject", OK, 6, {{2,sizeof(DD_SURFACE_LOCAL),R,}, {3,sizeof(DD_SURFACE_MORE),R,}, {4,sizeof(DD_SURFACE_GLOBAL),R,}, }},
    {0,"NtGdiDdDeleteSurfaceObject", OK, 1, },
    {0,"NtGdiDdDeleteDirectDrawObject", OK, 1, },
    {0,"NtGdiDdDestroySurface", OK, 2, },
    {0,"NtGdiDdFlip", OK, 5, {{4,sizeof(DD_FLIPDATA),R|W,}, }},
    {0,"NtGdiDdGetAvailDriverMemory", OK, 2, {{1,sizeof(DD_GETAVAILDRIVERMEMORYDATA),R|W,}, }},
    {0,"NtGdiDdGetBltStatus", OK, 2, {{1,sizeof(DD_GETBLTSTATUSDATA),R|W,}, }},
    {0,"NtGdiDdGetDC", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 2, {{1,sizeof(PALETTEENTRY),R,}, }, &sysnum_GdiDdGetDC},
    {0,"NtGdiDdGetDriverInfo", OK, 2, {{1,sizeof(DD_GETDRIVERINFODATA),R|W,}, }},
    {0,"NtGdiDdGetFlipStatus", OK, 2, {{1,sizeof(DD_GETFLIPSTATUSDATA),R|W,}, }},
    {0,"NtGdiDdGetScanLine", OK, 2, {{1,sizeof(DD_GETSCANLINEDATA),R|W,}, }},
    {0,"NtGdiDdSetExclusiveMode", OK, 2, {{1,sizeof(DD_SETEXCLUSIVEMODEDATA),R|W,}, }},
    {0,"NtGdiDdFlipToGDISurface", OK, 2, {{1,sizeof(DD_FLIPTOGDISURFACEDATA),R|W,}, }},
    {0,"NtGdiDdLock", OK, 3, {{1,sizeof(DD_LOCKDATA),R|W,}, }},
    {0,"NtGdiDdQueryDirectDrawObject", OK, 11, {{1,sizeof(DD_HALINFO),W,}, {2,3,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(DWORD)}, {3,sizeof(D3DNTHAL_CALLBACKS),W,}, {4,sizeof(D3DNTHAL_GLOBALDRIVERDATA),W,}, {5,sizeof(DD_D3DBUFCALLBACKS),W,}, {6,sizeof(DDSURFACEDESC),W,}, {7,sizeof(DWORD),W,}, {8,sizeof(VIDEOMEMORY),W,}, {9,sizeof(DWORD),W,}, {10,sizeof(DWORD),W,}, }},
    {0,"NtGdiDdReenableDirectDrawObject", OK, 2, {{1,sizeof(BOOL),R|W,}, }},
    {0,"NtGdiDdReleaseDC", OK, 1, {{0,sizeof(HANDLE),SYSARG_IS_HANDLE|W,}, }},
    {0,"NtGdiDdResetVisrgn", OK, 2, },
    {0,"NtGdiDdSetColorKey", OK, 2, {{1,sizeof(DD_SETCOLORKEYDATA),R|W,}, }},
    {0,"NtGdiDdSetOverlayPosition", OK, 3, {{2,sizeof(DD_SETOVERLAYPOSITIONDATA),R|W,}, }},
    {0,"NtGdiDdUnattachSurface", OK, 2, },
    {0,"NtGdiDdUnlock", OK, 2, {{1,sizeof(DD_UNLOCKDATA),R|W,}, }},
    {0,"NtGdiDdUpdateOverlay", OK, 3, {{2,sizeof(DD_UPDATEOVERLAYDATA),R|W,}, }},
    {0,"NtGdiDdWaitForVerticalBlank", OK, 2, {{1,sizeof(DD_WAITFORVERTICALBLANKDATA),R|W,}, }},
    {0,"NtGdiDdGetDxHandle", OK, 3, },
    {0,"NtGdiDdSetGammaRamp", OK, 3, },
    {0,"NtGdiDdLockD3D", OK, 2, {{1,sizeof(DD_LOCKDATA),R|W,}, }},
    {0,"NtGdiDdUnlockD3D", OK, 2, {{1,sizeof(DD_UNLOCKDATA),R|W,}, }},
    {0,"NtGdiDdCreateD3DBuffer", OK|SYSINFO_CREATE_HANDLE, 8, {{1,sizeof(HANDLE),SYSARG_IS_HANDLE|R|W,}, {2,sizeof(DDSURFACEDESC),R|W,}, {3,sizeof(DD_SURFACE_GLOBAL),R|W,}, {4,sizeof(DD_SURFACE_LOCAL),R|W,}, {5,sizeof(DD_SURFACE_MORE),R|W,}, {6,sizeof(DD_CREATESURFACEDATA),R|W,}, {7,sizeof(HANDLE),SYSARG_IS_HANDLE|R|W,}, }},
    {0,"NtGdiDdCanCreateD3DBuffer", OK, 2, {{1,sizeof(DD_CANCREATESURFACEDATA),R|W,}, }},
    {0,"NtGdiDdDestroyD3DBuffer", OK, 1, },
    {0,"NtGdiD3dContextCreate", OK, 4, {{3,sizeof(D3DNTHAL_CONTEXTCREATEI),R|W,}, }},
    {0,"NtGdiD3dContextDestroy", OK, 1, {{0,sizeof(D3DNTHAL_CONTEXTDESTROYDATA),R,}, }},
    {0,"NtGdiD3dContextDestroyAll", OK, 1, {{0,sizeof(D3DNTHAL_CONTEXTDESTROYALLDATA),W,}, }},
    {0,"NtGdiD3dValidateTextureStageState", OK, 1, {{0,sizeof(D3DNTHAL_VALIDATETEXTURESTAGESTATEDATA),R|W,}, }},
    {0,"NtGdiD3dDrawPrimitives2", OK, 7, {{2,sizeof(D3DNTHAL_DRAWPRIMITIVES2DATA),R|W,}, {3,sizeof(FLATPTR),R|W,}, {4,sizeof(DWORD),R|W,}, {5,sizeof(FLATPTR),R|W,}, {6,sizeof(DWORD),R|W,}, }},
    {0,"NtGdiDdGetDriverState", OK, 1, {{0,sizeof(DD_GETDRIVERSTATEDATA),R|W,}, }},
    {0,"NtGdiDdCreateSurfaceEx", OK, 3, },
    {0,"NtGdiDvpCanCreateVideoPort", OK, 2, {{1,sizeof(DD_CANCREATEVPORTDATA),R|W,}, }},
    {0,"NtGdiDvpColorControl", OK, 2, {{1,sizeof(DD_VPORTCOLORDATA),R|W,}, }},
    {0,"NtGdiDvpCreateVideoPort", OK, 2, {{1,sizeof(DD_CREATEVPORTDATA),R|W,}, }},
    {0,"NtGdiDvpDestroyVideoPort", OK, 2, {{1,sizeof(DD_DESTROYVPORTDATA),R|W,}, }},
    {0,"NtGdiDvpFlipVideoPort", OK, 4, {{3,sizeof(DD_FLIPVPORTDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortBandwidth", OK, 2, {{1,sizeof(DD_GETVPORTBANDWIDTHDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortField", OK, 2, {{1,sizeof(DD_GETVPORTFIELDDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortFlipStatus", OK, 2, {{1,sizeof(DD_GETVPORTFLIPSTATUSDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortInputFormats", OK, 2, {{1,sizeof(DD_GETVPORTINPUTFORMATDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortLine", OK, 2, {{1,sizeof(DD_GETVPORTLINEDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortOutputFormats", OK, 2, {{1,sizeof(DD_GETVPORTOUTPUTFORMATDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoPortConnectInfo", OK, 2, {{1,sizeof(DD_GETVPORTCONNECTDATA),R|W,}, }},
    {0,"NtGdiDvpGetVideoSignalStatus", OK, 2, {{1,sizeof(DD_GETVPORTSIGNALDATA),R|W,}, }},
    {0,"NtGdiDvpUpdateVideoPort", OK, 4, {{1,sizeof(HANDLE),SYSARG_IS_HANDLE|R,}, {2,sizeof(HANDLE),SYSARG_IS_HANDLE|R,}, {3,sizeof(DD_UPDATEVPORTDATA),R|W,}, }},
    {0,"NtGdiDvpWaitForVideoPortSync", OK, 2, {{1,sizeof(DD_WAITFORVPORTSYNCDATA),R|W,}, }},
    {0,"NtGdiDvpAcquireNotification", OK|SYSINFO_CREATE_HANDLE, 3, {{1,sizeof(HANDLE),SYSARG_IS_HANDLE|R|W,}, {2,sizeof(DDVIDEOPORTNOTIFY),R,}, }},
    {0,"NtGdiDvpReleaseNotification", OK, 2, },
    {0,"NtGdiDdGetMoCompGuids", OK, 2, {{1,sizeof(DD_GETMOCOMPGUIDSDATA),R|W,}, }},
    {0,"NtGdiDdGetMoCompFormats", OK, 2, {{1,sizeof(DD_GETMOCOMPFORMATSDATA),R|W,}, }},
    {0,"NtGdiDdGetMoCompBuffInfo", OK, 2, {{1,sizeof(DD_GETMOCOMPCOMPBUFFDATA),R|W,}, }},
    {0,"NtGdiDdGetInternalMoCompInfo", OK, 2, {{1,sizeof(DD_GETINTERNALMOCOMPDATA),R|W,}, }},
    {0,"NtGdiDdCreateMoComp", OK, 2, {{1,sizeof(DD_CREATEMOCOMPDATA),R|W,}, }},
    {0,"NtGdiDdDestroyMoComp", OK, 2, {{1,sizeof(DD_DESTROYMOCOMPDATA),R|W,}, }},
    {0,"NtGdiDdBeginMoCompFrame", OK, 2, {{1,sizeof(DD_BEGINMOCOMPFRAMEDATA),R|W,}, }},
    {0,"NtGdiDdEndMoCompFrame", OK, 2, {{1,sizeof(DD_ENDMOCOMPFRAMEDATA),R|W,}, }},
    {0,"NtGdiDdRenderMoComp", OK, 2, {{1,sizeof(DD_RENDERMOCOMPDATA),R|W,}, }},
    {0,"NtGdiDdQueryMoCompStatus", OK, 2, {{1,sizeof(DD_QUERYMOCOMPSTATUSDATA),R|W,}, }},
    {0,"NtGdiDdAlphaBlt", OK, 3, {{2,sizeof(DD_BLTDATA),R|W,}, }},
    {0,"NtGdiAlphaBlend", OK, 12, },
    {0,"NtGdiGradientFill", OK, 6, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(TRIVERTEX)}, }},
    {0,"NtGdiSetIcmMode", OK, 3, },
    {0,"NtGdiCreateColorSpace", OK|SYSINFO_CREATE_HANDLE, 1, {{0,sizeof(LOGCOLORSPACEEXW),R,}, }},
    {0,"NtGdiDeleteColorSpace", OK, 1, {{0,sizeof(HANDLE),SYSARG_IS_HANDLE|R,}, }},
    {0,"NtGdiSetColorSpace", OK, 2, },
    {0,"NtGdiCreateColorTransform", OK|SYSINFO_CREATE_HANDLE, 8, {{1,sizeof(LOGCOLORSPACEW),R,}, }},
    {0,"NtGdiDeleteColorTransform", OK, 2, },
    {0,"NtGdiCheckBitmapBits", OK, 8, {{0,}/*too complex: special-cased*/, }, &sysnum_GdiCheckBitmapBits},
    {0,"NtGdiColorCorrectPalette", OK, 6, {{4,-3,R|W|SYSARG_SIZE_IN_ELEMENTS,sizeof(PALETTEENTRY)}, }},
    {0,"NtGdiGetColorSpaceforBitmap", OK, 1, },
    {0,"NtGdiGetDeviceGammaRamp", OK, 2, {{1,256*2*3,W,}, }},
    {0,"NtGdiSetDeviceGammaRamp", OK, 2, },
    {0,"NtGdiIcmBrushInfo", OK, 8, {{2,sizeof(BITMAPINFO) + ((/*MAX_COLORTABLE*/256 - 1) * sizeof(RGBQUAD)),R|W,}, {3,-4,R|SYSARG_LENGTH_INOUT,}, {4,sizeof(ULONG),R|W,}, {5,sizeof(DWORD),W,}, {6,sizeof(BOOL),W,}, }},
    {0,"NtGdiFlush", OK, 0, },
    {0,"NtGdiCreateMetafileDC", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 1, {{0,}}, &sysnum_GdiCreateMetafileDC},
    {0,"NtGdiMakeInfoDC", OK, 2, },
    {0,"NtGdiCreateClientObj", OK|SYSINFO_CREATE_HANDLE, 1, },
    {0,"NtGdiDeleteClientObj", OK, 1, },
    {0,"NtGdiGetBitmapBits", OK, 3, {{2,-1,W,}, }},
    {0,"NtGdiDeleteObjectApp", OK|SYSINFO_DELETE_HANDLE, 1, {{0,}}, &sysnum_GdiDeleteObjectApp},
    {0,"NtGdiGetPath", OK, 4, {{1,-3,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINT)}, {2,-3,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(BYTE)}, }},
    {0,"NtGdiCreateCompatibleDC", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 1, {{0,}}, &sysnum_GdiCreateCompatibleDC},
    {0,"NtGdiCreateDIBitmapInternal", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 11, {{4,-8,R,}, {5,-7,R,}, }},
    {0,"NtGdiCreateDIBSection", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 9, {{3,-5,R,}, {8,sizeof(PVOID),W,}, }},
    {0,"NtGdiCreateSolidBrush", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 2, },
    {0,"NtGdiCreateDIBBrush", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 6, },
    {0,"NtGdiCreatePatternBrushInternal", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 3, },
    {0,"NtGdiCreateHatchBrushInternal", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 3, },
    {0,"NtGdiExtCreatePen", OK|SYSINFO_RET_ZERO_FAIL, 11, {{7,-6,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(ULONG)}, }},
    {0,"NtGdiCreateEllipticRgn", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 4, },
    {0,"NtGdiCreateRoundRectRgn", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 6, },
    {0,"NtGdiCreateServerMetaFile", OK|SYSINFO_CREATE_HANDLE, 6, {{2,-1,R,}, }},
    {0,"NtGdiExtCreateRegion", OK|SYSINFO_RET_ZERO_FAIL, 3, {{0,sizeof(XFORM),R,}, {2,-1,R,}, }},
    {0,"NtGdiMakeFontDir", OK, 5, {{1,-2,W,}, {3,-4,R,}, }},
    {0,"NtGdiPolyDraw", OK, 4, {{1,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINT)}, {2,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(BYTE)}, }},
    {0,"NtGdiPolyTextOutW", OK, 4, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(POLYTEXTW)}, }},
    {0,"NtGdiGetServerMetaFileBits", OK, 7, {{2,-1,W,}, {3,sizeof(DWORD),W,}, {4,sizeof(DWORD),W,}, {5,sizeof(DWORD),W,}, {6,sizeof(DWORD),W,}, }},
    {0,"NtGdiEqualRgn", OK, 2, },
    {0,"NtGdiGetBitmapDimension", OK, 2, {{1,sizeof(SIZE),W,}, }},
    {0,"NtGdiGetNearestPaletteIndex", OK, 2, },
    {0,"NtGdiPtVisible", OK, 3, },
    {0,"NtGdiRectVisible", OK, 2, {{1,sizeof(RECT),R,}, }},
    {0,"NtGdiRemoveFontResourceW", OK, 6, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(WCHAR)}, {5,sizeof(DESIGNVECTOR),R,}, }},
    {0,"NtGdiResizePalette", OK, 2, },
    {0,"NtGdiSetBitmapDimension", OK, 4, {{3,sizeof(SIZE),W,}, }},
    {0,"NtGdiOffsetClipRgn", OK, 3, },
    {0,"NtGdiSetMetaRgn", OK, 1, },
    {0,"NtGdiSetTextJustification", OK, 3, },
    {0,"NtGdiGetAppClipBox", OK, 2, {{1,sizeof(RECT),W,}, }},
    {0,"NtGdiGetTextExtentExW", OK, 8, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {4,sizeof(ULONG),W,}, {5,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(ULONG)}, {5,-4,WI|SYSARG_SIZE_IN_ELEMENTS,sizeof(ULONG)}, {6,sizeof(SIZE),W,}, }},
    {0,"NtGdiGetCharABCWidthsW", OK, 6, {{3,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(WCHAR)}, {5,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(ABC)}, }},
    {0,"NtGdiGetCharacterPlacementW", OK, 6, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {4,sizeof(GCP_RESULTSW),R|W,}, }},
    {0,"NtGdiAngleArc", OK, 6, },
    {0,"NtGdiBeginPath", OK, 1, },
    {0,"NtGdiSelectClipPath", OK, 2, },
    {0,"NtGdiCloseFigure", OK, 1, },
    {0,"NtGdiEndPath", OK, 1, },
    {0,"NtGdiAbortPath", OK, 1, },
    {0,"NtGdiFillPath", OK, 1, },
    {0,"NtGdiStrokeAndFillPath", OK, 1, },
    {0,"NtGdiStrokePath", OK, 1, },
    {0,"NtGdiWidenPath", OK, 1, },
    {0,"NtGdiFlattenPath", OK, 1, },
    {0,"NtGdiPathToRegion", OK, 1, },
    {0,"NtGdiSetMiterLimit", OK, 3, {{2,sizeof(DWORD),R|W,}, }},
    {0,"NtGdiSetFontXform", OK, 3, },
    {0,"NtGdiGetMiterLimit", OK, 2, {{1,sizeof(DWORD),W,}, }},
    {0,"NtGdiEllipse", OK, 5, },
    {0,"NtGdiRectangle", OK, 5, },
    {0,"NtGdiRoundRect", OK, 7, },
    {0,"NtGdiPlgBlt", OK, 11, {{1,3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINT)}, }},
    {0,"NtGdiMaskBlt", OK, 13, },
    {0,"NtGdiExtFloodFill", OK, 5, },
    {0,"NtGdiFillRgn", OK, 3, },
    {0,"NtGdiFrameRgn", OK, 5, },
    {0,"NtGdiSetPixel", OK, 4, },
    {0,"NtGdiGetPixel", OK, 3, },
    {0,"NtGdiStartPage", OK, 1, },
    {0,"NtGdiEndPage", OK, 1, },
    {0,"NtGdiStartDoc", OK, 4, {{1,sizeof(DOCINFOW),R,}, {2,sizeof(BOOL),W,}, }},
    {0,"NtGdiEndDoc", OK, 1, },
    {0,"NtGdiAbortDoc", OK, 1, },
    {0,"NtGdiUpdateColors", OK, 1, },
    {0,"NtGdiGetCharWidthW", OK, 6, {{3,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(WCHAR)}, {5,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(ULONG)}, }},
    {0,"NtGdiGetCharWidthInfo", OK, 2, {{1,sizeof(CHWIDTHINFO),W,}, }},
    {0,"NtGdiDrawEscape", OK, 4, {{3,-2,R,}, }},
    {0,"NtGdiExtEscape", OK, 8, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(WCHAR)}, {5,-4,R,}, {7,-6,W,}, }},
    {0,"NtGdiGetFontData", OK, 5, {{3,-4,W,}, {3,RET,W,}, }},
    {0,"NtGdiGetFontFileData", OK, 5, {{2,sizeof(ULONGLONG),R,}, {3,-4,W,}, }},
    {0,"NtGdiGetFontFileInfo", OK, 5, {{2,-3,W,}, {4,sizeof(SIZE_T),W,}, }},
    {0,"NtGdiGetGlyphOutline", OK, 8, {{3,sizeof(GLYPHMETRICS),W,}, {5,-4,W,}, {6,sizeof(MAT2),R,}, }},
    {0,"NtGdiGetETM", OK, 2, {{1,sizeof(EXTTEXTMETRIC),W,}, }},
    {0,"NtGdiGetRasterizerCaps", OK, 2, {{0,-1,W,}, }},
    {0,"NtGdiGetKerningPairs", OK, 3, {{2,-1,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(KERNINGPAIR)}, {2,RET,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(KERNINGPAIR)}, }},
    {0,"NtGdiMonoBitmap", OK, 1, },
    {0,"NtGdiGetObjectBitmapHandle", OK|SYSINFO_RET_ZERO_FAIL, 2, {{1,sizeof(UINT),W,}, }},
    {0,"NtGdiEnumObjects", OK, 4, {{3,-2,W,}, }},
    {0,"NtGdiResetDC", OK, 5, {{1,sizeof(DEVMODEW)/*really var-len*/,R|CT,SYSARG_TYPE_DEVMODEW}, {2,sizeof(BOOL),W,}, {3,sizeof(DRIVER_INFO_2W),R,}, {4,sizeof(PUMDHPDEV *),W,}, }},
    {0,"NtGdiSetBoundsRect", OK, 3, {{1,sizeof(RECT),R,}, }},
    {0,"NtGdiGetColorAdjustment", OK, 2, {{1,sizeof(COLORADJUSTMENT),W,}, }},
    {0,"NtGdiSetColorAdjustment", OK, 2, {{1,sizeof(COLORADJUSTMENT),R,}, }},
    {0,"NtGdiCancelDC", OK, 1, },
    {0,"NtGdiOpenDCW", OK|SYSINFO_CREATE_HANDLE, 7/*8 on Vista+*/, {{0,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING,}, {1,sizeof(DEVMODEW)/*really var-len*/,R|CT,SYSARG_TYPE_DEVMODEW}, {2,sizeof(UNICODE_STRING),R|CT,SYSARG_TYPE_UNICODE_STRING,}, /*arg added in middle in Vista so special-cased*/}, &sysnum_GdiOpenDCW},
    {0,"NtGdiGetDCDword", OK, 3, {{2,sizeof(DWORD),W,}, }},
    {0,"NtGdiGetDCPoint", OK, 3, {{2,sizeof(POINTL),W,}, }},
    {0,"NtGdiScaleViewportExtEx", OK, 6, {{5,sizeof(SIZE),W,}, }},
    {0,"NtGdiScaleWindowExtEx", OK, 6, {{5,sizeof(SIZE),W,}, }},
    {0,"NtGdiSetVirtualResolution", OK, 5, },
    {0,"NtGdiSetSizeDevice", OK, 3, },
    {0,"NtGdiGetTransform", OK, 3, {{2,sizeof(XFORM),W,}, }},
    {0,"NtGdiModifyWorldTransform", OK, 3, {{1,sizeof(XFORM),R,}, }},
    {0,"NtGdiCombineTransform", OK, 3, {{0,sizeof(XFORM),W,}, {1,sizeof(XFORM),R,}, {2,sizeof(XFORM),R,}, }},
    {0,"NtGdiTransformPoints", OK, 5, {{1,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINT)}, {2,-3,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINT)}, }},
    {0,"NtGdiConvertMetafileRect", OK, 2, {{1,sizeof(RECTL),R|W,}, }},
    {0,"NtGdiGetTextCharsetInfo", OK, 3, {{1,sizeof(FONTSIGNATURE),W,}, }},
    {0,"NtGdiDoBanding", OK, 4, {{2,sizeof(POINTL),W,}, {3,sizeof(SIZE),W,}, }},
    {0,"NtGdiGetPerBandInfo", OK, 2, {{1,sizeof(PERBANDINFO),R|W,}, }},
    {0,"NtGdiGetStats", OK, 5, {{3,-4,W,}, }},
    {0,"NtGdiSetMagicColors", OK, 3, },
    {0,"NtGdiSelectBrush", OK|SYSINFO_RET_ZERO_FAIL, 2, },
    {0,"NtGdiSelectPen", OK|SYSINFO_RET_ZERO_FAIL, 2, },
    {0,"NtGdiSelectBitmap", OK|SYSINFO_RET_ZERO_FAIL, 2, },
    {0,"NtGdiSelectFont", OK|SYSINFO_RET_ZERO_FAIL, 2, },
    {0,"NtGdiExtSelectClipRgn", OK, 3, },
    {0,"NtGdiCreatePen", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 4, {{0,},}},
    {0,"NtGdiBitBlt", OK, 11, },
    {0,"NtGdiTileBitBlt", OK, 7, {{1,sizeof(RECTL),R,}, {3,sizeof(RECTL),R,}, {4,sizeof(POINTL),R,}, }},
    {0,"NtGdiTransparentBlt", OK, 11, },
    {0,"NtGdiGetTextExtent", OK, 5, {{1,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {3,sizeof(SIZE),W,}, }},
    {0,"NtGdiGetTextMetricsW", OK, 3, {{1,-2,W,}, }},
    {0,"NtGdiGetTextFaceW", OK, 4, {{2,-1,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {2,RET,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, }},
    {0,"NtGdiGetRandomRgn", OK, 3, },
    {0,"NtGdiExtTextOutW", OK, 9, {{4,sizeof(RECT),R,}, {5,-6,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {7,-6,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(INT)/*can be larger: special-cased*/}, }, &sysnum_GdiExtTextOutW},
    {0,"NtGdiIntersectClipRect", OK, 5, },
    {0,"NtGdiCreateRectRgn", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 4, },
    {0,"NtGdiPatBlt", OK, 6, },
    {0,"NtGdiPolyPatBlt", OK, 5, {{2,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(POLYPATBLT)}, }},
    {0,"NtGdiUnrealizeObject", OK, 1, },
    {0,"NtGdiGetStockObject", OK, 1, },
    {0,"NtGdiCreateCompatibleBitmap", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 3, {{0,}, }},
    {0,"NtGdiCreateBitmapFromDxSurface", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 5, },
    {0,"NtGdiBeginGdiRendering", OK, 2, },
    {0,"NtGdiEndGdiRendering", OK, 3, {{2,sizeof(BOOL),W,}, }},
    {0,"NtGdiLineTo", OK, 3, },
    {0,"NtGdiMoveTo", OK, 4, {{3,sizeof(POINT),W,}, }},
    {0,"NtGdiExtGetObjectW", OK, 3, {{2,-1,W}, {2,RET,W,}, }},
    {0,"NtGdiGetDeviceCaps", OK, 2, },
    {0,"NtGdiGetDeviceCapsAll", OK, 2, {{1,sizeof(DEVCAPS),W,}, }},
    {0,"NtGdiStretchBlt", OK, 12, },
    {0,"NtGdiSetBrushOrg", OK, 4, {{3,sizeof(POINT),W,}, }},
    {0,"NtGdiCreateBitmap", OK|SYSINFO_CREATE_HANDLE|SYSINFO_RET_ZERO_FAIL, 5, {{4,sizeof(BYTE),R,}, }},
    {0,"NtGdiCreateHalftonePalette", OK|SYSINFO_CREATE_HANDLE, 1, },
    {0,"NtGdiRestoreDC", OK, 2, },
    {0,"NtGdiExcludeClipRect", OK, 5, },
    {0,"NtGdiSaveDC", OK, 1, },
    {0,"NtGdiCombineRgn", OK, 4, },
    {0,"NtGdiSetRectRgn", OK, 5, },
    {0,"NtGdiSetBitmapBits", OK, 3, {{2,-1,R,}, }},
    {0,"NtGdiGetDIBitsInternal", OK, 9, {{4,-7,W,}, {5,sizeof(BITMAPINFO),R|W,}, }},
    {0,"NtGdiOffsetRgn", OK, 3, },
    {0,"NtGdiGetRgnBox", OK, 2, {{1,sizeof(RECT),W,}, }},
    {0,"NtGdiRectInRegion", OK, 2, {{1,sizeof(RECT),R|W,}, }},
    {0,"NtGdiGetBoundsRect", OK, 3, {{1,sizeof(RECT),W,}, }},
    {0,"NtGdiPtInRegion", OK, 3, },
    {0,"NtGdiGetNearestColor", OK, 2, },
    {0,"NtGdiGetSystemPaletteUse", OK, 1, },
    {0,"NtGdiSetSystemPaletteUse", OK, 2, },
    {0,"NtGdiGetRegionData", OK, 3, {{2,-1,W,}, {2,RET,W,}, }},
    {0,"NtGdiInvertRgn", OK, 2, },
    {0,"NtGdiHfontCreate", OK, 5, {{0,}, },/*special-cased*/ &sysnum_GdiHfontCreate},
#if 0 /* for _WIN32_WINNT < 0x0500 == NT which we ignore for now */
    {0,"NtGdiHfontCreate", OK, 5, {{0,sizeof(EXTLOGFONTW),R,}, }},
#endif
    {0,"NtGdiSetFontEnumeration", OK, 1, },
    {0,"NtGdiEnumFonts", OK, 8, {{4,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {6,sizeof(ULONG),R|W,}, {7,-6,WI,}, }},
    {0,"NtGdiQueryFonts", OK, 3, {{0,-1,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(UNIVERSAL_FONT_ID)}, {2,sizeof(LARGE_INTEGER),W,}, }},
    {0,"NtGdiGetCharSet", OK, 1, },
    {0,"NtGdiEnableEudc", OK, 1, },
    {0,"NtGdiEudcLoadUnloadLink", OK, 7, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, {2,-3,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, }},
    {0,"NtGdiGetStringBitmapW", OK, 5, {{1,sizeof(wchar_t),R,}, {4,-3,W,}, }},
    {0,"NtGdiGetEudcTimeStampEx", OK, 3, {{0,-1,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(wchar_t)}, }},
    {0,"NtGdiQueryFontAssocInfo", OK, 1, },
    {0,"NtGdiGetFontUnicodeRanges", OK, 2, {{1,RET,W,/*FIXME i#485: pre size from prior syscall ret*/}, }},
    /* FIXME i#485: the REALIZATION_INFO struct is much larger on win7 */
    {0,"NtGdiGetRealizationInfo", UNKNOWN, 2, {{1,sizeof(REALIZATION_INFO),W,}, }},
    {0,"NtGdiAddRemoteMMInstanceToDC", OK, 3, {{1,-2,R,}, }},
    {0,"NtGdiUnloadPrinterDriver", OK, 2, {{0,-1,R,}, }},
    {0,"NtGdiEngAssociateSurface", OK, 3, },
    {0,"NtGdiEngEraseSurface", OK, 3, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(RECTL),R,}, }},
    {0,"NtGdiEngCreateBitmap", OK, 5, },
    {0,"NtGdiEngDeleteSurface", OK, 1, },
    {0,"NtGdiEngLockSurface", OK, 1, },
    {0,"NtGdiEngUnlockSurface", OK, 1, {{0,sizeof(SURFOBJ),R,}, }},
    {0,"NtGdiEngMarkBandingSurface", OK, 1, },
    {0,"NtGdiEngCreateDeviceSurface", OK, 3, },
    {0,"NtGdiEngCreateDeviceBitmap", OK, 3, },
    {0,"NtGdiEngCopyBits", OK, 6, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(CLIPOBJ),R,}, {3,sizeof(XLATEOBJ),R,}, {4,sizeof(RECTL),R,}, {5,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngStretchBlt", OK, 11, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(SURFOBJ),R,}, {3,sizeof(CLIPOBJ),R,}, {4,sizeof(XLATEOBJ),R,}, {5,sizeof(COLORADJUSTMENT),R,}, {6,sizeof(POINTL),R,}, {7,sizeof(RECTL),R,}, {8,sizeof(RECTL),R,}, {9,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngBitBlt", OK, 11, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(SURFOBJ),R,}, {3,sizeof(CLIPOBJ),R,}, {4,sizeof(XLATEOBJ),R,}, {5,sizeof(RECTL),R,}, {6,sizeof(POINTL),R,}, {7,sizeof(POINTL),R,}, {8,sizeof(BRUSHOBJ),R,}, {9,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngPlgBlt", OK, 11, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(SURFOBJ),R,}, {3,sizeof(CLIPOBJ),R,}, {4,sizeof(XLATEOBJ),R,}, {5,sizeof(COLORADJUSTMENT),R,}, {6,sizeof(POINTL),R,}, {7,sizeof(POINTFIX),R,}, {8,sizeof(RECTL),R,}, {9,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngCreatePalette", OK, 6, {{2,sizeof(ULONG),R,}, }},
    {0,"NtGdiEngDeletePalette", OK, 1, },
    {0,"NtGdiEngStrokePath", OK, 8, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(PATHOBJ),R,}, {2,sizeof(CLIPOBJ),R,}, {3,sizeof(XFORMOBJ),R,}, {4,sizeof(BRUSHOBJ),R,}, {5,sizeof(POINTL),R,}, {6,sizeof(LINEATTRS),R,}, }},
    {0,"NtGdiEngFillPath", OK, 7, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(PATHOBJ),R,}, {2,sizeof(CLIPOBJ),R,}, {3,sizeof(BRUSHOBJ),R,}, {4,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngStrokeAndFillPath", OK, 10, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(PATHOBJ),R,}, {2,sizeof(CLIPOBJ),R,}, {3,sizeof(XFORMOBJ),R,}, {4,sizeof(BRUSHOBJ),R,}, {5,sizeof(LINEATTRS),R,}, {6,sizeof(BRUSHOBJ),R,}, {7,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngPaint", OK, 5, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(CLIPOBJ),R,}, {2,sizeof(BRUSHOBJ),R,}, {3,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngLineTo", OK, 9, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(CLIPOBJ),R,}, {2,sizeof(BRUSHOBJ),R,}, {7,sizeof(RECTL),R,}, }},
    {0,"NtGdiEngAlphaBlend", OK, 7, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(CLIPOBJ),R,}, {3,sizeof(XLATEOBJ),R,}, {4,sizeof(RECTL),R,}, {5,sizeof(RECTL),R,}, {6,sizeof(BLENDOBJ),R,}, }},
    {0,"NtGdiEngGradientFill", OK, 10, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(CLIPOBJ),R,}, {2,sizeof(XLATEOBJ),R,}, {3,-4,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(TRIVERTEX)}, {7,sizeof(RECTL),R,}, {8,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngTransparentBlt", OK, 8, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(CLIPOBJ),R,}, {3,sizeof(XLATEOBJ),R,}, {4,sizeof(RECTL),R,}, {5,sizeof(RECTL),R,}, }},
    {0,"NtGdiEngTextOut", OK, 10, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(STROBJ),R,}, {2,sizeof(FONTOBJ),R,}, {3,sizeof(CLIPOBJ),R,}, {4,sizeof(RECTL),R,}, {5,sizeof(RECTL),R,}, {6,sizeof(BRUSHOBJ),R,}, {7,sizeof(BRUSHOBJ),R,}, {8,sizeof(POINTL),R,}, }},
    {0,"NtGdiEngStretchBltROP", OK, 13, {{0,sizeof(SURFOBJ),R,}, {1,sizeof(SURFOBJ),R,}, {2,sizeof(SURFOBJ),R,}, {3,sizeof(CLIPOBJ),R,}, {4,sizeof(XLATEOBJ),R,}, {5,sizeof(COLORADJUSTMENT),R,}, {6,sizeof(POINTL),R,}, {7,sizeof(RECTL),R,}, {8,sizeof(RECTL),R,}, {9,sizeof(POINTL),R,}, {11,sizeof(BRUSHOBJ),R,}, }},
    {0,"NtGdiXLATEOBJ_cGetPalette", OK, 4, {{0,sizeof(XLATEOBJ),R,}, {3,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(ULONG)}, }},
    {0,"NtGdiCLIPOBJ_cEnumStart", OK, 5, {{0,sizeof(CLIPOBJ),R,}, }},
    {0,"NtGdiCLIPOBJ_bEnum", OK, 3, {{0,sizeof(CLIPOBJ),R,}, {2,-1,W,}, }},
    {0,"NtGdiCLIPOBJ_ppoGetPath", OK, 1, {{0,sizeof(CLIPOBJ),R,}, }},
    {0,"NtGdiEngCreateClip", OK, 0, },
    {0,"NtGdiEngDeleteClip", OK, 1, {{0,sizeof(CLIPOBJ),R,}, }},
    {0,"NtGdiBRUSHOBJ_pvAllocRbrush", OK, 2, {{0,sizeof(BRUSHOBJ),R,}, }},
    {0,"NtGdiBRUSHOBJ_pvGetRbrush", OK, 1, {{0,sizeof(BRUSHOBJ),R,}, }},
    {0,"NtGdiBRUSHOBJ_ulGetBrushColor", OK, 1, {{0,sizeof(BRUSHOBJ),R,}, }},
    {0,"NtGdiBRUSHOBJ_hGetColorTransform", OK, 1, {{0,sizeof(BRUSHOBJ),R,}, }},
    {0,"NtGdiXFORMOBJ_bApplyXform", OK, 5, {{0,sizeof(XFORMOBJ),R,}, {3,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINTL)}, {4,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINTL)}, }},
    {0,"NtGdiXFORMOBJ_iGetXform", OK, 2, {{0,sizeof(XFORMOBJ),R,}, {1,sizeof(XFORML),W,}, }},
    {0,"NtGdiFONTOBJ_vGetInfo", OK, 3, {{0,sizeof(FONTOBJ),R,}, {2,-1,W,}, }},
    {0,"NtGdiFONTOBJ_cGetGlyphs", OK, 5, {{0,sizeof(FONTOBJ),R,}, {3,sizeof(HGLYPH),R,}, {4,sizeof(GLYPHDATA **),W,}, }},
    {0,"NtGdiFONTOBJ_pxoGetXform", OK, 1, {{0,sizeof(FONTOBJ),R,}, }},
    {0,"NtGdiFONTOBJ_pifi", OK, 1, {{0,sizeof(FONTOBJ),R,}, }},
    {0,"NtGdiFONTOBJ_pfdg", OK, 1, {{0,sizeof(FONTOBJ),R,}, }},
    {0,"NtGdiFONTOBJ_cGetAllGlyphHandles", OK, 2, {{0,sizeof(FONTOBJ),R,}, {1,RET,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(HGLYPH)/*FIXME i#485: pre size from prior syscall ret*/}, }},
    {0,"NtGdiFONTOBJ_pvTrueTypeFontFile", OK, 2, {{0,sizeof(FONTOBJ),R,}, {1,sizeof(ULONG),W,}, }},
    {0,"NtGdiFONTOBJ_pQueryGlyphAttrs", OK, 2, {{0,sizeof(FONTOBJ),R,}, }},
    {0,"NtGdiSTROBJ_bEnum", OK, 3, {{0,sizeof(STROBJ),R,}, {1,sizeof(ULONG),R|W,/*XXX: I'm assuming R: else how know? prior syscall (i#485)?*/}, {2,-1,WI|SYSARG_SIZE_IN_ELEMENTS,sizeof(PGLYPHPOS)}, }},
    {0,"NtGdiSTROBJ_bEnumPositionsOnly", OK, 3, {{0,sizeof(STROBJ),R,}, {1,sizeof(ULONG),R|W,/*XXX: I'm assuming R: else how know? prior syscall (i#485)?*/}, {2,-1,WI|SYSARG_SIZE_IN_ELEMENTS,sizeof(PGLYPHPOS)}, }},
    {0,"NtGdiSTROBJ_vEnumStart", OK, 1, {{0,sizeof(STROBJ),R,}, }},
    {0,"NtGdiSTROBJ_dwGetCodePage", OK, 1, {{0,sizeof(STROBJ),R,}, }},
    {0,"NtGdiSTROBJ_bGetAdvanceWidths", OK, 4, {{0,sizeof(STROBJ),R,}, {3,-2,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(POINTQF)}, }},
    {0,"NtGdiEngComputeGlyphSet", OK, 3, },
    {0,"NtGdiXLATEOBJ_iXlate", OK, 2, {{0,sizeof(XLATEOBJ),R,}, }},
    {0,"NtGdiXLATEOBJ_hGetColorTransform", OK, 1, {{0,sizeof(XLATEOBJ),R,}, }},
    {0,"NtGdiPATHOBJ_vGetBounds", OK, 2, {{0,sizeof(PATHOBJ),R,}, {1,sizeof(RECTFX),W,}, }},
    {0,"NtGdiPATHOBJ_bEnum", OK, 2, {{0,sizeof(PATHOBJ),R,}, {1,sizeof(PATHDATA),W,}, }},
    {0,"NtGdiPATHOBJ_vEnumStart", OK, 1, {{0,sizeof(PATHOBJ),R,}, }},
    {0,"NtGdiEngDeletePath", OK, 1, {{0,sizeof(PATHOBJ),R,}, }},
    {0,"NtGdiPATHOBJ_vEnumStartClipLines", OK, 4, {{0,sizeof(PATHOBJ),R,}, {1,sizeof(CLIPOBJ),R,}, {2,sizeof(SURFOBJ),R,}, {3,sizeof(LINEATTRS),R,}, }},
    {0,"NtGdiPATHOBJ_bEnumClipLines", OK, 3, {{0,sizeof(PATHOBJ),R,}, {2,-1,W,}, }},
    {0,"NtGdiEngCheckAbort", OK, 1, {{0,sizeof(SURFOBJ),R,}, }},
    {0,"NtGdiGetDhpdev", OK, 1, },
    {0,"NtGdiHT_Get8BPPFormatPalette", OK, 4, {{0,RET,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(PALETTEENTRY)/*FIXME i#485: pre size from prior syscall ret*/}, }},
    {0,"NtGdiHT_Get8BPPMaskPalette", OK, 6, {{0,RET,W|SYSARG_SIZE_IN_ELEMENTS,sizeof(PALETTEENTRY)/*FIXME i#485: pre size from prior syscall ret*/}, }},
    {0,"NtGdiUpdateTransform", OK, 1, },
    {0,"NtGdiSetLayout", OK, 3, },
    {0,"NtGdiMirrorWindowOrg", OK, 1, },
    {0,"NtGdiGetDeviceWidth", OK, 1, },
    {0,"NtGdiSetPUMPDOBJ", OK|SYSINFO_CREATE_HANDLE, 4, {{2,sizeof(HUMPD),SYSARG_IS_HANDLE|R|W,}, {3,sizeof(BOOL),W,}, }},
    {0,"NtGdiBRUSHOBJ_DeleteRbrush", OK, 2, {{0,sizeof(BRUSHOBJ),R,}, {1,sizeof(BRUSHOBJ),R,}, }},
    {0,"NtGdiUMPDEngFreeUserMem", OK, 1, {{0,sizeof(KERNEL_PVOID),R,}, }},
    {0,"NtGdiSetBitmapAttributes", OK, 2, },
    {0,"NtGdiClearBitmapAttributes", OK, 2, },
    {0,"NtGdiSetBrushAttributes", OK, 2, },
    {0,"NtGdiClearBrushAttributes", OK, 2, },
    {0,"NtGdiDrawStream", OK, 3, },
    {0,"NtGdiMakeObjectXferable", OK, 2, },
    {0,"NtGdiMakeObjectUnXferable", OK, 1, },
    {0,"NtGdiSfmGetNotificationTokens", OK, 3, {{1,sizeof(UINT),W,}, {2,-0,W,}, }},
    {0,"NtGdiSfmRegisterLogicalSurfaceForSignaling", OK, 2, },
    {0,"NtGdiDwmGetHighColorMode", OK, 1, {{0,sizeof(DXGI_FORMAT),W,}, }},
    {0,"NtGdiDwmSetHighColorMode", OK, 1, },
    {0,"NtGdiDwmCaptureScreen", OK, 2, {{0,sizeof(RECT),R,}, }},
    {0,"NtGdiDdCreateFullscreenSprite", OK|SYSINFO_CREATE_HANDLE, 4, {{2,sizeof(HANDLE),SYSARG_IS_HANDLE|W,}, {3,sizeof(HDC),W,}, }},
    {0,"NtGdiDdNotifyFullscreenSpriteUpdate", OK, 2, },
    {0,"NtGdiDdDestroyFullscreenSprite", OK, 2, },
    {0,"NtGdiDdQueryVisRgnUniqueness", OK, 0, },

};
#define NUM_GDI32_SYSCALLS \
    (sizeof(syscall_gdi32_info)/sizeof(syscall_gdi32_info[0]))

size_t
num_gdi32_syscalls(void)
{
    return NUM_GDI32_SYSCALLS;
}

#undef OK
#undef UNKNOWN
#undef W
#undef R
#undef CT
#undef WI
#undef IB
#undef RET

/***************************************************************************
 * CUSTOM SYSCALL DATA STRUCTURE HANDLING
 */

/* XXX i#488: if too many params can take atoms or strings, should perhaps
 * query to verify really an atom to avoid false negatives with
 * bad string pointers
 */
static bool
is_atom(void *ptr)
{
    /* top 2 bytes are guaranteed to be 0 */
    return ((ptr_uint_t)ptr) < 0x10000;
}

/* XXX i#488: see is_atom comment */
static bool
is_int_resource(void *ptr)
{
    /* top 2 bytes are guaranteed to be 0 */
    return IS_INTRESOURCE(ptr);
}

extern bool
handle_unicode_string_access(bool pre, int sysnum, dr_mcontext_t *mc,
                             uint arg_num, const syscall_arg_t *arg_info,
                             app_pc start, uint size, bool ignore_len);

extern bool
handle_cwstring(bool pre, int sysnum, dr_mcontext_t *mc, const char *id,
                byte *start, size_t size, uint arg_flags, wchar_t *safe,
                bool check_addr);


bool
handle_large_string_access(bool pre, int sysnum, dr_mcontext_t *mc,
                             uint arg_num,
                             const syscall_arg_t *arg_info,
                             app_pc start, uint size)
{
    uint check_type = SYSARG_CHECK_TYPE(arg_info->flags, pre);
    LARGE_STRING ls;
    LARGE_STRING *arg = (LARGE_STRING *) start;
    ASSERT(size == sizeof(LARGE_STRING), "invalid size");
    /* I've seen an atom (or int resource?) here
     * XXX i#488: avoid false neg: not too many of these now though
     * so we allow on all syscalls
     */
    if (is_atom(start))
        return true; /* handled */
    /* we assume OUT fields just have their Buffer as OUT */
    if (pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, (byte *)&arg->Length,
                     sizeof(arg->Length), mc, "LARGE_STRING.Length");
        /* i#489: LARGE_STRING.MaximumLength and LARGE_STRING.bAnsi end
         * up initialized by a series of bit manips that fool us
         * so we don't check here
         */
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, (byte *)&arg->Buffer,
                     sizeof(arg->Buffer), mc, "LARGE_STRING.Buffer");
    }
    if (safe_read((void*)start, sizeof(ls), &ls)) {
        if (pre) {
            LOG(SYSCALL_VERBOSE,
                "LARGE_STRING Buffer="PFX" Length=%d MaximumLength=%d\n",
                (byte *)ls.Buffer, ls.Length, ls.MaximumLength);
            /* See i#489 notes above: check for undef if looks "suspicious": weak,
             * but simpler and more efficient than pattern match on every bb.
             */
            if (ls.MaximumLength > ls.Length &&
                ls.MaximumLength > 1024 /* suspicious */) {
                check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start + sizeof(arg->Length),
                             sizeof(ULONG/*+bAnsi*/), mc, "LARGE_STRING.MaximumLength");
            } else {
                shadow_set_range(start + sizeof(arg->Length),
                                 (byte *)&arg->Buffer, SHADOW_DEFINED);
            }
            check_sysmem(MEMREF_CHECK_ADDRESSABLE, sysnum,
                         (byte *)ls.Buffer, ls.MaximumLength, mc,
                         "LARGE_STRING capacity");
            if (TEST(SYSARG_READ, arg_info->flags)) {
                check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum,
                             (byte *)ls.Buffer, ls.Length, mc, "LARGE_STRING content");
            }
        } else if (TEST(SYSARG_WRITE, arg_info->flags)) {
            check_sysmem(MEMREF_WRITE, sysnum, (byte *)ls.Buffer, ls.Length, mc,
                          "LARGE_STRING content");
        }
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true; /* handled */
}

bool
handle_devmodew_access(bool pre, int sysnum, dr_mcontext_t *mc,
                       uint arg_num,
                       const syscall_arg_t *arg_info,
                       app_pc start, uint size)
{
    /* DEVMODEW is var-len by windows ver plus optional private driver data appended */
    uint check_type = SYSARG_CHECK_TYPE(arg_info->flags, pre);
    /* can't use a DEVMODEW as ours may be longer than app's if on older windows */
    char buf[offsetof(DEVMODEW,dmFields)]; /* need dmSize and dmDriverExtra */
    DEVMODEW *safe;
    DEVMODEW *param = (DEVMODEW *) start;
    if (pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     BUFFER_SIZE_BYTES(buf), mc, "DEVMODEW through dmDriverExtra");
    }
    if (safe_read(start, BUFFER_SIZE_BYTES(buf), buf)) {
        safe = (DEVMODEW *) buf;
        ASSERT(safe->dmSize > offsetof(DEVMODEW, dmFormName), "invalid size");
        /* there's some padding in the middle */
        check_sysmem(check_type, sysnum, (byte *) &param->dmFields,
                     ((byte *) &param->dmCollate) + sizeof(safe->dmCollate) -
                     (byte *) &param->dmFields,
                     mc, "DEVMODEW dmFields through dmCollate");
        check_sysmem(check_type, sysnum, (byte *) &param->dmFormName,
                     (start + safe->dmSize) - (byte *) (&param->dmFormName),
                     mc, "DEVMODEW dmFormName onward");
        check_sysmem(check_type, sysnum, start + safe->dmSize, safe->dmDriverExtra,
                     mc, "DEVMODEW driver extra info");
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true; /* handled */
}

bool
handle_wndclassexw_access(bool pre, int sysnum, dr_mcontext_t *mc,
                          uint arg_num,
                          const syscall_arg_t *arg_info,
                          app_pc start, uint size)
{
    uint check_type = SYSARG_CHECK_TYPE(arg_info->flags, pre);
    WNDCLASSEXW safe;
    /* i#499: it seems that cbSize is not set for NtUserGetClassInfo when using
     * user32!GetClassInfo so we use sizeof for writes.  I suspect that once
     * they add any more new fields they will start using it.  We could
     * alternatively keep the check here and treat this is a user32.dll bug and
     * suppress it.
     */
    bool use_cbSize = TEST(SYSARG_READ, arg_info->flags);
    if (pre && use_cbSize) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     sizeof(safe.cbSize), mc, "WNDCLASSEX.cbSize");
    }
    if (safe_read(start, sizeof(safe), &safe)) {
        check_sysmem(check_type, sysnum, start,
                     use_cbSize ? safe.cbSize : sizeof(WNDCLASSEX), mc, "WNDCLASSEX");
        /* For WRITE there is no capacity here so nothing to check (i#505) */
        if ((pre && TEST(SYSARG_READ, arg_info->flags)) ||
            (!pre && TEST(SYSARG_WRITE, arg_info->flags))) {
                /* lpszMenuName can be from MAKEINTRESOURCE, and
                 * lpszClassName can be an atom
                 */
                if ((!use_cbSize || safe.cbSize > offsetof(WNDCLASSEX, lpszMenuName)) &&
                    !is_atom((void *)safe.lpszMenuName)) {
                    handle_cwstring(pre, sysnum, mc, "WNDCLASSEXW.lpszMenuName",
                                    (byte *) safe.lpszMenuName, 0, arg_info->flags,
                                    NULL, true);
                }
                if ((!use_cbSize || safe.cbSize > offsetof(WNDCLASSEX, lpszClassName)) &&
                    !is_int_resource((void *)safe.lpszClassName)) {
                    handle_cwstring(pre, sysnum, mc, "WNDCLASSEXW.lpszClassName",
                                    /* docs say 256 is max length: we read until
                                     * NULL though
                                     */
                                    (byte *) safe.lpszClassName, 0, arg_info->flags,
                                    NULL, true);
                }
        }
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true; /* handled */
}

bool
handle_clsmenuname_access(bool pre, int sysnum, dr_mcontext_t *mc,
                          uint arg_num,
                          const syscall_arg_t *arg_info,
                          app_pc start, uint size)
{
    uint check_type = SYSARG_CHECK_TYPE(arg_info->flags, pre);
    CLSMENUNAME safe;
    check_sysmem(check_type, sysnum, start, size, mc, "CLSMENUNAME");
    if (pre && !TEST(SYSARG_READ, arg_info->flags)) {
        /* looks like even the UNICODE_STRING is not set up: contains garbage,
         * so presumably kernel creates it and doesn't just write to Buffer
         */
        return true; /* handled */
    }
    /* FIXME i#487: CLSMENUNAME format is not fully known and doesn't seem
     * to match this, on win7 at least
     */
#if 0 /* disabled: see comment above */
    if (safe_read(start, sizeof(safe), &safe)) {
        if (!is_atom(safe.pszClientAnsiMenuName)) {
            handle_cstring(pre, sysnum, mc, "CLSMENUNAME.lpszMenuName",
                           safe.pszClientAnsiMenuName, 0, arg_info->flags,
                           NULL, true);
        }
        if (!is_atom(safe.pwszClientUnicodeMenuName)) {
            handle_cwstring(pre, sysnum, mc, "CLSMENUNAME.lpszMenuName",
                            (byte *) safe.pwszClientUnicodeMenuName, 0, arg_info->flags,
                            NULL, true);
        }
        /* XXX: I've seen the pusMenuName pointer itself be an atom, though
         * perhaps should also handle just the Buffer being an atom?
         */
        if (!is_atom(safe.pusMenuName)) {
            handle_unicode_string_access(pre, sysnum, mc, arg_num, arg_info,
                                         (byte *) safe.pusMenuName,
                                         sizeof(UNICODE_STRING), false);
        }
    } else
        WARN("WARNING: unable to read syscall param\n");
#endif
    return true; /* handled */
}

bool
handle_menuiteminfow_access(bool pre, int sysnum, dr_mcontext_t *mc,
                            uint arg_num,
                            const syscall_arg_t *arg_info,
                            app_pc start, uint size)
{
    uint check_type = SYSARG_CHECK_TYPE(arg_info->flags, pre);
    MENUITEMINFOW *real = (MENUITEMINFOW *) start;
    MENUITEMINFOW safe;
    bool check_dwTypeData = false;
    /* user must set cbSize for set or get */
    if (pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     sizeof(safe.cbSize), mc, "MENUITEMINFOW.cbSize");
    }
    if (safe_read(start, sizeof(safe), &safe)) {
        if (pre) {
            check_sysmem(MEMREF_CHECK_ADDRESSABLE, sysnum, start,
                         safe.cbSize, mc, "MENUITEMINFOW");
        }
        if (TEST(MIIM_BITMAP, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, hbmpItem)) {
            check_sysmem(check_type, sysnum, (byte *) &real->hbmpItem,
                         sizeof(real->hbmpItem), mc, "MENUITEMINFOW.hbmpItem");
        }
        if (TEST(MIIM_CHECKMARKS, safe.fMask)) {
            if (safe.cbSize > offsetof(MENUITEMINFOW, hbmpChecked)) {
                check_sysmem(check_type, sysnum, (byte *) &real->hbmpChecked,
                             sizeof(real->hbmpChecked), mc, "MENUITEMINFOW.hbmpChecked");
            }
            if (safe.cbSize > offsetof(MENUITEMINFOW, hbmpUnchecked)) {
                check_sysmem(check_type, sysnum, (byte *) &real->hbmpUnchecked,
                             sizeof(real->hbmpUnchecked), mc,
                             "MENUITEMINFOW.hbmpUnchecked");
            }
        }
        if (TEST(MIIM_DATA, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, dwItemData)) {
            check_sysmem(check_type, sysnum, (byte *) &real->dwItemData,
                         sizeof(real->dwItemData), mc, "MENUITEMINFOW.dwItemData");
        }
        if (TEST(MIIM_FTYPE, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, fType)) {
            check_sysmem(check_type, sysnum, (byte *) &real->fType,
                         sizeof(real->fType), mc, "MENUITEMINFOW.fType");
        }
        if (TEST(MIIM_ID, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, wID)) {
            check_sysmem(check_type, sysnum, (byte *) &real->wID,
                         sizeof(real->wID), mc, "MENUITEMINFOW.wID");
        }
        if (TEST(MIIM_STATE, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, fState)) {
            check_sysmem(check_type, sysnum, (byte *) &real->fState,
                         sizeof(real->fState), mc, "MENUITEMINFOW.fState");
        }
        if (TEST(MIIM_STRING, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, dwTypeData)) {
            check_sysmem(check_type, sysnum, (byte *) &real->dwTypeData,
                         sizeof(real->dwTypeData), mc, "MENUITEMINFOW.dwTypeData");
            check_dwTypeData = true;
        }
        if (TEST(MIIM_SUBMENU, safe.fMask) &&
            safe.cbSize > offsetof(MENUITEMINFOW, hSubMenu)) {
            check_sysmem(check_type, sysnum, (byte *) &real->hSubMenu,
                         sizeof(real->hSubMenu), mc, "MENUITEMINFOW.hSubMenu");
        }
        if (TEST(MIIM_TYPE, safe.fMask) &&
            !TESTANY(MIIM_BITMAP | MIIM_FTYPE | MIIM_STRING, safe.fMask)) {
            if (safe.cbSize > offsetof(MENUITEMINFOW, fType)) {
                check_sysmem(check_type, sysnum, (byte *) &real->fType,
                             sizeof(real->fType), mc, "MENUITEMINFOW.fType");
            }
            if (safe.cbSize > offsetof(MENUITEMINFOW, dwTypeData)) {
                check_sysmem(check_type, sysnum, (byte *) &real->dwTypeData,
                             sizeof(real->dwTypeData), mc, "MENUITEMINFOW.dwTypeData");
                check_dwTypeData = true;
            }
        }
        if (check_dwTypeData) {
            /* kernel sets safe.cch so we don't have to walk the string */
            check_sysmem(check_type, sysnum, (byte *) safe.dwTypeData,
                         (safe.cch + 1/*null*/) * sizeof(wchar_t),
                         mc, "MENUITEMINFOW.dwTypeData");
        }
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true; /* handled */
}

static void
handle_logfont(bool pre, void *drcontext, int sysnum, dr_mcontext_t *mc,
               byte *start, size_t size, uint arg_flags, LOGFONTW *safe)
{
    uint check_type = SYSARG_CHECK_TYPE(arg_flags, pre);
    LOGFONTW *font = (LOGFONTW *) start;
    if (pre && TEST(SYSARG_WRITE, arg_flags)) {
        check_sysmem(check_type, sysnum, start, size, mc, "LOGFONTW");
    } else {
        size_t check_sz;
        if (size == 0) {
            /* i#873: existing code passes in 0 for the size, which violates
             * the MSDN docs, yet the kernel doesn't care and still returns
             * success.  Thus we don't report as an error and we make
             * it work.
             */
            size = sizeof(LOGFONTW);
        }
        check_sz = MIN(size - offsetof(LOGFONTW, lfFaceName),
                       sizeof(font->lfFaceName));
        ASSERT(size >= offsetof(LOGFONTW, lfFaceName), "invalid size");
        check_sysmem(check_type, sysnum, start,
                     offsetof(LOGFONTW, lfFaceName), mc, "LOGFONTW");
        handle_cwstring(pre, sysnum, mc, "LOGFONTW.lfFaceName",
                        (byte *) &font->lfFaceName, check_sz, arg_flags,
                        (safe == NULL) ? NULL : (wchar_t *)&safe->lfFaceName, true);
    }
}

static void
handle_nonclientmetrics(bool pre, void *drcontext, int sysnum, dr_mcontext_t *mc,
                        byte *start, size_t size_specified,
                        uint arg_flags, NONCLIENTMETRICSW *safe)
{
    NONCLIENTMETRICSW *ptr_arg = (NONCLIENTMETRICSW *) start;
    NONCLIENTMETRICSW *ptr_safe;
    NONCLIENTMETRICSW ptr_local;
    uint check_type = SYSARG_CHECK_TYPE(arg_flags, pre);
    size_t size;
    if (safe != NULL)
        ptr_safe = safe;
    else {
        if (!safe_read(start, sizeof(ptr_local), &ptr_local)) {
            WARN("WARNING: unable to read syscall param\n");
            return;
        }
        ptr_safe = &ptr_local;
    }
    /* Turns out that despite user32!SystemParametersInfoA requiring both uiParam
     * and cbSize, it turns around and calls NtUserSystemParametersInfo w/o
     * initializing cbSize!  Plus, it passes the A size instead of the W size!
     * Ditto on SET where it keeps the A size in the temp struct cbSize.
     * So we don't check that ptr_arg->cbSize is defined for pre-write
     * and we pretty much ignore the uiParam and cbSize values except
     * post-write (kernel puts in the right size).  Crazy.
     */
    LOG(2, "NONCLIENTMETRICSW %s: sizeof(NONCLIENTMETRICSW)=%x, cbSize=%x, uiParam=%x\n",
        TEST(SYSARG_WRITE, arg_flags) ? "write" : "read",
        sizeof(NONCLIENTMETRICSW), ptr_safe->cbSize, size_specified);
    if (running_on_Win7_or_later()/*win7 seems to set cbSize properly, always*/ ||
        (!pre && TEST(SYSARG_WRITE, arg_flags)))
        size = ptr_safe->cbSize;
    else {
        /* MAX to handle future additions.  I don't think older versions
         * have smaller NONCLIENTMETRICSW than anywhere we're compiling.
         */
        size = MAX(sizeof(NONCLIENTMETRICSW), size_specified);
    }

    if (pre && TEST(SYSARG_WRITE, arg_flags)) {
        check_sysmem(check_type, sysnum, start, size, mc, "NONCLIENTMETRICSW");
    } else {
        size_t offs = 0;
        size_t check_sz = MIN(size, offsetof(NONCLIENTMETRICSW, lfCaptionFont));
        check_sysmem(check_type, sysnum, start, check_sz, mc, "NONCLIENTMETRICSW A");
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, sizeof(LOGFONTW));
        handle_logfont(pre, drcontext, sysnum, mc, (byte *) &ptr_arg->lfCaptionFont,
                       check_sz, arg_flags, &ptr_safe->lfCaptionFont);
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, offsetof(NONCLIENTMETRICSW, lfSmCaptionFont) -
                       offsetof(NONCLIENTMETRICSW, iSmCaptionWidth));
        check_sysmem(check_type, sysnum, (byte *) &ptr_arg->iSmCaptionWidth,
                     check_sz, mc, "NONCLIENTMETRICSW B");
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, sizeof(LOGFONTW));
        handle_logfont(pre, drcontext, sysnum, mc, (byte *) &ptr_arg->lfSmCaptionFont,
                       check_sz, arg_flags, &ptr_safe->lfSmCaptionFont);
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, offsetof(NONCLIENTMETRICSW, lfMenuFont) -
                       offsetof(NONCLIENTMETRICSW, iMenuWidth));
        check_sysmem(check_type, sysnum, (byte *) &ptr_arg->iMenuWidth,
                     check_sz, mc, "NONCLIENTMETRICSW B");
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, sizeof(LOGFONTW));
        handle_logfont(pre, drcontext, sysnum, mc, (byte *) &ptr_arg->lfMenuFont,
                       check_sz, arg_flags, &ptr_safe->lfMenuFont);
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, sizeof(LOGFONTW));
        handle_logfont(pre, drcontext, sysnum, mc, (byte *) &ptr_arg->lfStatusFont,
                       check_sz, arg_flags, &ptr_safe->lfStatusFont);
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, sizeof(LOGFONTW));
        handle_logfont(pre, drcontext, sysnum, mc, (byte *) &ptr_arg->lfMessageFont,
                       check_sz, arg_flags, &ptr_safe->lfMessageFont);
        offs += check_sz;
        if (offs >= size)
            return;

        /* there is another field on Vista */
        check_sz = size - offs;
        check_sysmem(check_type, sysnum, ((byte *)ptr_arg) + offs,
                     check_sz, mc, "NONCLIENTMETRICSW C");
    }
}

static void
handle_iconmetrics(bool pre, void *drcontext, int sysnum, dr_mcontext_t *mc,
                        byte *start, uint arg_flags, ICONMETRICSW *safe)
{
    ICONMETRICSW *ptr_arg = (ICONMETRICSW *) start;
    ICONMETRICSW *ptr_safe;
    ICONMETRICSW ptr_local;
    uint check_type = SYSARG_CHECK_TYPE(arg_flags, pre);
    size_t size;
    if (safe != NULL)
        ptr_safe = safe;
    else {
        if (!safe_read(start, sizeof(ptr_local), &ptr_local)) {
            WARN("WARNING: unable to read syscall param\n");
            return;
        }
        ptr_safe = &ptr_local;
    }
    size = ptr_safe->cbSize;

    if (pre && TEST(SYSARG_WRITE, arg_flags)) {
        check_sysmem(check_type, sysnum, start, size, mc, "ICONMETRICSW");
    } else {
        size_t offs = 0;
        size_t check_sz = MIN(size, offsetof(ICONMETRICSW, lfFont));
        check_sysmem(check_type, sysnum, start, check_sz, mc, "ICONMETRICSW A");
        offs += check_sz;
        if (offs >= size)
            return;

        check_sz = MIN(size - offs, sizeof(LOGFONTW));
        handle_logfont(pre, drcontext, sysnum, mc, (byte *) &ptr_arg->lfFont,
                       check_sz, arg_flags, &ptr_safe->lfFont);
        offs += check_sz;
        if (offs >= size)
            return;

        /* currently no more args, but here for forward compat */
        check_sz = size - offs;
        check_sysmem(check_type, sysnum, ((byte *)ptr_arg) + offs,
                     check_sz, mc, "ICONMETRICSW B");
    }
}

static void
handle_serialkeys(bool pre, void *drcontext, int sysnum, dr_mcontext_t *mc,
                  byte *start, uint arg_flags, SERIALKEYSW *safe)
{
    SERIALKEYSW *ptr_safe;
    SERIALKEYSW ptr_local;
    uint check_type = SYSARG_CHECK_TYPE(arg_flags, pre);
    size_t size;
    if (safe != NULL)
        ptr_safe = safe;
    else {
        if (!safe_read(start, sizeof(ptr_local), &ptr_local)) {
            WARN("WARNING: unable to read syscall param\n");
            return;
        }
        ptr_safe = &ptr_local;
    }
    size = ptr_safe->cbSize;
    check_sysmem(check_type, sysnum, start, size, mc, "SERIALKEYSW");
    handle_cwstring(pre, sysnum, mc, "SERIALKEYSW.lpszActivePort",
                    (byte *) ptr_safe->lpszActivePort, 0, arg_flags, NULL, true);
    handle_cwstring(pre, sysnum, mc, "SERIALKEYSW.lpszPort",
                    (byte *) ptr_safe->lpszPort, 0, arg_flags, NULL, true);
}

static void
handle_cwstring_field(bool pre, int sysnum, dr_mcontext_t *mc, const char *id,
                      uint arg_flags,
                      byte *struct_start, size_t struct_size, size_t cwstring_offs)
{
    wchar_t *ptr;
    uint check_type = SYSARG_CHECK_TYPE(arg_flags, pre);
    if (struct_size <= cwstring_offs)
        return;
    if (!safe_read(struct_start + cwstring_offs, sizeof(ptr), &ptr)) {
        WARN("WARNING: unable to read syscall param\n");
        return;
    }
    handle_cwstring(pre, sysnum, mc, id, (byte *)ptr, 0, arg_flags, NULL, true);
}

bool
wingdi_process_syscall_arg(bool pre, int sysnum, dr_mcontext_t *mc, uint arg_num,
                           const syscall_arg_t *arg_info, app_pc start, uint size)
{
    switch (arg_info->misc) {
    case SYSARG_TYPE_LARGE_STRING:
        return handle_large_string_access(pre, sysnum, mc, arg_num,
                                          arg_info, start, size);
    case SYSARG_TYPE_DEVMODEW:
        return handle_devmodew_access(pre, sysnum, mc, arg_num, arg_info, start, size);
    case SYSARG_TYPE_WNDCLASSEXW:
        return handle_wndclassexw_access(pre, sysnum, mc, arg_num,
                                         arg_info, start, size);
    case SYSARG_TYPE_CLSMENUNAME:
        return handle_clsmenuname_access(pre, sysnum, mc, arg_num,
                                         arg_info, start, size);
    case SYSARG_TYPE_MENUITEMINFOW:
        return handle_menuiteminfow_access(pre, sysnum, mc, arg_num,
                                           arg_info, start, size);
    }
    return false; /* not handled */
}

/***************************************************************************
 * CUSTOM SYSCALL HANDLING
 */

static bool
handle_UserSystemParametersInfo(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                                dr_mcontext_t *mc)
{
    UINT uiAction = (UINT) pt->sysarg[0];
    UINT uiParam = (UINT) pt->sysarg[1];
    byte *pvParam = (byte *) pt->sysarg[2];
    bool get = true;
    size_t sz = 0;
    bool uses_pvParam = false; /* also considered used if sz>0 */
    bool uses_uiParam = false;

    switch (uiAction) {
    case SPI_GETBEEP: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETBEEP: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSE: get = true;  sz = 3 * sizeof(INT); break;
    case SPI_SETMOUSE: get = false; sz = 3 * sizeof(INT); break;
    case SPI_GETBORDER: get = true;  sz = sizeof(int); break;
    case SPI_SETBORDER: get = false; uses_uiParam = true; break;
    case SPI_GETKEYBOARDSPEED: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETKEYBOARDSPEED: get = false; uses_uiParam = true; break;
    case SPI_GETSCREENSAVETIMEOUT: get = true;  sz = sizeof(int); break;
    case SPI_SETSCREENSAVETIMEOUT: get = false; uses_uiParam = true; break;
    case SPI_GETSCREENSAVEACTIVE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSCREENSAVEACTIVE: get = false; uses_uiParam = true; break;
    /* XXX: no official docs for these 2: */
    case SPI_GETGRIDGRANULARITY: get = true;  sz = sizeof(int); break;
    case SPI_SETGRIDGRANULARITY: get = false; uses_uiParam = true; break;
    case SPI_GETDESKWALLPAPER: {
        /* uiParam is size in characters */
        handle_cwstring(pre, sysnum, mc, "pvParam", pvParam,
                        uiParam * sizeof(wchar_t), SYSARG_WRITE, NULL, true);
        get = true;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_SETDESKWALLPAPER: {
        syscall_arg_t arg = {0, sizeof(UNICODE_STRING),
                             SYSARG_READ|SYSARG_COMPLEX_TYPE,
                             SYSARG_TYPE_UNICODE_STRING};
        handle_unicode_string_access(pre, sysnum, mc, 0/*unused*/,
                                     &arg, pvParam, sizeof(UNICODE_STRING), false);
        get = false;
        uses_pvParam = true;
        break;
    }
    case SPI_SETDESKPATTERN: get = false; break;
    case SPI_GETKEYBOARDDELAY: get = true;  sz = sizeof(int); break;
    case SPI_SETKEYBOARDDELAY: get = false; uses_uiParam = true; break;
    case SPI_ICONHORIZONTALSPACING: {
        if (pvParam != NULL) {
            get = true; 
            sz = sizeof(int);
        } else {
            get = false; 
            uses_uiParam = true;
        }
        break;
    }
    case SPI_ICONVERTICALSPACING: {
        if (pvParam != NULL) {
            get = true; 
            sz = sizeof(int);
        } else {
            get = false; 
            uses_uiParam = true;
        }
        break;
    }
    case SPI_GETICONTITLEWRAP: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETICONTITLEWRAP: get = false; uses_uiParam = true; break;
    case SPI_GETMENUDROPALIGNMENT: get = true;  sz = sizeof(int); break;
    case SPI_SETMENUDROPALIGNMENT: get = false; uses_uiParam = true; break;
    case SPI_SETDOUBLECLKWIDTH: get = false; uses_uiParam = true; break;
    case SPI_SETDOUBLECLKHEIGHT: get = false; uses_uiParam = true; break;
    case SPI_GETICONTITLELOGFONT: {
        handle_logfont(pre, drcontext, sysnum, mc, pvParam,
                       uiParam, SYSARG_WRITE, NULL);
        get = true;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_SETICONTITLELOGFONT: {
        handle_logfont(pre, drcontext, sysnum, mc, pvParam,
                       uiParam, SYSARG_READ, NULL);
        get = false;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_SETDOUBLECLICKTIME: get = false; uses_uiParam = true; break;
    case SPI_SETMOUSEBUTTONSWAP: get = false; uses_uiParam = true; break;
    /* XXX: no official docs: */
    case SPI_GETFASTTASKSWITCH: get = true;  sz = sizeof(int); break;
    case SPI_GETDRAGFULLWINDOWS: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETDRAGFULLWINDOWS: get = false; uses_uiParam = true; break;
    case SPI_GETNONCLIENTMETRICS: {
        handle_nonclientmetrics(pre, drcontext, sysnum, mc, pvParam, uiParam,
                                SYSARG_WRITE, NULL);
        get = true;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_SETNONCLIENTMETRICS: {
        handle_nonclientmetrics(pre, drcontext, sysnum, mc, pvParam, uiParam,
                                SYSARG_READ, NULL);
        get = false;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_GETMINIMIZEDMETRICS: get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETMINIMIZEDMETRICS: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETICONMETRICS: {
        handle_iconmetrics(pre, drcontext, sysnum, mc, pvParam, SYSARG_WRITE, NULL);
        get = true;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_SETICONMETRICS: {
        handle_iconmetrics(pre, drcontext, sysnum, mc, pvParam, SYSARG_READ, NULL);
        get = false;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_GETWORKAREA: get = true;  sz = sizeof(RECT); break;
    case SPI_SETWORKAREA: get = false; sz = sizeof(RECT); break;
    case SPI_GETFILTERKEYS: get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETFILTERKEYS: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETTOGGLEKEYS: get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETTOGGLEKEYS: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETMOUSEKEYS:  get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETMOUSEKEYS:  get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETSHOWSOUNDS: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSHOWSOUNDS: get = false; uses_uiParam = true; break;
    case SPI_GETSTICKYKEYS: get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETSTICKYKEYS: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETACCESSTIMEOUT: get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETACCESSTIMEOUT: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETSERIALKEYS: {
        handle_serialkeys(pre, drcontext, sysnum, mc, pvParam, SYSARG_WRITE, NULL);
        get = true;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_SETSERIALKEYS: {
        handle_serialkeys(pre, drcontext, sysnum, mc, pvParam, SYSARG_READ, NULL);
        get = false;
        uses_uiParam = true;
        uses_pvParam = true;
        break;
    }
    case SPI_GETSOUNDSENTRY: {
        handle_cwstring_field(pre, sysnum, mc, "SOUNDSENTRYW.lpszWindowsEffectDLL",
                              SYSARG_WRITE, pvParam, uiParam,
                              offsetof(SOUNDSENTRYW, lpszWindowsEffectDLL));
        /* rest of struct handled through pvParam check below */
        get = true;
        uses_uiParam = true;
        sz = uiParam;
        break;
    }
    case SPI_SETSOUNDSENTRY: {
        handle_cwstring_field(pre, sysnum, mc, "SOUNDSENTRYW.lpszWindowsEffectDLL",
                              SYSARG_READ, pvParam, uiParam,
                              offsetof(SOUNDSENTRYW, lpszWindowsEffectDLL));
        /* rest of struct handled through pvParam check below */
        get = false;
        uses_uiParam = true;
        sz = uiParam;
        break;
    }
    case SPI_GETHIGHCONTRAST: {
        handle_cwstring_field(pre, sysnum, mc, "HIGHCONTRASTW.lpszDefaultScheme",
                              SYSARG_WRITE, pvParam, uiParam,
                              offsetof(HIGHCONTRASTW, lpszDefaultScheme));
        /* rest of struct handled through pvParam check below */
        get = true;
        uses_uiParam = true;
        sz = uiParam;
        break;
    }
    case SPI_SETHIGHCONTRAST: {
        handle_cwstring_field(pre, sysnum, mc, "HIGHCONTRASTW.lpszDefaultScheme",
                              SYSARG_READ, pvParam, uiParam,
                              offsetof(HIGHCONTRASTW, lpszDefaultScheme));
        /* rest of struct handled through pvParam check below */
        get = false;
        uses_uiParam = true;
        sz = uiParam;
        break;
    }
    case SPI_GETKEYBOARDPREF: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETKEYBOARDPREF: get = false; uses_uiParam = true; break;
    case SPI_GETSCREENREADER: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSCREENREADER: get = false; uses_uiParam = true; break;
    case SPI_GETANIMATION: get = true;  uses_uiParam = true; sz = uiParam; break;
    case SPI_SETANIMATION: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETFONTSMOOTHING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETFONTSMOOTHING: get = false; uses_uiParam = true; break;
    case SPI_SETDRAGWIDTH: get = false; uses_uiParam = true; break;
    case SPI_SETDRAGHEIGHT: get = false; uses_uiParam = true; break;
    /* XXX: no official docs: */
    case SPI_SETHANDHELD: get = false; uses_uiParam = true; break;
    case SPI_GETLOWPOWERTIMEOUT: get = true;  sz = sizeof(int); break;
    case SPI_GETPOWEROFFTIMEOUT: get = true;  sz = sizeof(int); break;
    case SPI_SETLOWPOWERTIMEOUT: get = false; uses_uiParam = true; break;
    case SPI_SETPOWEROFFTIMEOUT: get = false; uses_uiParam = true; break;
    case SPI_GETLOWPOWERACTIVE: get = true;  sz = sizeof(BOOL); break;
    case SPI_GETPOWEROFFACTIVE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETLOWPOWERACTIVE: get = false; uses_uiParam = true; break;
    case SPI_SETPOWEROFFACTIVE: get = false; uses_uiParam = true; break;
    /* XXX: docs say to set uiParam=0 and pvParam=NULL; we don't check init */
    case SPI_SETCURSORS: get = false; break;
    case SPI_SETICONS: get = false; break;
    case SPI_GETDEFAULTINPUTLANG: get = true;  sz = sizeof(HKL); break;
    case SPI_SETDEFAULTINPUTLANG: get = false; sz = sizeof(HKL); break;
    case SPI_SETLANGTOGGLE: get = false; break;
    case SPI_GETMOUSETRAILS: get = true;  sz = sizeof(int); break;
    case SPI_SETMOUSETRAILS: get = false; uses_uiParam = true; break;
    case SPI_GETSNAPTODEFBUTTON: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSNAPTODEFBUTTON: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSEHOVERWIDTH: get = true;  sz = sizeof(UINT); break;
    case SPI_SETMOUSEHOVERWIDTH: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSEHOVERHEIGHT: get = true;  sz = sizeof(UINT); break;
    case SPI_SETMOUSEHOVERHEIGHT: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSEHOVERTIME: get = true;  sz = sizeof(UINT); break;
    case SPI_SETMOUSEHOVERTIME: get = false; uses_uiParam = true; break;
    case SPI_GETWHEELSCROLLLINES: get = true;  sz = sizeof(UINT); break;
    case SPI_SETWHEELSCROLLLINES: get = false; uses_uiParam = true; break;
    case SPI_GETMENUSHOWDELAY: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETMENUSHOWDELAY: get = false; uses_uiParam = true; break;
    case SPI_GETWHEELSCROLLCHARS: get = true;  sz = sizeof(UINT); break;
    case SPI_SETWHEELSCROLLCHARS: get = false; uses_uiParam = true; break;
    case SPI_GETSHOWIMEUI: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSHOWIMEUI: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSESPEED: get = true;  sz = sizeof(int); break;
    case SPI_SETMOUSESPEED: get = false; uses_uiParam = true; break;
    case SPI_GETSCREENSAVERRUNNING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSCREENSAVERRUNNING: get = false; uses_uiParam = true; break;
    case SPI_GETAUDIODESCRIPTION: get = true;  uses_uiParam = true; sz = uiParam; break;
    /* XXX: docs don't actually say to set uiParam: I'm assuming for symmetry */
    case SPI_SETAUDIODESCRIPTION: get = false; uses_uiParam = true; sz = uiParam; break;
    case SPI_GETSCREENSAVESECURE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSCREENSAVESECURE: get = false; uses_uiParam = true; break;
    case SPI_GETHUNGAPPTIMEOUT: get = true;  sz = sizeof(int); break;
    case SPI_SETHUNGAPPTIMEOUT: get = false; uses_uiParam = true; break;
    case SPI_GETWAITTOKILLTIMEOUT: get = true;  sz = sizeof(int); break;
    case SPI_SETWAITTOKILLTIMEOUT: get = false; uses_uiParam = true; break;
    case SPI_GETWAITTOKILLSERVICETIMEOUT: get = true;  sz = sizeof(int); break;
    case SPI_SETWAITTOKILLSERVICETIMEOUT: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSEDOCKTHRESHOLD: get = true;  sz = sizeof(DWORD); break;
    /* Note that many of the sets below use pvParam as either an inlined BOOL
     * or a pointer to a DWORD (why not inlined?), instead of using uiParam
     */
    case SPI_SETMOUSEDOCKTHRESHOLD: get = false; sz = sizeof(DWORD); break;
    /* XXX: docs don't say it writes to pvParam: ret val instead? */
    case SPI_GETPENDOCKTHRESHOLD: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETPENDOCKTHRESHOLD: get = false; sz = sizeof(DWORD); break;
    case SPI_GETWINARRANGING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETWINARRANGING: get = false; uses_pvParam = true; break;
    /* XXX: docs don't say it writes to pvParam: ret val instead? */
    case SPI_GETMOUSEDRAGOUTTHRESHOLD: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETMOUSEDRAGOUTTHRESHOLD: get = false; sz = sizeof(DWORD); break;
    /* XXX: docs don't say it writes to pvParam: ret val instead? */
    case SPI_GETPENDRAGOUTTHRESHOLD: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETPENDRAGOUTTHRESHOLD: get = false; sz = sizeof(DWORD); break;
    /* XXX: docs don't say it writes to pvParam: ret val instead? */
    case SPI_GETMOUSESIDEMOVETHRESHOLD: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETMOUSESIDEMOVETHRESHOLD: get = false; sz = sizeof(DWORD); break;
    /* XXX: docs don't say it writes to pvParam: ret val instead? */
    case SPI_GETPENSIDEMOVETHRESHOLD: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETPENSIDEMOVETHRESHOLD: get = false; sz = sizeof(DWORD); break;
    case SPI_GETDRAGFROMMAXIMIZE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETDRAGFROMMAXIMIZE: get = false; uses_pvParam = true; break;
    case SPI_GETSNAPSIZING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSNAPSIZING:  get = false; uses_pvParam = true; break;
    case SPI_GETDOCKMOVING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETDOCKMOVING:  get = false; uses_pvParam = true; break;
    case SPI_GETACTIVEWINDOWTRACKING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETACTIVEWINDOWTRACKING: get = false; uses_pvParam = true; break;
    case SPI_GETMENUANIMATION: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETMENUANIMATION: get = false; uses_pvParam = true; break;
    case SPI_GETCOMBOBOXANIMATION: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETCOMBOBOXANIMATION: get = false; uses_pvParam = true; break;
    case SPI_GETLISTBOXSMOOTHSCROLLING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETLISTBOXSMOOTHSCROLLING: get = false; uses_pvParam = true; break;
    case SPI_GETGRADIENTCAPTIONS: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETGRADIENTCAPTIONS: get = false; uses_pvParam = true; break;
    case SPI_GETKEYBOARDCUES: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETKEYBOARDCUES: get = false; uses_pvParam = true; break;
    case SPI_GETACTIVEWNDTRKZORDER: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETACTIVEWNDTRKZORDER: get = false; uses_pvParam = true; break;
    case SPI_GETHOTTRACKING: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETHOTTRACKING: get = false; uses_pvParam = true; break;
    case SPI_GETMENUFADE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETMENUFADE: get = false; uses_pvParam = true; break;
    case SPI_GETSELECTIONFADE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSELECTIONFADE: get = false; uses_pvParam = true; break;
    case SPI_GETTOOLTIPANIMATION: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETTOOLTIPANIMATION: get = false; uses_pvParam = true; break;
    case SPI_GETTOOLTIPFADE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETTOOLTIPFADE: get = false; uses_pvParam = true; break;
    case SPI_GETCURSORSHADOW: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETCURSORSHADOW: get = false; uses_pvParam = true; break;
    case SPI_GETMOUSESONAR: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETMOUSESONAR: get = false; uses_uiParam = true; break;
    case SPI_GETMOUSECLICKLOCK: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETMOUSECLICKLOCK: get = false; uses_pvParam = true; break;
    case SPI_GETMOUSEVANISH: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETMOUSEVANISH: get = false; uses_uiParam = true; break;
    case SPI_GETFLATMENU: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETFLATMENU: get = false; uses_uiParam = true; break;
    case SPI_GETDROPSHADOW: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETDROPSHADOW: get = false; uses_uiParam = true; break;
    case SPI_GETBLOCKSENDINPUTRESETS: get = true;  sz = sizeof(BOOL); break;
    /* yes this is uiParam in the midst of many pvParams */
    case SPI_SETBLOCKSENDINPUTRESETS: get = false; uses_uiParam = true; break;
    case SPI_GETUIEFFECTS: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETUIEFFECTS: get = false; uses_pvParam = true; break;
    case SPI_GETDISABLEOVERLAPPEDCONTENT: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETDISABLEOVERLAPPEDCONTENT: get = false; uses_uiParam = true; break;
    case SPI_GETCLIENTAREAANIMATION: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETCLIENTAREAANIMATION: get = false; uses_uiParam = true; break;
    case SPI_GETCLEARTYPE: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETCLEARTYPE: get = false; uses_uiParam = true; break;
    case SPI_GETSPEECHRECOGNITION: get = true;  sz = sizeof(BOOL); break;
    case SPI_SETSPEECHRECOGNITION: get = false; uses_uiParam = true; break;
    case SPI_GETFOREGROUNDLOCKTIMEOUT: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETFOREGROUNDLOCKTIMEOUT: get = false; uses_pvParam = true; break;
    case SPI_GETACTIVEWNDTRKTIMEOUT: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETACTIVEWNDTRKTIMEOUT: get = false; uses_pvParam = true; break;
    case SPI_GETFOREGROUNDFLASHCOUNT: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETFOREGROUNDFLASHCOUNT: get = false; uses_pvParam = true; break;
    case SPI_GETCARETWIDTH: get = true;  sz = sizeof(DWORD); break;
    case SPI_SETCARETWIDTH: get = false; uses_pvParam = true; break;
    case SPI_GETMOUSECLICKLOCKTIME: get = true;  sz = sizeof(DWORD); break;
    /* yes this is uiParam in the midst of many pvParams */
    case SPI_SETMOUSECLICKLOCKTIME: get = false; uses_uiParam = true; break;
    case SPI_GETFONTSMOOTHINGTYPE: get = true;  sz = sizeof(UINT); break;
    case SPI_SETFONTSMOOTHINGTYPE: get = false; uses_pvParam = true; break;
    case SPI_GETFONTSMOOTHINGCONTRAST: get = true;  sz = sizeof(UINT); break;
    case SPI_SETFONTSMOOTHINGCONTRAST: get = false; uses_pvParam = true; break;
    case SPI_GETFOCUSBORDERWIDTH: get = true;  sz = sizeof(UINT); break;
    case SPI_SETFOCUSBORDERWIDTH: get = false; uses_pvParam = true; break;
    case SPI_GETFOCUSBORDERHEIGHT: get = true;  sz = sizeof(UINT); break;
    case SPI_SETFOCUSBORDERHEIGHT: get = false; uses_pvParam = true; break;
    case SPI_GETFONTSMOOTHINGORIENTATION: get = true;  sz = sizeof(UINT); break;
    case SPI_SETFONTSMOOTHINGORIENTATION: get = false; uses_pvParam = true; break;
    case SPI_GETMESSAGEDURATION: get = true;  sz = sizeof(ULONG); break;
    case SPI_SETMESSAGEDURATION: get = false; uses_pvParam = true; break;

    /* XXX: unknown behavior */
    case SPI_LANGDRIVER:
    case SPI_SETFASTTASKSWITCH:
    case SPI_SETPENWINDOWS:
    case SPI_GETWINDOWSEXTENSION:
    default:
        WARN("WARNING: unhandled UserSystemParametersInfo uiAction 0x%x\n",
             uiAction);
    }

    /* table entry only checked uiAction for definedness */
    if (uses_uiParam && pre)
        check_sysparam_defined(sysnum, 1, mc, sizeof(reg_t));
    if (sz > 0 || uses_pvParam) { /* pvParam is used */
        if (pre)
            check_sysparam_defined(sysnum, 2, mc, sizeof(reg_t));
        if (get && sz > 0) {
            check_sysmem(pre ? MEMREF_CHECK_ADDRESSABLE : MEMREF_WRITE, sysnum, 
                         pvParam, sz, mc, "pvParam");
        } else if (pre && sz > 0)
            check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, pvParam, sz, mc, "pvParam");
    }
    if (!get && pre) /* fWinIni used for all SET codes */
        check_sysparam_defined(sysnum, 3, mc, sizeof(reg_t));

    return true;
}

static bool
handle_UserMenuInfo(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                        dr_mcontext_t *mc)
{
    /* 3rd param is bool saying whether it's Set or Get */
    BOOL set = (BOOL) pt->sysarg[3];
    uint check_type = SYSARG_CHECK_TYPE(set ? SYSARG_READ : SYSARG_WRITE, pre);
    MENUINFO info;
    /* user must set cbSize for set or get */
    if (pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, (byte *) pt->sysarg[1],
                     sizeof(info.cbSize), mc, "MENUINFOW.cbSize");
    }
    if (safe_read((byte *) pt->sysarg[3], sizeof(info), &info)) {
        check_sysmem(check_type, sysnum, (byte *) pt->sysarg[3],
                     info.cbSize, mc, "MENUINFOW");
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true;
}

static bool
handle_UserMenuItemInfo(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                        dr_mcontext_t *mc)
{
    /* 4th param is bool saying whether it's Set or Get */
    BOOL set = (BOOL) pt->sysarg[4];
    syscall_arg_t arg = {0, 0,
                         (set ? SYSARG_READ : SYSARG_WRITE)|SYSARG_COMPLEX_TYPE,
                         SYSARG_TYPE_MENUITEMINFOW};
    handle_menuiteminfow_access(pre, sysnum, mc, 0/*unused*/,
                                &arg, (byte *) pt->sysarg[3], 0);
    return true;
}

static bool
handle_UserGetAltTabInfo(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                         dr_mcontext_t *mc)
{
    /* buffer is ansi or unicode depending on arg 5; size (arg 4) is in chars */
    BOOL ansi = (BOOL) pt->sysarg[5];
    uint check_type = SYSARG_CHECK_TYPE(SYSARG_WRITE, pre);
    UINT count = (UINT) pt->sysarg[4];
    check_sysmem(check_type, sysnum, (byte *) pt->sysarg[3],
                 count * (ansi ? sizeof(char) : sizeof(wchar_t)),
                 mc, "pszItemText");
    return true;
}

static bool
handle_UserGetRawInputBuffer(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                             dr_mcontext_t *mc)
{
    uint check_type = SYSARG_CHECK_TYPE(SYSARG_WRITE, pre);
    byte *buf = (byte *) pt->sysarg[0];
    UINT size;
    if (buf == NULL) {
        /* writes out total buffer size needed in bytes to param #1 */
        check_sysmem(check_type, sysnum, (byte *) pt->sysarg[1],
                     sizeof(UINT), mc, "pcbSize");
    } else {
        if (pre) {
            /* FIXME i#485: we don't know the number of array entries so we
             * can't check addressability pre-syscall: comes from a prior
             * buf==NULL call
             */
        } else if (safe_read((byte *) pt->sysarg[1], sizeof(size), &size)) {
            /* param #1 holds size of each RAWINPUT array entry */
            size = (size * dr_syscall_get_result(drcontext)) +
                /* param #2 holds header size */
                (UINT) pt->sysarg[2];
            check_sysmem(check_type, sysnum, buf, size, mc, "pData");
        } else
            WARN("WARNING: unable to read syscall param\n");
    }
    return true;
}

static bool
handle_UserGetRawInputData(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                           dr_mcontext_t *mc)
{
    byte *buf = (byte *) pt->sysarg[2];
    /* arg #3 is either R or W.  when W buf must be NULL and the 2,-3,WI entry
     * will do a safe_read but won't do a check so no false pos.
     */
    uint check_type = SYSARG_CHECK_TYPE((buf == NULL) ? SYSARG_WRITE : SYSARG_READ, pre);
    check_sysmem(check_type, sysnum, (byte *) pt->sysarg[3], sizeof(UINT), mc, "pcbSize");
    return true;
}

static bool
handle_UserGetRawInputDeviceInfo(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                                 dr_mcontext_t *mc)
{
    uint check_type = SYSARG_CHECK_TYPE(SYSARG_WRITE, pre);
    UINT uiCommand = (UINT) pt->sysarg[1];
    UINT size;
    if (safe_read((byte *) pt->sysarg[3], sizeof(size), &size)) {
        /* for uiCommand == RIDI_DEVICEINFO we assume pcbSize (3rd param)
         * will be set and we don't bother to check RID_DEVICE_INFO.cbSize
         */
        if (uiCommand == RIDI_DEVICENAME) {
            /* output is a string and size is in chars
             * XXX: I'm assuming a wide string!
             */
            size *= sizeof(wchar_t);
        }
        check_sysmem(check_type, sysnum, (byte *) pt->sysarg[2], size, mc, "pData");
        if (pt->sysarg[2] == 0) {
            /* XXX i#486: if buffer is not large enough, returns -1 but still
             * sets *pcbSize
             */
            check_sysmem(check_type, sysnum, (byte *) pt->sysarg[3],
                         sizeof(UINT), mc, "pData");
        }
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true;
}

static bool
handle_UserTrackMouseEvent(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                           dr_mcontext_t *mc)
{
    DWORD dwFlags = (BOOL) pt->sysarg[3];
    TRACKMOUSEEVENT *safe;
    byte buf[offsetof(TRACKMOUSEEVENT, dwFlags) + sizeof(safe->dwFlags)];
    /* user must set cbSize and dwFlags */
    if (pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, (byte *) pt->sysarg[0],
                     offsetof(TRACKMOUSEEVENT, dwFlags) + sizeof(safe->dwFlags),
                     mc, "TRACKMOUSEEVENT cbSize+dwFlags");
    }
    if (safe_read((byte *) pt->sysarg[0], BUFFER_SIZE_BYTES(buf), buf)) {
        uint check_type;
        safe = (TRACKMOUSEEVENT *) buf;
        /* XXX: for non-TME_QUERY are the other fields read? */
        check_type = SYSARG_CHECK_TYPE(TEST(TME_QUERY, safe->dwFlags) ?
                                       SYSARG_WRITE : SYSARG_READ, pre);
        if (safe->cbSize > BUFFER_SIZE_BYTES(buf)) {
            check_sysmem(check_type, sysnum,
                         ((byte *)pt->sysarg[0]) + BUFFER_SIZE_BYTES(buf),
                         safe->cbSize - BUFFER_SIZE_BYTES(buf), mc,
                         "TRACKMOUSEEVENT post-dwFlags");
        }
    } else
        WARN("WARNING: unable to read syscall param\n");
    return true;
}

static bool
handle_GdiCreateDIBSection(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                           dr_mcontext_t *mc)
{
    byte *dib;
    if (pre)
        return true;
    if (safe_read((byte *) pt->sysarg[8], sizeof(dib), &dib)) {
        /* XXX: move this into common/alloc.c since that's currently
         * driving all the known allocs, heap and otherwise
         */
        byte *dib_base;
        size_t dib_size;
        if (dr_query_memory(dib, &dib_base, &dib_size, NULL)) {
            LOG(SYSCALL_VERBOSE, "NtGdiCreateDIBSection created "PFX"-"PFX"\n",
                dib_base, dib_base+dib_size);
            client_handle_mmap(drcontext, dib_base, dib_size,
                               /* XXX: may not be file-backed but treating as
                                * all-defined and non-heap which is what this param
                                * does today.  could do dr_virtual_query().
                                */
                               true/*file-backed*/);
        } else
            WARN("WARNING: unable to query DIB section "PFX"\n", dib);
    } else
        WARN("WARNING: unable to read NtGdiCreateDIBSection param\n");
    /* When passed-in section pointer is NULL, the return value is
     * HBITMAP but doesn't seem to be a real memory address, which is
     * odd, b/c presumably when a section is used it would be a real
     * memory address, right?  The value is typically large so clearly
     * not just a table index.  Xref i#539.
     */
    return true;
}

static bool
handle_GdiHfontCreate(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                      dr_mcontext_t *mc)
{
    ENUMLOGFONTEXDVW dvw;
    ENUMLOGFONTEXDVW *real_dvw = (ENUMLOGFONTEXDVW *) pt->sysarg[0];
    if (pre && safe_read((byte *) pt->sysarg[0], sizeof(dvw), &dvw)) {
        uint i;
        byte *start = (byte *) pt->sysarg[0];
        ULONG total_size = (ULONG) pt->sysarg[1];
        /* Would be: {0,-1,R,}
         * Except not all fields need to be defined.
         * If any other syscall turns out to have this param type should
         * turn this into a type handler and not a syscall handler.
         */
        check_sysmem(MEMREF_CHECK_ADDRESSABLE, sysnum, start,
                     total_size, mc, "ENUMLOGFONTEXDVW");

        ASSERT(offsetof(ENUMLOGFONTEXDVW, elfEnumLogfontEx) == 0 &&
               offsetof(ENUMLOGFONTEXW, elfLogFont) == 0, "logfont structs changed");
        handle_logfont(pre, drcontext, sysnum, mc, start,
                       sizeof(LOGFONTW), SYSARG_READ, &dvw.elfEnumLogfontEx.elfLogFont);

        start = (byte *) &real_dvw->elfEnumLogfontEx.elfFullName;
        for (i = 0;
             i < sizeof(dvw.elfEnumLogfontEx.elfFullName)/sizeof(wchar_t) &&
                 dvw.elfEnumLogfontEx.elfFullName[i] != L'\0';
             i++)
            ; /* nothing */
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     i * sizeof(wchar_t), mc, "ENUMLOGFONTEXW.elfFullName");

        start = (byte *) &real_dvw->elfEnumLogfontEx.elfStyle;
        for (i = 0;
             i < sizeof(dvw.elfEnumLogfontEx.elfStyle)/sizeof(wchar_t) &&
                 dvw.elfEnumLogfontEx.elfStyle[i] != L'\0';
             i++)
            ; /* nothing */
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     i * sizeof(wchar_t), mc, "ENUMLOGFONTEXW.elfStyle");

        start = (byte *) &real_dvw->elfEnumLogfontEx.elfScript;
        for (i = 0;
             i < sizeof(dvw.elfEnumLogfontEx.elfScript)/sizeof(wchar_t) &&
                 dvw.elfEnumLogfontEx.elfScript[i] != L'\0';
             i++)
            ; /* nothing */
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     i * sizeof(wchar_t), mc, "ENUMLOGFONTEXW.elfScript");

        /* the dvValues of DESIGNVECTOR are optional: from 0 to 64 bytes */
        start = (byte *) &real_dvw->elfDesignVector;
        if (dvw.elfDesignVector.dvNumAxes > MM_MAX_NUMAXES) {
            dvw.elfDesignVector.dvNumAxes = MM_MAX_NUMAXES;
            WARN("WARNING: NtGdiHfontCreate design vector larger than max\n");
        }
        if ((start + offsetof(DESIGNVECTOR, dvValues) +
             dvw.elfDesignVector.dvNumAxes * sizeof(LONG)) -
            (byte*) pt->sysarg[0] != total_size) {
            WARN("WARNING: NtGdiHfontCreate total size doesn't match\n");
        }
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, start,
                     offsetof(DESIGNVECTOR, dvValues) +
                     dvw.elfDesignVector.dvNumAxes * sizeof(LONG),
                     mc, "DESIGNVECTOR");
    } else if (pre)
        WARN("WARNING: unable to read NtGdiHfontCreate param\n");
    return true;
}

static bool
handle_GdiDoPalette(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                    dr_mcontext_t *mc)
{
    /* Entry would read: {3,-2,R|SYSARG_SIZE_IN_ELEMENTS,sizeof(PALETTEENTRY)}
     * But pPalEntries is an OUT param if !bInbound.
     * It's a convenient arg: else would have to look at iFunc.
     */
    WORD cEntries = (WORD) pt->sysarg[2];
    PALETTEENTRY *pPalEntries = (PALETTEENTRY *) pt->sysarg[3];
    bool bInbound = (bool) pt->sysarg[5];
    if (bInbound && pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, (byte *) pPalEntries,
                     cEntries * sizeof(PALETTEENTRY), mc, "pPalEntries");
    } else if (!bInbound) {
        check_sysmem(pre ? MEMREF_CHECK_ADDRESSABLE : MEMREF_WRITE, sysnum,
                     (byte *) pPalEntries,
                     cEntries * sizeof(PALETTEENTRY), mc, "pPalEntries");
    }
    return true;
}

static bool
handle_GdiOpenDCW(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                  dr_mcontext_t *mc)
{
    /* An extra arg "BOOL bDisplay" was added as arg #4 in Vista so
     * we have to special-case the subsequent args, which for Vista+ are:
     *   {6,sizeof(DRIVER_INFO_2W),R,}, {7,sizeof(PUMDHPDEV *),W,},
     */
    uint num_driver = 5;
    uint num_pump = 6;
    if (running_on_Vista_or_later()) {
        if (pre)
            check_sysparam_defined(sysnum, 7, mc, sizeof(reg_t));
        num_driver = 6;
        num_pump = 7;
    }
    if (pre) {
        check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum,
                     (byte *) pt->sysarg[num_driver], sizeof(DRIVER_INFO_2W),
                     mc, "DRIVER_INFO_2W");
    }
    check_sysmem(pre ? MEMREF_CHECK_ADDRESSABLE : MEMREF_WRITE, sysnum,
                 (byte *) pt->sysarg[num_pump], sizeof(PUMDHPDEV *),
                 mc, "PUMDHPDEV*");
    return true;
}

bool
wingdi_shadow_process_syscall(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                              dr_mcontext_t *mc)
{
    /* handlers here do not check for success so we check up front */
    if (!pre) {
        syscall_info_t *sysinfo = syscall_lookup(sysnum);
        if (!os_syscall_succeeded(sysnum, sysinfo, dr_syscall_get_result(drcontext)))
            return true;
    }
    if (sysnum == sysnum_UserSystemParametersInfo) {
        return handle_UserSystemParametersInfo(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserMenuInfo) {
        return handle_UserMenuInfo(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserMenuItemInfo) {
        return handle_UserMenuItemInfo(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserGetAltTabInfo) {
        return handle_UserGetAltTabInfo(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserGetRawInputBuffer) {
        return handle_UserGetRawInputBuffer(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserGetRawInputData) {
        return handle_UserGetRawInputData(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserGetRawInputDeviceInfo) {
        return handle_UserGetRawInputDeviceInfo(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserTrackMouseEvent) {
        return handle_UserTrackMouseEvent(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_UserCreateWindowStation ||
               sysnum == sysnum_UserLoadKeyboardLayoutEx) {
        /* Vista SP1 added one arg (both were 7, now 8)
         * FIXME i#487: figure out what it is and whether we need to process it
         * for each of the two syscalls.
         * Also check whether it's defined after first deciding whether
         * we're on SP1: use core's method of checking for export?
         */
    } else if (sysnum == sysnum_GdiCreatePaletteInternal) {
        /* Entry would read: {0,cEntries * 4  + 4,R,} but see comment in ntgdi.h */
        if (pre) {
            UINT cEntries = (UINT) pt->sysarg[1];
            check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, (byte *)pt->sysarg[0],
                         sizeof(LOGPALETTE) - sizeof(PALETTEENTRY) +
                         sizeof(PALETTEENTRY) * cEntries, mc, "pLogPal");
        }
    } else if (sysnum == sysnum_GdiCheckBitmapBits) {
        /* Entry would read: {7,dwWidth * dwHeight,W,} */
        DWORD dwWidth = (DWORD) pt->sysarg[4];
        DWORD dwHeight = (DWORD) pt->sysarg[5];
        check_sysmem(pre ? MEMREF_CHECK_ADDRESSABLE : MEMREF_WRITE, sysnum,
                     (byte *)pt->sysarg[7], dwWidth * dwHeight, mc, "paResults");
    } else if (sysnum == sysnum_GdiCreateDIBSection) {
        return handle_GdiCreateDIBSection(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_GdiHfontCreate) {
        return handle_GdiHfontCreate(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_GdiDoPalette) {
        return handle_GdiDoPalette(pre, drcontext, sysnum, pt, mc);
    } else if (sysnum == sysnum_GdiExtTextOutW) {
        UINT fuOptions = (UINT) pt->sysarg[3];
        int cwc = (int) pt->sysarg[6];
        INT *pdx = (INT *) pt->sysarg[7];
        if (pre && TEST(ETO_PDY, fuOptions)) {
            /* pdx contains pairs of INTs.  regular entry already checked
             * size of singletons of INTs so here we check the extra size.
             */
            check_sysmem(MEMREF_CHECK_DEFINEDNESS, sysnum, ((byte *)pdx) + cwc*sizeof(INT),
                         cwc*sizeof(INT), mc, "pdx extra size from ETO_PDY");
        }
    } else if (sysnum == sysnum_GdiOpenDCW) {
        return handle_GdiOpenDCW(pre, drcontext, sysnum, pt, mc);
    } 
    return true; /* execute syscall */
}

/***************************************************************************
 * General (non-shadow/memory checking) system call handling
 */
/* Caller should check for success and only call if syscall is successful (for !pre) */
static void
syscall_check_gdi(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                  dr_mcontext_t *mc)
{
    ASSERT(options.check_gdi, "shouldn't be called");
    if (sysnum == sysnum_UserGetDC || sysnum == sysnum_UserGetDCEx ||
        sysnum == sysnum_UserGetWindowDC || sysnum == sysnum_UserBeginPaint ||
        sysnum == sysnum_GdiGetDCforBitmap || sysnum == sysnum_GdiDdGetDC) {
        if (!pre) {
            HDC hdc = (HDC) dr_syscall_get_result(drcontext);
            gdicheck_dc_alloc(hdc, false/*Get not Create*/, false, sysnum, mc);
            if (sysnum == sysnum_UserBeginPaint) {
                /* we store the hdc for access in EndPaint */
                pt->paintDC = hdc;
            }
        }
    } else if (sysnum == sysnum_GdiCreateMetafileDC ||
               sysnum == sysnum_GdiCreateCompatibleDC ||
               sysnum == sysnum_GdiOpenDCW) {
        if (!pre) {
            HDC hdc = (HDC) dr_syscall_get_result(drcontext);
            gdicheck_dc_alloc(hdc, true/*Create not Get*/,
                              (sysnum == sysnum_GdiCreateCompatibleDC &&
                               pt->sysarg[0] == 0),
                              sysnum, mc);
        }
    } else if (sysnum == sysnum_UserReleaseDC || sysnum == sysnum_UserEndPaint) {
        if (pre) {
            HDC hdc;
            if (sysnum == sysnum_UserReleaseDC)
                hdc = (HDC)pt->sysarg[0];
            else {
                hdc = pt->paintDC;
                pt->paintDC = NULL;
            }
            gdicheck_dc_free(hdc, false/*Get not Create*/, sysnum, mc);
        }
    } else if (sysnum == sysnum_GdiDeleteObjectApp) {
        if (pre)
            gdicheck_obj_free((HANDLE)pt->sysarg[0], sysnum, mc);
    }
}

bool
wingdi_shared_process_syscall(bool pre, void *drcontext, int sysnum, cls_syscall_t *pt,
                              dr_mcontext_t *mc)
{
    /* handlers here do not check for success so we check up front */
    if (!pre) {
        syscall_info_t *sysinfo = syscall_lookup(sysnum);
        if (!os_syscall_succeeded(sysnum, sysinfo, dr_syscall_get_result(drcontext)))
            return true;
    }
    if (options.check_gdi) {
        syscall_check_gdi(pre, drcontext, sysnum, pt, mc);
    }

    return true; /* execute syscall */
}


// ============================================================================
//  RoShim.cpp —— mingw 缺 combase 导入库，这里给出两个 WinRT 入口的转发实现
//  （不这样做，链接期会缺 RoGetActivationFactory / RoOriginateLanguageException）
// ============================================================================
#include <windows.h>
#include <roapi.h>

extern "C" HRESULT __stdcall RoGetActivationFactory(HSTRING classId, REFIID iid, void **factory)
{
    typedef HRESULT(__stdcall *Fn)(HSTRING, REFIID, void **);
    static Fn fn = nullptr;
    if (!fn) {
        HMODULE h = GetModuleHandleW(L"combase.dll");
        if (!h) h = LoadLibraryW(L"combase.dll");
        if (h) fn = (Fn)GetProcAddress(h, "RoGetActivationFactory");
    }
    if (!fn) return REGDB_E_CLASSNOTREG;
    return fn(classId, iid, factory);
}

extern "C" BOOL __stdcall RoOriginateLanguageException(HRESULT error, HSTRING message,
                                                       IUnknown *languageException)
{
    (void)error;
    (void)message;
    (void)languageException;
    return FALSE;
}

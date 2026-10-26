// ============================================================================
//  RainmeterApi.h —— 从 Rainmeter.dll 动态取 RmReadString
//
//  rainmeterapi.h 里那些 inline 包装是按「宿主传入的 void* rm」实现的，
// 但本机插件按老式 (file, section, id) 签名调用时拿到的是垃圾 id。
// 这里只解析一次导出地址，用 V3 的 (rm, option, default, replaceMeasures) 调用。
// ============================================================================
#pragma once

#include <windows.h>

namespace rmapi {

void resolve();
LPCWSTR read(void *rm, const wchar_t *key, const wchar_t *def);

} // namespace rmapi

// ============================================================================
//  Log.h —— 插件内轻量日志（追加写、UTF-8、控制台可选）
// ============================================================================
#pragma once

#include <string>

namespace logx {

// 控制台回显：探测工具用；插件里保持关闭
void set_console(bool on);

void write(const wchar_t *msg);
void writef(const wchar_t *fmt, ...);

} // namespace logx

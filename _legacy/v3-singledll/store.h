// ============================================================================
//  store.h —— 采样数据落地（SQLite）
// ============================================================================
#pragma once
#include <string>

namespace store {

// 打开/创建 <dir>\dormtemp.db；返回 false 表示数据库不可用（只显示、不记录）
bool init(const std::wstring &dir);

// 追加一条采样（同一秒内的重复通知会被丢弃）
void sample(double temp, double hum, double volt);

// 数据库当前是否可用
bool available();

} // namespace store

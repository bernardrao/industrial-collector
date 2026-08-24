#pragma once
/**
 * formula.h — 地址公式求值器
 *
 * 设备规格的测点地址不是常量，而是随实例位置变化的表达式：
 *
 *     "1000 + idx_PACK*128 + idx_CELL*2"
 *
 * 电芯的 Modbus 地址同时取决于【它在包内的序号】和【包在簇内的序号】，所以
 * 变量不能只有"自己的序号"—— 编译时把各级祖先的序号都注入进来，变量名按
 * 祖先的设备规格 code 命名（idx_PACK / idx_CLST …），另有 idx 作为本级序号的别名。
 *
 * ── 为什么是整数运算 ────────────────────────────────────────────────────────
 * 结果要当寄存器地址/CAN ID 用，浮点会引入 1999.9999 这种取整歧义。故字面量
 * 只接受整数（支持 0x 十六进制，CAN ID 常写作 0x18FF50E5），'/' 是整数除法，
 * '%' 是取模。写 0.5 会明确报错，而不是悄悄截断。
 *
 * ── 支持的语法 ──────────────────────────────────────────────────────────────
 *     expr    := term (('+'|'-') term)*
 *     term    := factor (('*'|'/'|'%') factor)*
 *     factor  := ('+'|'-')? primary
 *     primary := number | ident | '(' expr ')'
 *
 * 不支持函数调用、位运算、三目 —— 地址计算用不到，加了反而让"配置"变成"编程"，
 * 出错时也更难向现场人员解释。真需要时再加。
 */
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace industrial {

// 求值。失败返回 nullopt，err 内含【原因 + 出错位置】——公式是用户在 Web 上
// 手写的，只说"表达式错误"等于没说。
std::optional<int64_t> evalFormula(const std::string& expr,
                                   const std::map<std::string,int64_t>& vars,
                                   std::string& err);

// 只做语法检查（变量是否存在不管），供前端即时校验：用户还没选实例位置时
// 也能先告诉他括号少了一个。
bool checkFormulaSyntax(const std::string& expr, std::string& err);

} // namespace industrial

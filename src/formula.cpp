// src/formula.cpp — 地址公式求值器（递归下降）
#include "formula.h"

#include <cctype>
#include <limits>
#include <stdexcept>

namespace industrial {

namespace {

// 解析失败时带上位置：公式由用户手写，"第 12 字符处" 比 "语法错误" 有用得多
struct ParseError : std::runtime_error {
    ParseError(const std::string& msg, size_t pos)
        : std::runtime_error(msg), position(pos) {}
    size_t position;
};

class Parser {
public:
    Parser(const std::string& s, const std::map<std::string,int64_t>* vars,
           bool syntax_only)
        : s_(s), vars_(vars), syntax_only_(syntax_only) {}

    int64_t parse() {
        skipSpace();
        if (atEnd()) throw ParseError("公式为空", 0);
        int64_t v = expr();
        skipSpace();
        if (!atEnd())
            throw ParseError(std::string("多余字符 '") + s_[i_] + "'", i_);
        return v;
    }

private:
    const std::string&                    s_;
    const std::map<std::string,int64_t>*  vars_;
    bool                                  syntax_only_;
    size_t                                i_ = 0;

    bool atEnd() const { return i_ >= s_.size(); }
    void skipSpace() { while (!atEnd() && std::isspace((unsigned char)s_[i_])) ++i_; }

    // ── 溢出保护 ──
    // 地址公式写错（比如把 idx 乘成 1e9）不该悄悄回绕成一个看似合法的地址，
    // 那会让采集器去读一个风马牛不相及的寄存器。
    static int64_t addChecked(int64_t a, int64_t b, size_t pos) {
        int64_t r;
        if (__builtin_add_overflow(a, b, &r)) throw ParseError("数值溢出", pos);
        return r;
    }
    static int64_t subChecked(int64_t a, int64_t b, size_t pos) {
        int64_t r;
        if (__builtin_sub_overflow(a, b, &r)) throw ParseError("数值溢出", pos);
        return r;
    }
    static int64_t mulChecked(int64_t a, int64_t b, size_t pos) {
        int64_t r;
        if (__builtin_mul_overflow(a, b, &r)) throw ParseError("数值溢出", pos);
        return r;
    }

    int64_t expr() {
        int64_t v = term();
        for (;;) {
            skipSpace();
            if (atEnd()) return v;
            char c = s_[i_];
            if (c != '+' && c != '-') return v;
            size_t op = i_++;
            int64_t r = term();
            v = (c == '+') ? addChecked(v, r, op) : subChecked(v, r, op);
        }
    }

    int64_t term() {
        int64_t v = factor();
        for (;;) {
            skipSpace();
            if (atEnd()) return v;
            char c = s_[i_];
            if (c != '*' && c != '/' && c != '%') return v;
            size_t op = i_++;
            int64_t r = factor();
            if (c == '*') { v = mulChecked(v, r, op); continue; }
            // 语法检查模式下变量都当 0，除数为 0 属正常，不该报错
            if (r == 0) {
                if (syntax_only_) { v = 0; continue; }
                throw ParseError(c == '/' ? "除以零" : "对零取模", op);
            }
            // INT64_MIN / -1 会溢出
            if (v == std::numeric_limits<int64_t>::min() && r == -1)
                throw ParseError("数值溢出", op);
            v = (c == '/') ? v / r : v % r;
        }
    }

    int64_t factor() {
        skipSpace();
        if (atEnd()) throw ParseError("表达式意外结束", i_);
        if (s_[i_] == '+') { ++i_; return factor(); }
        if (s_[i_] == '-') { size_t p = i_++; return subChecked(0, factor(), p); }
        return primary();
    }

    int64_t primary() {
        skipSpace();
        if (atEnd()) throw ParseError("表达式意外结束", i_);

        if (s_[i_] == '(') {
            ++i_;
            int64_t v = expr();
            skipSpace();
            if (atEnd() || s_[i_] != ')') throw ParseError("缺少右括号 ')'", i_);
            ++i_;
            return v;
        }
        if (std::isdigit((unsigned char)s_[i_])) return number();
        if (std::isalpha((unsigned char)s_[i_]) || s_[i_] == '_') return ident();

        throw ParseError(std::string("无法识别的字符 '") + s_[i_] + "'", i_);
    }

    int64_t number() {
        const size_t start = i_;
        int64_t v = 0;

        // 0x 十六进制：CAN ID 惯用写法（0x18FF50E5）
        if (s_[i_] == '0' && i_ + 1 < s_.size() &&
            (s_[i_+1] == 'x' || s_[i_+1] == 'X')) {
            i_ += 2;
            if (atEnd() || !std::isxdigit((unsigned char)s_[i_]))
                throw ParseError("0x 后缺少十六进制数字", i_);
            while (!atEnd() && std::isxdigit((unsigned char)s_[i_])) {
                int d = std::isdigit((unsigned char)s_[i_])
                            ? s_[i_] - '0'
                            : (std::tolower(s_[i_]) - 'a' + 10);
                v = mulChecked(v, 16, start);
                v = addChecked(v, d, start);
                ++i_;
            }
            return v;
        }

        while (!atEnd() && std::isdigit((unsigned char)s_[i_])) {
            v = mulChecked(v, 10, start);
            v = addChecked(v, s_[i_] - '0', start);
            ++i_;
        }
        // 小数点：地址是整数，写 0.5 必须明确报错而非悄悄截断
        if (!atEnd() && s_[i_] == '.')
            throw ParseError("地址公式只支持整数，不能含小数点", i_);
        return v;
    }

    int64_t ident() {
        const size_t start = i_;
        while (!atEnd() &&
               (std::isalnum((unsigned char)s_[i_]) || s_[i_] == '_')) ++i_;
        const std::string name = s_.substr(start, i_ - start);

        if (syntax_only_) return 0;      // 只查语法时变量一律当 0

        auto it = vars_->find(name);
        if (it == vars_->end()) {
            // 把可用变量列出来 —— 用户多半是把 idx_PACK 写成了 idx_pack
            std::string avail;
            for (const auto& kv : *vars_) {
                if (!avail.empty()) avail += ", ";
                avail += kv.first;
            }
            throw ParseError("未知变量 '" + name + "'" +
                             (avail.empty() ? std::string("（此处无可用变量）")
                                            : std::string("，可用: ") + avail),
                             start);
        }
        return it->second;
    }
};

// 出错信息统一带上公式原文与插入符定位
std::string formatError(const std::string& expr, const std::string& msg, size_t pos) {
    std::string caret(pos, ' ');
    caret += '^';
    return msg + "\n    " + expr + "\n    " + caret;
}

} // namespace

std::optional<int64_t> evalFormula(const std::string& expr,
                                   const std::map<std::string,int64_t>& vars,
                                   std::string& err) {
    try {
        Parser p(expr, &vars, /*syntax_only=*/false);
        return p.parse();
    } catch (const ParseError& e) {
        err = formatError(expr, e.what(), e.position);
        return std::nullopt;
    } catch (const std::exception& e) {
        err = e.what();
        return std::nullopt;
    }
}

bool checkFormulaSyntax(const std::string& expr, std::string& err) {
    try {
        Parser p(expr, nullptr, /*syntax_only=*/true);
        p.parse();
        return true;
    } catch (const ParseError& e) {
        err = formatError(expr, e.what(), e.position);
        return false;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace industrial

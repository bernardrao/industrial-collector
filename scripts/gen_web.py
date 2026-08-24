#!/usr/bin/env python3
"""
scripts/gen_web.py  <web_dir>  <output_header.h>

编译时由 CMake 调用，将 web/ 目录下的静态资源打包成 C++ 头文件。
打包文件：index.html, assets/style.css, assets/app.js

生成内容：
  - 每个文件一个字节数组（含结尾 '\0'）
  - industrial::WEB_ASSETS[]：{ url_path, mime, data, size } 表
  - industrial::WEB_ASSET_COUNT

服务器据此自动注册路由，新增文件只需在下方 ASSETS 列表登记。
"""
import sys, os

# (磁盘相对路径, URL 路径, MIME 类型)
ASSETS = [
    ("index.html",          "/",                    "text/html; charset=utf-8"),
    ("index.html",          "/index.html",          "text/html; charset=utf-8"),
    ("assets/style.css",    "/assets/style.css",    "text/css; charset=utf-8"),
    ("assets/app.js",       "/assets/app.js",       "application/javascript; charset=utf-8"),
    ("assets/config.js",    "/assets/config.js",    "application/javascript; charset=utf-8"),
    ("assets/model.js",     "/assets/model.js",     "application/javascript; charset=utf-8"),
]

def c_ident(path):
    return "asset_" + "".join(c if c.isalnum() else "_" for c in path)

def main():
    if len(sys.argv) != 3:
        print(f"用法: {sys.argv[0]} <web_dir> <output.h>", file=sys.stderr)
        sys.exit(1)
    web_dir, out = sys.argv[1], sys.argv[2]
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)

    # 去重：同一磁盘文件只生成一个字节数组（如 index.html 对应 "/" 和 "/index.html"）
    files = {}
    for disk, _, _ in ASSETS:
        if disk not in files:
            with open(os.path.join(web_dir, disk), "rb") as f:
                files[disk] = f.read()

    with open(out, "w", encoding="utf-8") as f:
        f.write("// AUTO-GENERATED — do not edit; modify web/ files instead\n")
        f.write("#pragma once\n#include <cstddef>\n")
        f.write("namespace industrial {\n")

        # 各文件字节数组
        for disk, data in files.items():
            ident = c_ident(disk)
            f.write(f"// {disk} ({len(data)} bytes)\n")
            f.write(f"static const unsigned char {ident}[] = {{\n")
            for i, b in enumerate(data):
                f.write(f"0x{b:02x},")
                if (i + 1) % 16 == 0:
                    f.write("\n")
            f.write("0x00\n};\n")

        # 资源表
        f.write("\nstruct WebAsset { const char* path; const char* mime;"
                " const char* data; size_t size; };\n")
        f.write("static const WebAsset WEB_ASSETS[] = {\n")
        for disk, url, mime in ASSETS:
            ident = c_ident(disk)
            size  = len(files[disk])
            f.write(f'    {{ "{url}", "{mime}", '
                    f"reinterpret_cast<const char*>({ident}), {size} }},\n")
        f.write("};\n")
        f.write(f"static const size_t WEB_ASSET_COUNT = {len(ASSETS)};\n")
        f.write("} // namespace industrial\n")

    total = sum(len(d) for d in files.values())
    print(f"[gen_web] {len(files)} 文件 / {total} bytes → {out}")

if __name__ == "__main__":
    main()

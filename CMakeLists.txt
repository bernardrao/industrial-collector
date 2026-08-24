cmake_minimum_required(VERSION 3.14)
project(IndustrialCollector VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED THIRD_PARTY_DIR)
    set(THIRD_PARTY_DIR "${CMAKE_SOURCE_DIR}/third_party")
endif()
# 转为绝对路径：IMPORTED target 的 INTERFACE_INCLUDE_DIRECTORIES 不接受相对路径
get_filename_component(THIRD_PARTY_DIR "${THIRD_PARTY_DIR}" ABSOLUTE
                       BASE_DIR "${CMAKE_BINARY_DIR}")
message(STATUS "THIRD_PARTY_DIR = ${THIRD_PARTY_DIR}")

# ── nlohmann/json ─────────────────────────────────────────────────────────────
if(NOT EXISTS "${THIRD_PARTY_DIR}/nlohmann/json.hpp")
    message(FATAL_ERROR "缺少 nlohmann/json，请先运行 scripts/bootstrap.sh")
endif()
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE "${THIRD_PARTY_DIR}/nlohmann")

# ── cpp-httplib v0.12.6（无 SSL）─────────────────────────────────────────────
if(NOT EXISTS "${THIRD_PARTY_DIR}/httplib.h")
    message(FATAL_ERROR "缺少 httplib.h，请先运行 scripts/bootstrap.sh")
endif()
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE "${THIRD_PARTY_DIR}")
# httplib v0.12.6 使用 #ifdef 判断，不定义这些宏 = 禁用对应功能
# 不能定义为 0，否则 #ifdef 仍为 true 会尝试引入 zlib/openssl/brotli

# ── spdlog ────────────────────────────────────────────────────────────────────
if(NOT EXISTS "${THIRD_PARTY_DIR}/spdlog/include/spdlog/spdlog.h")
    message(FATAL_ERROR "缺少 spdlog，请先运行 scripts/bootstrap.sh")
endif()
add_library(spdlog_iface INTERFACE)
target_include_directories(spdlog_iface INTERFACE
    "${THIRD_PARTY_DIR}/spdlog/include")

# ── libmodbus ─────────────────────────────────────────────────────────────────
set(MODBUS_INST "${THIRD_PARTY_DIR}/install/libmodbus")
if(NOT EXISTS "${MODBUS_INST}/lib/libmodbus.a")
    message(FATAL_ERROR "缺少 libmodbus，请先运行 scripts/bootstrap.sh")
endif()
add_library(modbus_static STATIC IMPORTED)
set_target_properties(modbus_static PROPERTIES
    IMPORTED_LOCATION             "${MODBUS_INST}/lib/libmodbus.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MODBUS_INST}/include")

# ── paho.mqtt.c ───────────────────────────────────────────────────────────────
set(PAHOC_INST "${THIRD_PARTY_DIR}/install/paho-c")
if(NOT EXISTS "${PAHOC_INST}/lib/libpaho-mqtt3a.a")
    message(FATAL_ERROR "缺少 paho.mqtt.c，请先运行 scripts/bootstrap.sh")
endif()
add_library(paho_c_static STATIC IMPORTED)
set_target_properties(paho_c_static PROPERTIES
    IMPORTED_LOCATION             "${PAHOC_INST}/lib/libpaho-mqtt3a.a"
    INTERFACE_INCLUDE_DIRECTORIES "${PAHOC_INST}/include")

# ── paho.mqtt.cpp ─────────────────────────────────────────────────────────────
set(PAHOPP_INST "${THIRD_PARTY_DIR}/install/paho-cpp")
if(NOT EXISTS "${PAHOPP_INST}/lib/libpaho-mqttpp3.a")
    message(FATAL_ERROR "缺少 paho.mqtt.cpp，请先运行 scripts/bootstrap.sh")
endif()
add_library(paho_cpp_static STATIC IMPORTED)
set_target_properties(paho_cpp_static PROPERTIES
    IMPORTED_LOCATION             "${PAHOPP_INST}/lib/libpaho-mqttpp3.a"
    INTERFACE_INCLUDE_DIRECTORIES "${PAHOPP_INST}/include")

# ── libiec61850 ───────────────────────────────────────────────────────────────
set(IEC61850_INST "${THIRD_PARTY_DIR}/install/libiec61850")
if(NOT EXISTS "${IEC61850_INST}/lib/libiec61850.a")
    message(FATAL_ERROR "缺少 libiec61850，请先运行 scripts/bootstrap.sh")
endif()
add_library(iec61850_static STATIC IMPORTED)
set_target_properties(iec61850_static PROPERTIES
    IMPORTED_LOCATION             "${IEC61850_INST}/lib/libiec61850.a"
    INTERFACE_INCLUDE_DIRECTORIES "${IEC61850_INST}/include")

# libiec61850 HAL（Linux 平台实现：Memory/Semaphore/Socket/Hal_getTimeInMs）
add_library(hal_static STATIC IMPORTED)
set_target_properties(hal_static PROPERTIES
    IMPORTED_LOCATION "${IEC61850_INST}/lib/libhal.a")

# ── open62541（OPC UA）───────────────────────────────────────────────────────
set(UA_INST "${THIRD_PARTY_DIR}/install/open62541")
if(NOT EXISTS "${UA_INST}/lib/libopen62541.a")
    message(FATAL_ERROR "缺少 open62541，请先运行 scripts/bootstrap.sh")
endif()
add_library(open62541_static STATIC IMPORTED)
set_target_properties(open62541_static PROPERTIES
    IMPORTED_LOCATION             "${UA_INST}/lib/libopen62541.a"
    INTERFACE_INCLUDE_DIRECTORIES "${UA_INST}/include")

# ── SQLite（本地缓存 / 断点续传）──────────────────────────────────────────────
set(SQLITE_INST "${THIRD_PARTY_DIR}/install/sqlite3")
if(NOT EXISTS "${SQLITE_INST}/lib/libsqlite3.a")
    message(FATAL_ERROR "缺少 sqlite3，请先运行 scripts/bootstrap.sh")
endif()
add_library(sqlite3_static STATIC IMPORTED)
set_target_properties(sqlite3_static PROPERTIES
    IMPORTED_LOCATION             "${SQLITE_INST}/lib/libsqlite3.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SQLITE_INST}/include")

# ── OpenSSL 1.1.1（Web HTTPS）────────────────────────────────────────────────
set(SSL_INST "${THIRD_PARTY_DIR}/install/openssl")
if(NOT EXISTS "${SSL_INST}/lib/libssl.a")
    message(FATAL_ERROR "缺少 openssl，请先运行 scripts/bootstrap.sh")
endif()
add_library(ssl_static STATIC IMPORTED)
set_target_properties(ssl_static PROPERTIES
    IMPORTED_LOCATION             "${SSL_INST}/lib/libssl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SSL_INST}/include")
add_library(crypto_static STATIC IMPORTED)
set_target_properties(crypto_static PROPERTIES
    IMPORTED_LOCATION             "${SSL_INST}/lib/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SSL_INST}/include")

# ── Web 资源：编译时将 web/index.html 打包进 web_assets_gen.h ─────────────────
set(WEB_DIR       "${CMAKE_SOURCE_DIR}/web")
set(WEB_ASSETS_H  "${CMAKE_BINARY_DIR}/web_assets_gen.h")
find_program(PYTHON3_EXEC python3)
if(NOT PYTHON3_EXEC)
    message(FATAL_ERROR "未找到 python3，无法生成 web_assets_gen.h")
endif()
add_custom_command(
    OUTPUT  "${WEB_ASSETS_H}"
    COMMAND "${PYTHON3_EXEC}"
            "${CMAKE_SOURCE_DIR}/scripts/gen_web.py"
            "${WEB_DIR}"
            "${WEB_ASSETS_H}"
    DEPENDS "${WEB_DIR}/index.html"
            "${WEB_DIR}/assets/style.css"
            "${WEB_DIR}/assets/app.js"
            "${WEB_DIR}/assets/config.js"
            "${WEB_DIR}/assets/model.js"
            "${CMAKE_SOURCE_DIR}/scripts/gen_web.py"
    COMMENT "打包 web/ 静态资源 → web_assets_gen.h"
    VERBATIM
)
add_custom_target(web_assets DEPENDS "${WEB_ASSETS_H}")

# ── 主可执行文件 ──────────────────────────────────────────────────────────────
add_executable(industrial_collector
    src/main.cpp
    src/config.cpp
    src/mqtt_publisher.cpp
    src/channel.cpp
    src/modbus_collector.cpp
    src/iec104_collector.cpp
    src/iec61850_collector.cpp
    src/opcua_collector.cpp
    src/io_transport.cpp
    src/dlt645_collector.cpp
    src/dlt698_collector.cpp
    src/can_collector.cpp
    src/data_cache.cpp
    src/web_server.cpp
    src/config_db.cpp
    src/config_migrate.cpp
    src/config_csv.cpp
    src/config_compile.cpp
    src/config_runtime.cpp
    src/formula.cpp
    src/web_api_model.cpp
    src/compat.c
)
add_dependencies(industrial_collector web_assets)

target_include_directories(industrial_collector PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}          # 使 web_assets_gen.h 可被 include
)

target_link_libraries(industrial_collector PRIVATE
    paho_cpp_static
    paho_c_static
    modbus_static
    iec61850_static
    hal_static          # libiec61850 HAL：Memory/Semaphore/Socket
    open62541_static
    sqlite3_static
    nlohmann_json
    httplib
    spdlog_iface
    ssl_static          # HTTPS：必须 ssl 在 crypto 之前、二者在 dl 之前
    crypto_static
    pthread
    dl
    m
)

target_compile_options(industrial_collector PRIVATE
    -Wall -Wextra -O2
    -Wno-unused-parameter    # open62541 头文件有大量未用参数
)

# debug 模式默认前端目录（源码 web/ 绝对路径），供 --debug / web.debug 实时伺服
# CPPHTTPLIB_OPENSSL_SUPPORT=1 开启 httplib 的 HTTPS（链接静态 OpenSSL）
target_compile_definitions(industrial_collector PRIVATE
    IC_WEB_SOURCE_DIR="${CMAKE_SOURCE_DIR}/web"
    CPPHTTPLIB_OPENSSL_SUPPORT=1
    # httplib 把 application/x-www-form-urlencoded 的请求体卡在 8KB。
    # curl --data-binary 默认就发这个 Content-Type，于是设备树 CSV 一超过 8KB
    # （一个储能站的 CSV 有 330KB）就被 413 挡下，而错误响应体是空的，
    # 排查起来毫无线索。本项目没有表单编码接口，该上限只会误伤 CSV 导入，
    # 故放宽到 64MB —— 仍是有界的，不是取消限制。
    CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH=67108864)

## httplib 禁用宏不在此处定义（见 httplib INTERFACE target 注释）

install(TARGETS industrial_collector DESTINATION bin)
install(FILES config/config.json DESTINATION etc/industrial_collector)

# ── DLT 帧字节级单元测试（不随默认构建；make dlt_frame_test 单独编译）──────────
# 规约符合性测试，无需 MQTT/web 依赖，验证 +0x33 / BCD字节序 / 校验和 / CRC。
add_executable(dlt_frame_test EXCLUDE_FROM_ALL
    test/dlt_frame_test.cpp
    src/dlt645_collector.cpp
    src/dlt698_collector.cpp
    src/io_transport.cpp
)
target_include_directories(dlt_frame_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dlt_frame_test PRIVATE spdlog_iface pthread)
target_compile_options(dlt_frame_test PRIVATE -Wall -Wextra)

# ── CAN 信号位域解码测试（不随默认构建；make can_signal_test）─────────────────
# Intel/Motorola 位序与符号扩展是 CAN 解码最易出错处，用手工推导的期望值验证，
# 无需真实 CAN 接口。
add_executable(can_signal_test EXCLUDE_FROM_ALL
    test/can_signal_test.cpp
    src/can_collector.cpp
)
target_include_directories(can_signal_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(can_signal_test PRIVATE spdlog_iface pthread)
target_compile_options(can_signal_test PRIVATE -Wall -Wextra)

# ── 传输层超时语义测试（不随默认构建；make io_timeout_test）───────────────────
# 内含旧实现作对照组，验证"整体截止时间"而非"每段重置"。串口路径用 openpty 模拟，
# 故需链接 libutil；不需要真实串口或网关。
add_executable(io_timeout_test EXCLUDE_FROM_ALL
    test/io_timeout_test.cpp
    src/io_transport.cpp
)
target_include_directories(io_timeout_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(io_timeout_test PRIVATE pthread util)
target_compile_options(io_timeout_test PRIVATE -Wall -Wextra)

# ── DLT 事务整体超时测试（不随默认构建；make dlt_deadline_test）───────────────
# 用 TCP 回环模拟噪声线路，验证 transact() 受单一 deadline 约束并给出超时提示。
add_executable(dlt_deadline_test EXCLUDE_FROM_ALL
    test/dlt_deadline_test.cpp
    src/dlt645_collector.cpp
    src/io_transport.cpp
)
target_include_directories(dlt_deadline_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(dlt_deadline_test PRIVATE spdlog_iface pthread)
target_compile_options(dlt_deadline_test PRIVATE -Wall -Wextra)

# ── 本地缓存测试（不随默认构建；make data_cache_test）─────────────────────────
# 定时兜底刷盘需要真实等待 >BATCH_FLUSH_MS，故用临时 db 文件，约 3 秒。
add_executable(data_cache_test EXCLUDE_FROM_ALL
    test/data_cache_test.cpp
    src/data_cache.cpp
)
target_include_directories(data_cache_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(data_cache_test PRIVATE sqlite3_static spdlog_iface pthread dl)
target_compile_options(data_cache_test PRIVATE -Wall -Wextra)

# ── 配置库 schema + 迁移器测试（不随默认构建；make config_db_test）────────────
# 核心是语义往返：真实 config.json → 迁移入库 → 导出 → 逐字段比对。
# 迁移器的典型失效是静默丢字段，"插入了几行"证明不了正确性。
add_executable(config_db_test EXCLUDE_FROM_ALL
    test/config_db_test.cpp
    src/config_db.cpp
    src/config_migrate.cpp
    src/config_csv.cpp
    src/config_compile.cpp
    src/config_runtime.cpp   # Excel 新增点位的判据是"快照能被运行时解析器吃下"
    src/formula.cpp
    src/config.cpp
)
target_include_directories(config_db_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(config_db_test PRIVATE
    sqlite3_static nlohmann_json spdlog_iface pthread dl)
target_compile_options(config_db_test PRIVATE -Wall -Wextra)

# ── 编译快照 → 运行时设备表 测试（不随默认构建；make runtime_test）───────────
# P2 里风险最高的一步：还原错了采集器会读错地址且不崩溃。判据是逐条核对地址。
add_executable(runtime_test EXCLUDE_FROM_ALL
    test/runtime_test.cpp
    src/config_db.cpp
    src/config_migrate.cpp
    src/config_compile.cpp
    src/config_runtime.cpp
    src/formula.cpp
    src/config.cpp
)
target_include_directories(runtime_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(runtime_test PRIVATE
    sqlite3_static nlohmann_json spdlog_iface pthread dl)
target_compile_options(runtime_test PRIVATE -Wall -Wextra)

# ── 编译器（「生效」）测试（不随默认构建；make compile_test）─────────────────
# 判据是每个实例算出的地址都对，不是"编译成功"——地址算错不崩溃，只会静默读到
# 别的寄存器。用缩小版储能站（2簇×2包×3芯 + 控制器）逐个核对手工推导的地址。
add_executable(compile_test EXCLUDE_FROM_ALL
    test/compile_test.cpp
    src/config_db.cpp
    src/config_compile.cpp
    src/formula.cpp
)
target_include_directories(compile_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(compile_test PRIVATE
    sqlite3_static nlohmann_json spdlog_iface pthread dl)
target_compile_options(compile_test PRIVATE -Wall -Wextra)

# ── 地址公式求值器测试（不随默认构建；make formula_test）─────────────────────
# 纯计算、无依赖。地址算错不会崩溃，只会静默读到别的寄存器，故边界与错误
# 诊断同样要测。
add_executable(formula_test EXCLUDE_FROM_ALL
    test/formula_test.cpp
    src/formula.cpp
)
target_include_directories(formula_test PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_options(formula_test PRIVATE -Wall -Wextra)

message(STATUS "执行 make -j\$(nproc) 开始编译")

"""
并发启动全部协议模拟器（容器入口）。
任一模拟器崩溃只打印告警，不影响其他模拟器。
"""
import threading
import traceback


def guard(name, fn):
    try:
        fn()
    except Exception:
        print(f"[{name}] 模拟器异常退出：", flush=True)
        traceback.print_exc()


def main():
    import modbus_sim
    import opcua_sim
    import dlt_sim
    import ess_modbus_sim

    targets = [
        ("modbus", modbus_sim.run),
        ("opcua",  opcua_sim.run),
        ("dlt",    dlt_sim.run),
        ("ess",    ess_modbus_sim.run),   # 储能站大规模点表（P4 层级聚合的数据源）
    ]

    # IEC104 依赖 c104，单独 try，缺失不致命
    try:
        import iec104_sim
        targets.append(("iec104", iec104_sim.run))
    except Exception:
        print("[iec104] c104 不可用，跳过 IEC104 模拟器", flush=True)

    threads = []
    for name, fn in targets:
        t = threading.Thread(target=guard, args=(name, fn), daemon=True)
        t.start()
        threads.append(t)

    print(f"[run_all] 已启动 {len(threads)} 个模拟器", flush=True)
    for t in threads:
        t.join()


if __name__ == "__main__":
    main()

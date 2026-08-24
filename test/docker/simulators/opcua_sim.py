"""
OPC UA server 模拟器（asyncua）
端点 opc.tcp://0.0.0.0:4840

节点（与 config.docker.json 的 opcua 点表对应）：
  ns=2;s=Temperature  -> 25.5  (Double)
  ns=2;s=Pressure     -> 1.23  (Double)
  ns=2;s=MotorSpeed   -> 1450  (Int32)
"""
import asyncio
from asyncua import Server, ua


async def main():
    server = Server()
    await server.init()
    # 绑定 15643：与对外发布端口一致，避免 OPC UA GetEndpoints 返回错误端口
    server.set_endpoint("opc.tcp://0.0.0.0:15643/freeopcua/server/")
    server.set_server_name("IC Test OPC UA Server")
    # 关闭安全策略，匹配采集器 security_policy=None
    server.set_security_policy([ua.SecurityPolicyType.NoSecurity])

    idx = await server.register_namespace("http://ic.test/sim")  # 期望返回 2
    objs = server.nodes.objects

    temp = await objs.add_variable(ua.NodeId("Temperature", idx), "Temperature", 25.5)
    pres = await objs.add_variable(ua.NodeId("Pressure", idx), "Pressure", 1.23)
    speed = await objs.add_variable(
        ua.NodeId("MotorSpeed", idx), "MotorSpeed",
        ua.Variant(1450, ua.VariantType.Int32))

    print(f"[opcua] server 启动 :15643  namespace idx={idx}", flush=True)

    async with server:
        # 让数值缓慢变化，便于看趋势图
        t = 0.0
        while True:
            await asyncio.sleep(2)
            t += 0.1
            await temp.write_value(25.5 + (t % 5))
            await pres.write_value(1.23 + (t % 3) * 0.01)


def run():
    asyncio.run(main())


if __name__ == "__main__":
    run()

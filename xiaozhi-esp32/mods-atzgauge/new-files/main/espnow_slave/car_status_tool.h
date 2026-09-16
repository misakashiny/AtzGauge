// car_status_tool.h -- 注册 MCP 工具 self.car.get_status
//
// 纯 C 接口包装，便于板级 .cc 调用；实现是 C++（MCP 服务器是 C++ 接口）。
// 板级在自己的 InitializeTools() 里调用一次即可。

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void car_status_tool_register(void);

#ifdef __cplusplus
}
#endif

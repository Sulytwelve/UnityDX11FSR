#pragma once
#include <cstdint>

namespace lillia {

void demosaic_start();
void demosaic_stop();
void demosaic_notify_rendering();   // Present hook 调用: 渲染已开始(元数据已初始化)
int  demosaic_status();            // 0=等待 1=已挂 2=未找到 -1=错误
int  demosaic_hits();              // hook 命中次数
int  demosaic_renderers_disabled(); // 累计 set_enabled(false) 调用数
int  demosaic_dc_count();           // 最近一次扫描到的 DressController 实例数

}

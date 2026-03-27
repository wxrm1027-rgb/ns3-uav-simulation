#ifndef HETEROGENEOUS_NMS_FRAMEWORK_H
#define HETEROGENEOUS_NMS_FRAMEWORK_H

/**
 * 模块化迁移入口：
 * - HnmsMain() 复用原 heterogeneous-nms-framework.cc 的完整仿真逻辑
 * - 后续可逐步将实现从 legacy 文件迁移到各子模块中
 */
int HnmsMain (int argc, char *argv[]);

#endif // HETEROGENEOUS_NMS_FRAMEWORK_H


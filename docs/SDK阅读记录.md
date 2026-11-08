# KtB2900 CHM 和代码阅读记录

## 2026-10-08 保护接口核对

关断顺序已调整为：只发送CH2 OFF、CH1 OFF，再查询两路关闭状态。正常停止、异常保护和断开均不写0 mA、不修改最后的电流设定；下一次启动仍按启动流程重新配置。任一路关断或查询失败都不阻止另一路关断。保护测试加入关断顺序、两路分别写入失败/读取失败、输出仍开启的故障注入，并检查关断/断开不追加任何设流命令、正负设定值保持不变。

| CHM/接口 | 本次用途 |
| --- | --- |
| `KTB2900_ATTR_OUTPUT_PROTECTION_ENABLED` | 开启仪器到达合规时自动关断，启动前读回核对 |
| `KTB2900_ATTR_OUTPUT_AUTO_ON_ENABLED` | 显式关闭，避免保护后由测量触发再次开启 |
| `KTB2900_ATTR_OUTPUT_AUTO_OFF_ENABLED` | 显式关闭，保持正常连续电流输出 |
| `KtB2900_StatusQuestionableReadConditionRegister` | 读取输出保护标志，bit2为CH1、bit3为CH2；使用C/CL SCPI手册279页的定义（CHM该函数的枚举列表不完整） |
| `KtB2900_SystemLockName/SystemLockOwner` | 读取当前接口和锁持有者 |
| `KtB2900_SystemRequestLock/SystemReleaseLock` | 申请、成对释放仪器接口锁，已有持有者时拒绝接管 |

接口锁范围是I/O接口，**不能代替同一接口多个程序之间的协调**。本工程另外增加本机固定内核互斥对象和按序列号的设备锁；旧程序、Interactive IO等未使用这些锁的软件仍需退出控制会话。接口锁会锁定仪器面板（包括Local键），断开时释放。`KtB2900_LockSession`仅为驱动多线程锁，没有用于跨程序互斥。[官方SCPI参考，220、279、304–306页](https://www.keysight.com.cn/cn/zh/assets/9925-01426/programming-guides/B2900C-CL-Series-Precision-Source-Measure-Unit-SCPI-Command-Reference.pdf)

软件保护入口在 `src/protection.cpp`，控制线程在实测电压后、下一次设流前检查±10 V；取得CH2实测电流后检查过流及持续偏差。检测阈值、容差、恢复流程见README“运行保护”。单位继续为界面mA、SDK A，1 mA按0.001 A下发。

新增离线保护测试含控制线程故障注入、本机子进程互斥和IVI仿真属性读回；没有进行本次版本的实机到限输出测试。仿真驱动不产生物理对应的测量值，不能用其随机读数验收真实电流或反馈保护。

阅读日期：2026-09-16。对应本机 IVI-C 1.1.0，面向 B2912C、Qt/C++ 和方案3。

## 本机资料位置

IVI 根目录：`C:/Program Files/IVI Foundation/IVI`。

- `Drivers/KtB2900/KtB2900.chm`：API 帮助、初始化、模拟、错误处理。
- `Include/KtB2900.h`：C 函数声明、属性和枚举。
- `Include/IviVisaType.h`：ViSession、ViStatus、ViReal64 和调用约定。
- `Drivers/KtB2900/Examples/Cpp/CppExample1/Program.cpp`：连接、属性、测量、关闭示例。
- `Drivers/KtB2900/Examples/Cpp/CppStatusSRQ/Program.cpp`：状态轮询与异步 SRQ 示例。
- `Drivers/KtB2900/Source/RCL/KtB2900/KtB2900MeasurementCollection.h` 等：厂商 C++ API 声明与注释；这些头文件不等于完整驱动实现源码。
- `Bin/KtB2900_64.dll`：本工程实际加载的 64 位 C API。
- `Lib_x64/msc/KtB2900.lib`：MSVC 可用的导入库；本工程的 MinGW 构建使用 QLibrary 加载 DLL。

CHM 已用 Windows hh.exe 解开并阅读相关 HTML 主题，同时和安装目录中的头文件及示例核对。没有把整份驱动源码或 CHM 重新打包。

## 已核对的关键主题

| CHM 主题或 HTML 文件 | 核对结论 | 工程落实 |
| --- | --- | --- |
| `IviCUsingSimulation.html` | Simulate=true 不执行仪器 I/O；可指定 DriverSetup=Model=B2912C；模拟不能证明真实性能和所有参数合法性 | SDK/GUI自动测试固定驱动仿真；软件对象模型仅用于内部回归，已从界面移除 |
| `KtB2900_InitWithOptions.html` | IdQuery 控制型号验证；Reset 控制复位；DriverSetup 必须放选项末尾 | 连接Reset=false；运行时先保存CH2上限再复位；真机只接受 B2912C |
| `KtB2900_GetOutputChannelName.html`、`KtB2900_GetMeasurementName.html` | 查询物理重复能力名称 | 实测模拟返回 OutputChannel1/2、Measurement1/2 |
| `KtB2900_MeasurementReadScalarData.html` | READ 内部执行INIT和FETCH，INIT包含输出触发，可能应用另一个触发电流值 | 持续恒流模式不使用此接口 |
| `KtB2900_MeasurementInitiate.html`、`KtB2900_SystemWaitForOperationComplete.html`、`KtB2900_MeasurementFetchScalarData.html` | 分别对应INIT:ACQ、*OPC?、FETCH；只启动测量并等待完成，再提取新数据 | 单通道 double[1]、长度1，检查返回长度及有限值；采样不触发输出动作 |
| `KtB2900_SetOutputType.html` | 当前专用枚举 CURRENT=0、VOLTAGE=1 | 使用专用 OUTPUT_TYPE_CURRENT；不混用 IVI 通用类枚举 |
| `KTB2900_ATTR_MEASUREMENT_VOLTAGE_RANGE.html` | 设置预期测量值并选择可测该值的量程 | 设置20 V，覆盖±10 V输入 |
| `KtB2900_SetMeasurementVoltageComplianceValue.html`、`KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_VALUE.html` | 设置/读取对称的电压合规值；手册默认2 V、范围-210～210 V；POS/NEG独立属性的支持型号列表不含B2912C | CH1=12 V；CH2默认读取原值、复位后恢复并核对；高级设置可显式覆盖0.1～20 V |
| `KTB2900_ATTR_MEASUREMENT_VOLTAGE_COMPLIANCE_STATUS_ENABLED.html` | 只读，查询是否触及合规上限 | 每轮测量后查询并在触及时停机 |
| `KtB2900_MeasurementFunctionSetEnabled.html` | 枚举类型是 MeasurementType，电压1、电流0 | CH1启用电压；CH2分两次调用启用电压、电流，避免1.1.0驱动多元素调用被实机拒绝 |
| `KtB2900_MeasurementFunctionGetEnabled.html` | 返回已启用的测量功能枚举数组及实际长度 | 配置诊断读回确认CH2同时包含电压和电流 |
| `KtB2900_error_query.html` | 查询仪器错误队列，适用于 QueryInstrStatus=true 的仪器错误 | GetError只提供驱动通用错误；补查error_query记录仪器错误码和描述 |
| `KtB2900_SetMeasurementTriggerSource.html` | 支持 AINT、BUS 等触发源 | 测量臂和触发源AINT，次数1 |
| `KTB2900_ATTR_OUTPUT_OFF_CONDITION.html` | HIZ/NORMAL/ZERO 是关断后的源条件 | HIZ关断；CH1测量时仍使能源0 A |
| `KTB2900_ATTR_SYSTEM_IO_TIMEOUT.html` | 毫秒单位，全局属性的重复能力传空字符串 | 初始化成功后设置2000 ms |
| `KtB2900_GetError.html` | 负数错误、正数警告；错误读取后清除；VI_NULL可取当前线程错误 | 错误抛出、警告记录、清理逐项执行 |

通道列表注意：CHM 参数文字写“1”“2”，官方 C++ 示例使用“(@1)”。本工程沿用官方示例的 `(@1)` / `(@2)`，已通过 SDK 仿真；真实仪器解析行为仍需实机测试。

## 本项目的最小接口顺序

1. `KtB2900_InitWithOptions`：连接时资源地址、IdQuery=true、Reset=false，只读身份并保持会话；用户启动控制时才调用reset。仿真选项包含 `Cache=false, Simulate=true, QueryInstrStatus=true, DriverSetup=Model=B2912C`。
2. 查询型号及两个输出/测量通道名称、CH2电压上限；全部操作在一个工作线程中执行。真实连接不设置输出参数。仅IVI仿真新会话初值为0 V时，初始化模拟CH2上限为2 V。
3. 启动时在复位前读取CH2上限（未勾选覆盖时），校验后复位。`KtB2900_SetAttributeViInt32/ViReal64/ViBoolean`：输出关闭、高阻关断模式、恒流模式、初始0 A、量程、恢复/覆盖合规、NPLC、AINT及单次采样。CH2上限读回与目标值不一致则不使能；保留的上限大于20 V时CH2电压测量采用自动量程。
4. CH1以0 A源模式测电压，CH2输出电流；使能两路输出。
5. 循环读取CH1新电压V → 与目标电压V比较 → PID求最终mA → 除1000写CH2安培值 → 读取CH2电流及合规状态。
6. 停止时尝试CH2设0 A、CH2/CH1关闭及状态查询，成功后保持会话；出错或断开连接时再调用 `KtB2900_close`。任何清理步骤失败都继续尝试后续步骤。

项目没有把串口、SCPI解析、SRQ异步回调、波形扫描等功能加入位置闭环。官方 SRQ 示例本身提醒异步回调中的 I/O 可能干扰主线程；这里用单工作线程串行访问简化控制。

## 对官方示例和原 PID 的处理

官方 CppExample1 使用 `Val[2]`，读取时却传1024，缓冲区长度不一致；工程使用一致的长度并检查实际返回数。示例 `str == "B2901A"` 等比较字符指针；工程用 QString 比较型号。示例用非零状态判断失败；工程区分警告与错误。

`F:/PIDCtrl/PIDFunction.cpp/.h` 原样复制并保留作者信息。这里只调用 PID_Init/PID_Compute；原滤波器、抖动和旧界面不启用。原算法的手动分支和参数切换分支不统一限幅，因此本工程在 VoltagePid 外层限制输入参数和最终电流，并在运行中锁定参数。手动电流由外层直接验证和输出，不调用原手动分支。

2026-10-08起，目标与反馈统一使用V，Kp使用mA/V，PID输出mA。删除位置标定a/b及其中间位置变量。CH1范围检查仍使用实际电压，CH2限幅仍使用电流。文档V1.0中的旧位置换算说明以当前README和代码为准。

原算法使用反馈微分；首轮把 feedback_last 初始化为当前反馈，避免初始非零位置造成微分突跳。每轮用实际 dt，Gain保持非负，方向系数在最终输出端乘±1。到目标后不清积分。

## 验证边界

界面默认真实仪器，移除了软件仿真入口；两种可见连接方式均需先连接成功再启动。GUI回归显式按模式数据选择IVI仿真，模式不存在则失败退出，不会自动连接真机。新增12 Ω理想电阻模型验证用于README测试参数的计算核对，不代表已做真实闭环实验。用户反馈此前已正确输出控制电流，计划接入电阻电压完成CH1反馈实验。

`tests/results` 保存本机测试结果；Release 编译通过，十组测试通过。驱动仿真验证DLL、函数签名和调用链能运行，不验证真实仪器指令合法性、设备时延和位置精度。原技术交流材料只出现±10 V及“精度0.1mv”，尚无清晰的位置误差、响应时间和电压精度验收定义。

2026-10-07：在真实 B2912C（序列号 MY65150137、固件6.0.516.0、KtB2900 1.1.0）关闭输出时复现了多元素 `MeasurementFunctionSetEnabled` 返回仪器错误 `-151 Invalid string data`。分两次调用后，`MeasurementFunctionGetEnabled` 实际返回CH2的 `[1,0]`（电压、电流），CH1在完整模式返回 `[1]`。本驱动的 `MeasurementFunctionGetState` 在同一配置下返回false，与功能列表结果不一致，因此诊断使用列表读回。两种模式配置均通过，最终CH1/CH2输出OFF，CH2设定0 A；没有进行非零电流输出测试。

同日用户照片显示CH2 Source为1.00000 mA，但测量约0.00001 mA、电压约-0.000127 V。代码确认界面1 mA除1000后下发0.001 A，读回A乘1000显示mA，单位一致。继续核对发现原READ采样会同时启动输出触发，与持续直流控制的意图不符，已改为INIT:ACQ → *OPC? → FETCH。官方[SCPI参考第98页](https://www.keysight.com/us/en/assets/9925-01426/programming-guides/B2900C-CL-Series-Precision-Source-Measure-Unit-SCPI-Command-Reference.pdf)列出READ的INIT/FETCH行为；[编程指南第34页](https://www.keysight.com/zz/en/assets/9925-01427/programming-guides/B2900C-CL-Series-Precision-Source-Measure-Unit-Programming-Guide.pdf)说明输出触发会应用CURR:TRIG值。

新调用链已通过十组仿真测试，增加±0.001 A及0 A下的采样与设定值保留检查。此次未干预正在运行的实机输出，尚未验证负载实测电流；照片中低读数的最终原因需用修复版及万用表复测确认。

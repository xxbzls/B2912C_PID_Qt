/******************************************************************************

  Copyright (C), 2017-2027, xiubinxiao@163.com, Ltd.

 ******************************************************************************
  File Name     : PIDFunction.h
  Version       : 1.0
  Author        : xiubin
  Created       : 2025-06-10
  Last Modified :
  Description   :  header file
  Function List :   PID控制函数
  History       :
  1.Date        : 2025-06-10
    Author      : xiubin
    Modification: Created file

******************************************************************************/
#ifndef PIDFUNCTION_H
#define PIDFUNCTION_H

#ifdef __cplusplus
extern "C" {
#endif

// =================== PID结构体 ===================
typedef struct
{
    // PID参数
    float Gain;      // 比例系数（Kp）
    float Ti;        // 积分时间常数
    float Td;        // 微分时间常数
    float dt;        // 采样周期（秒）

    // 状态变量
    float setpoint;        // 设定值
    float feedback;        // 当前反馈
    float error;           // 当前误差
    float error_last;      // 上一次误差
    float I_value;         // 积分项
    float D_value;         // 微分项
    float output;          // 当前输出
    float output_last;     // 上次输出
    float feedback_last;   // 上次反馈
    float P_value;         // 比例项

    // 输出限幅
    float out_min;
    float out_max;

    // 手动/自动切换
    int manual_mode;       // 1=手动
    float manual_output;   // 手动输出值
    int reset_flag;        // 1=复位

    // 参数变化检测
    float Gain_last;
    float Ti_last;
    float Td_last;
    int manual_last;
} PID_TypeDef;

// ========== PID核心函数 ==========
void PID_Init(PID_TypeDef *pid, float Gain, float Ti, float Td, float dt, float out_min, float out_max);
float PID_Compute(PID_TypeDef *pid, float setpoint, float feedback);
void PID_SetManualMode(PID_TypeDef *pid, int manual_mode, float manual_output);

// ================== 五点滑动平均滤波结构体及函数 ==================
typedef struct {
    float window[5];      // 滑动窗口
    int count;            // 窗口计数
    float weights[5];     // 加权系数（和为1）
    float last_out;       // 上一次滤波输出
    float threshold;      // 异常检测阈值
    int abnormal_count;   // 连续异常计数
    int abnormal_limit;   // 异常次数阈值
} Filter5Point_TypeDef;

void Filter5Point_Init(Filter5Point_TypeDef* filter, const float weights[5], float threshold, int abnormal_limit);
float Filter5Point_Update(Filter5Point_TypeDef* filter, float in);

#ifdef __cplusplus
}
#endif

#endif

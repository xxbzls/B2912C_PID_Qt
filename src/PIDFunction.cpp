#include "PIDFunction.h"
#include <string.h>
#include <math.h>

// ========== PID核心实现 ==========
void PID_Init(PID_TypeDef *pid, float Gain, float Ti, float Td, float dt, float out_min, float out_max)
{
    pid->Gain = Gain;
    pid->Ti = Ti;
    pid->Td = Td;
    pid->dt = dt;
    pid->out_min = out_min;
    pid->out_max = out_max;

    pid->setpoint = 0;
    pid->feedback = 0;
    pid->error = 0;
    pid->error_last = 0;
    pid->I_value = 0;
    pid->D_value = 0;
    pid->output = 0;
    pid->output_last = 0;
    pid->feedback_last = 0;
    pid->P_value = 0;

    pid->manual_mode = 0;
    pid->manual_output = 0;
    pid->reset_flag = 0;

    pid->Gain_last = Gain;
    pid->Ti_last = Ti;
    pid->Td_last = Td;
    pid->manual_last = 0;
}

float PID_Compute(PID_TypeDef *pid, float setpoint, float feedback)
{
    pid->setpoint = setpoint;
    pid->feedback = feedback;

    int Para_Change = 0;
    if (pid->Gain != pid->Gain_last || pid->Ti != pid->Ti_last ||
        pid->Td != pid->Td_last || pid->manual_mode != pid->manual_last)
        Para_Change = 1;

    // 复位处理，直接归零返回
    if (pid->reset_flag)
    {
        pid->I_value = 0.0f;
        pid->D_value = 0.0f;
        pid->output = 0.0f;
        pid->output_last = 0.0f;
        pid->error_last = 0.0f;
        pid->feedback_last = 0.0f;
        pid->reset_flag = 0;
        pid->Gain_last = pid->Gain;
        pid->Ti_last = pid->Ti;
        pid->Td_last = pid->Td;
        pid->manual_last = pid->manual_mode;
        return 0.0f;
    }

    // 手动模式
    if (pid->manual_mode)
    {
        pid->output = pid->manual_output;
        pid->output_last = pid->output;
        pid->Gain_last = pid->Gain;
        pid->Ti_last = pid->Ti;
        pid->Td_last = pid->Td;
        pid->manual_last = pid->manual_mode;
        return pid->output;
    }

    pid->error = pid->setpoint - pid->feedback;
    pid->P_value = pid->Gain * pid->error;

    // 微分项（反馈分离型）
    if (pid->Td > 0)
        pid->D_value = pid->Gain * pid->Td * (pid->feedback_last - pid->feedback) / pid->dt;
    else
        pid->D_value = 0.0f;

    // 积分部分及抗积分饱和处理
    if (pid->Ti != 0)
    {
        float I_temp1 = (pid->error + pid->error_last) * 0.5f * pid->Gain / pid->Ti * pid->dt + pid->I_value;

        if (Para_Change == 0)
        {
            float I_temp2 = I_temp1;
            float PI_temp = pid->P_value + I_temp2;

            // 抗积分饱和（Windup）
            if (((PI_temp > pid->out_max) && (pid->error > 0)) ||
                ((PI_temp < pid->out_min) && (pid->error < 0)))
            {
                // 积分冻结
                I_temp2 = pid->I_value;
                PI_temp = pid->P_value + I_temp2;
            }

            if (PI_temp >= pid->out_min && PI_temp <= pid->out_max)
                pid->I_value = I_temp2;
            else if (PI_temp < pid->out_min)
                pid->I_value = pid->out_min - pid->P_value;
            else
                pid->I_value = pid->out_max - pid->P_value;

            float pid_raw = pid->P_value + pid->I_value + pid->D_value;
            if (pid_raw < pid->out_min)
                pid->output = pid->out_min;
            else if (pid_raw > pid->out_max)
                pid->output = pid->out_max;
            else
                pid->output = pid_raw;
        }
        else
        {
            // 参数切换时，积分反解，输出保持不变
            pid->I_value = pid->output_last - pid->P_value - pid->D_value;
            pid->output = pid->output_last;
        }
    }
    else
    {
        pid->I_value = 0.0f;
        float pid_raw = pid->P_value + pid->D_value;
        if (pid_raw < pid->out_min)
            pid->output = pid->out_min;
        else if (pid_raw > pid->out_max)
            pid->output = pid->out_max;
        else
            pid->output = pid_raw;
    }

    // 更新历史量
    pid->error_last = pid->error;
    pid->feedback_last = pid->feedback;
    pid->output_last = pid->output;
    pid->Gain_last = pid->Gain;
    pid->Ti_last = pid->Ti;
    pid->Td_last = pid->Td;
    pid->manual_last = pid->manual_mode;

    return pid->output;
}

void PID_SetManualMode(PID_TypeDef *pid, int manual_mode, float manual_output)
{
    pid->manual_mode = manual_mode;
    if (manual_mode)
        pid->manual_output = manual_output;
}

// ========== 五点加权滑动平均滤波实现 ==========
void Filter5Point_Init(Filter5Point_TypeDef* filter, const float weights[5], float threshold, int abnormal_limit)
{
    memset(filter->window, 0, sizeof(filter->window));
    memcpy(filter->weights, weights, sizeof(filter->weights));
    filter->count = 0;
    filter->last_out = 0;
    filter->threshold = threshold;
    filter->abnormal_count = 0;
    filter->abnormal_limit = abnormal_limit;
}

float Filter5Point_Update(Filter5Point_TypeDef* filter, float in)
{
    int i;
    float out = 0;
    float absdiff = fabsf(filter->last_out - in);

    // 检查异常
    if (filter->count >= 5 && absdiff > filter->threshold)
    {
        in = filter->last_out;  // 用上次滤波结果代替异常
        filter->abnormal_count++;
    }
    else
    {
        filter->abnormal_count = 0;
    }

    // 窗口滑动
    for (i = 4; i > 0; --i)
        filter->window[i] = filter->window[i - 1];
    filter->window[0] = in;

    if (filter->count < 5)
        filter->count++;

    if (filter->count < 5)
        out = in;
    else
    {
        out = 0;
        for (i = 0; i < 5; ++i)
            out += filter->window[i] * filter->weights[i];
    }

    filter->last_out = out;
    return out;
}

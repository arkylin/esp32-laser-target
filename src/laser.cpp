#include "laser.h"
#include "config.h"
#include <driver/gpio.h>

// 当前激光状态：true = ON，false = OFF
static bool s_laserState = false;

void initLaser()
{
    // 上一次「关机」深睡时把引脚保持成了 LOW（见 power.cpp）。这里必须先解除保持，
    // 否则下面的 digitalWrite 根本不会作用到引脚上。
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)LASER_PIN);

    // 先把引脚拉到 OFF 电平再切成输出，避免上电瞬间出现短暂高电平毛刺。
    digitalWrite(LASER_PIN, LASER_OFF_LEVEL);
    pinMode(LASER_PIN, OUTPUT);
    digitalWrite(LASER_PIN, LASER_OFF_LEVEL);
    s_laserState = false;
}

void setLaser(bool on)
{
    digitalWrite(LASER_PIN, on ? LASER_ON_LEVEL : LASER_OFF_LEVEL);
    s_laserState = on;
}

void laserOn()
{
    setLaser(true);
}

void laserOff()
{
    setLaser(false);
}

bool getLaserState()
{
    return s_laserState;
}

#pragma once

#include <Arduino.h>

// 初始化激光控制引脚，并把激光置于 OFF（上电第一时间就是安全状态）。
void initLaser();

void laserOn();
void laserOff();

// 统一的开关入口，内部维护状态变量。on=true 开，false 关。
void setLaser(bool on);

// 当前激光状态：true = ON，false = OFF。
bool getLaserState();

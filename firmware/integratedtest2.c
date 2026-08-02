#include <stdio.h>

#define print tvtext_print_hidden
#include "TvText.h"
#undef print

#include "simpletools.h"
#include "adcDCpropab.h"
#include "TvText.c"

// ADC channels
#define RPM1_ADC_CH     0
#define CURRENT_ADC_CH  1
#define RPM2_ADC_CH     2

// ADC SPI pins
#define ADC_CS_PIN      21
#define ADC_CLK_PIN     20
#define ADC_DOUT_PIN    19
#define ADC_DIN_PIN     18

// RPM settings
#define THRESHOLD_HIGH  200
#define THRESHOLD_LOW   30
#define MIN_DELTA       4000000

// Digital potentiometer pins
#define POT_INC   10
#define POT_UD    11
#define POT_CS    9

// DC motor / encoder pins
#define MOTOR_IN1   1
#define MOTOR_IN2   2
#define MOTOR_EN    17
#define ENCODER_A   0

// Gear thresholds
#define UPSHIFT_1_TO_2_RPM    600
#define UPSHIFT_2_TO_3_RPM   1400
#define DOWNSHIFT_3_TO_2_RPM 1200
#define DOWNSHIFT_2_TO_1_RPM  400

// Tick calibration values
#define TICKS_1_TO_2   100
#define TICKS_2_TO_3   100

static volatile int rawRpm1 = 0;
static volatile int rawRpm2 = 0;
static volatile int currentRaw = 0;

static volatile int rpm1 = 0;
static volatile int rpm2 = 0;

static volatile int encoderTicks = 0;
static volatile int motorDirection = 0;

unsigned int adcStack[128];
unsigned int encoderStack[64];
unsigned int potStack[128];

typedef enum
{
  GEAR_1 = 1,
  GEAR_2 = 2,
  GEAR_3 = 3
} Gear;

void motorStop(void)
{
  low(MOTOR_EN);
  low(MOTOR_IN1);
  low(MOTOR_IN2);
  motorDirection = 0;
}

void motorForward(void)
{
  high(MOTOR_IN1);
  low(MOTOR_IN2);
  high(MOTOR_EN);
  motorDirection = 1;
}

void motorReverse(void)
{
  low(MOTOR_IN1);
  high(MOTOR_IN2);
  high(MOTOR_EN);
  motorDirection = -1;
}

void encoderTask(void *par)
{
  int last;
  int state;

  last = input(ENCODER_A);

  while(1)
  {
    state = input(ENCODER_A);

    if(state == 1 && last == 0)
    {
      if(motorDirection == 1)
        encoderTicks++;
      else if(motorDirection == -1)
        encoderTicks--;
    }

    last = state;
  }
}

void moveTicks(int ticks)
{
  int start;
  int target;

  start = encoderTicks;
  target = start + ticks;

  if(ticks > 0)
  {
    motorForward();
    while(encoderTicks < target)
      pause(20);
  }
  else if(ticks < 0)
  {
    motorReverse();
    while(encoderTicks > target)
      pause(20);
  }

  motorStop();
}

void adcSenseTask(void *par)
{
  int flag1;
  int flag2;
  long told1;
  long told2;
  long tnow;
  long delta;

  flag1 = 0;
  flag2 = 0;
  told1 = 0;
  told2 = 0;

  adc_init(ADC_CS_PIN, ADC_CLK_PIN, ADC_DOUT_PIN, ADC_DIN_PIN);

  while(1)
  {
    rawRpm1    = adc_in(RPM1_ADC_CH);
    currentRaw = (adc_in(CURRENT_ADC_CH) - 2010) * -1;
    rawRpm2    = adc_in(RPM2_ADC_CH);

    if(rawRpm1 > THRESHOLD_HIGH && flag1 == 0)
    {
      tnow  = CNT;
      delta = tnow - told1;

      if(told1 > 0 && delta > MIN_DELTA)
        rpm1 = (int)((4800000000.0 / delta) * 2.0);

      told1 = tnow;
      flag1 = 1;
    }
    else if(rawRpm1 < THRESHOLD_LOW)
    {
      flag1 = 0;
    }

    if(rawRpm2 > THRESHOLD_HIGH && flag2 == 0)
    {
      tnow  = CNT;
      delta = tnow - told2;

      if(told2 > 0 && delta > MIN_DELTA)
        rpm2 = (int)((4800000000.0 / delta) * 2.0);

      told2 = tnow;
      flag2 = 1;
    }
    else if(rawRpm2 < THRESHOLD_LOW)
    {
      flag2 = 0;
    }

    pause(1);
  }
}

void potTask(void *par)
{
  int i;

  while(1)
  {
    high(POT_UD);
    for(i = 0; i < 30; i++)
    {
      high(POT_INC);
      pause(200);
      low(POT_INC);
      pause(200);
    }

    pause(2000);

    low(POT_UD);
    for(i = 0; i < 30; i++)
    {
      high(POT_INC);
      pause(200);
      low(POT_INC);
      pause(200);
    }

    pause(2000);
  }
}

void shiftToGear(Gear *currentGear, Gear targetGear)
{
  if(*currentGear == targetGear)
    return;

  if(*currentGear == GEAR_1 && targetGear == GEAR_2)
  {
    moveTicks(TICKS_1_TO_2);
    *currentGear = GEAR_2;
  }
  else if(*currentGear == GEAR_2 && targetGear == GEAR_3)
  {
    moveTicks(TICKS_2_TO_3);
    *currentGear = GEAR_3;
  }
  else if(*currentGear == GEAR_3 && targetGear == GEAR_2)
  {
    moveTicks(-TICKS_2_TO_3);
    *currentGear = GEAR_2;
  }
  else if(*currentGear == GEAR_2 && targetGear == GEAR_1)
  {
    moveTicks(-TICKS_1_TO_2);
    *currentGear = GEAR_1;
  }
}

/* Shift logic  */
void handleAutomaticShifting(Gear *currentGear, int rpmForShift)
{
  if(*currentGear == GEAR_1)
  {
    if(rpmForShift >= UPSHIFT_1_TO_2_RPM)
      shiftToGear(currentGear, GEAR_2);
  }
  else if(*currentGear == GEAR_2)
  {
    if(rpmForShift >= UPSHIFT_2_TO_3_RPM)
      shiftToGear(currentGear, GEAR_3);
    else if(rpmForShift <= DOWNSHIFT_2_TO_1_RPM)
      shiftToGear(currentGear, GEAR_1);
  }
  else if(*currentGear == GEAR_3)
  {
    if(rpmForShift <= DOWNSHIFT_3_TO_2_RPM)
      shiftToGear(currentGear, GEAR_2);
  }
}

/* Fixed-position TV update */
void updateTV(int rpmDisplay1, int rpmDisplay2, int gear)
{
  char buf[32];

  tvText_out(0);   /* home cursor instead of moving down */
  tvText_str("Propeller Monitor   ");
  tvText_out(13);  /* new line */
  tvText_str("=================  ");
  tvText_out(13);

  sprintf(buf, "RPM1: %5d      ", rpmDisplay1);
  tvText_str(buf);
  tvText_out(13);

  sprintf(buf, "RPM2: %5d      ", rpmDisplay2);
  tvText_str(buf);
  tvText_out(13);

  sprintf(buf, "Gear: %1d         ", gear);
  tvText_str(buf);
  tvText_out(13);

  tvText_str("                ");
}

int main()
{
  Gear currentGear;

  currentGear = GEAR_1;

  pause(1000);

  low(POT_CS);
  high(POT_UD);
  low(POT_INC);

  low(MOTOR_IN1);
  low(MOTOR_IN2);
  low(MOTOR_EN);

  tvText_start(12);
  pause(500);
  tvText_out(12);   /* clear once at startup */

  cogstart(adcSenseTask, NULL, adcStack, sizeof(adcStack));
  cogstart(encoderTask, NULL, encoderStack, sizeof(encoderStack));
  cogstart(potTask, NULL, potStack, sizeof(potStack));

  while(1)
  {
    handleAutomaticShifting(&currentGear, rpm1);
    updateTV(rpm1, rpm2, currentGear);
    pause(200);
  }

  return 0;
}
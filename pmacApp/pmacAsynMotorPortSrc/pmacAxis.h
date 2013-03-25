/********************************************
 *  pmacAxis.cpp
 * 
 *  PMAC Asyn motor based on the 
 *  asynMotorAxis class.
 * 
 *  Matthew Pearson
 *  23 May 2012
 * 
 ********************************************/

#ifndef pmacAxis_H
#define pmacAxis_H

#include "asynMotorController.h"
#include "asynMotorAxis.h"

class pmacController;

class pmacAxis : public asynMotorAxis
{
  public:
  /* These are the methods we override from the base class */
  pmacAxis(pmacController *pController, int axisNo);
  virtual ~pmacAxis();
  asynStatus move(double position, int relative, double min_velocity, double max_velocity, double acceleration);
  asynStatus moveVelocity(double min_velocity, double max_velocity, double acceleration);
  asynStatus home(double min_velocity, double max_velocity, double acceleration, int forwards);
  asynStatus stop(double acceleration);
  asynStatus poll(bool *moving);
  asynStatus setPosition(double position);
  asynStatus setClosedLoop(bool closedLoop);
  
  private:
  pmacController *pC_;
  
  asynStatus getAxisStatus(bool *moving);
  asynStatus getAxisInitialStatus(void);

  double setpointPosition_;
  double encoderPosition_;
  double currentVelocity_;
  double velocity_;
  double accel_;
  double highLimit_;
  double lowLimit_;
  int limitsDisabled_;
  double stepSize_;
  double deferredPosition_;
  int deferredMove_;
  int deferredRelative_;
  int scale_;
  double previous_position_;
  int previous_direction_;
  int amp_enabled_;
  int fatal_following_;
  int encoder_axis_;
  int limitsCheckDisable_;
  int errorPrintCount_;
  int errorPrintFlag_;

  friend class pmacController;
};


#endif /* pmacAxis_H */

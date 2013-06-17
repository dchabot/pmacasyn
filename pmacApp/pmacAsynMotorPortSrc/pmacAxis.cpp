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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <epicsTime.h>
#include <epicsThread.h>
#include <epicsExport.h>
#include <epicsExit.h>
#include <epicsString.h>
#include <iocsh.h>

#include "pmacController.h"
#include <iostream>
using std::cout;
using std::endl;

/////////////////replace with a runtime function that can be called on IOC shell.////////////////////////
/////////////////Or, provide an overloaded constructor with this as an argument.////////////////////////
/* This #define affects the behaviour of the driver.

   REMOVE_LIMITS_ON_HOME removes the limits protection when homing if
    - ms??,i912 indicates you are homing onto a limit
    - ms??,i913 and the home velocity indicate that the limit you trigger.
      for home detection is the one you are homing towards.
    - any home offset is in the opposite sense to the home velocity.
*/

#define REMOVE_LIMITS_ON_HOME

static void shutdownCallback(void *pPvt)
{
  pmacController *pC = static_cast<pmacController *>(pPvt);

  pC->lock();
  pC->shuttingDown_ = 1;
  pC->unlock();
}

/**
 * pmacAxis constructor.
 * @param pC Pointer to a pmacController object.
 * @param axisNo The axis number for this pmacAxis (1 based).
 */
pmacAxis::pmacAxis(pmacController *pC, int axisNo)
  :   asynMotorAxis(pC, axisNo),
      pC_(pC)
{
  static const char *functionName = "pmacAxis::pmacAxis";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  //Initialize non-static data members
  setpointPosition_ = 0.0;
  encoderPosition_ = 0.0;
  currentVelocity_ = 0.0;
  velocity_ = 0.0;
  accel_ = 0.0;
  highLimit_ = 0.0;
  lowLimit_ = 0.0;
  limitsDisabled_ = 0;
  stepSize_ = 1; //Don't need?
  deferredPosition_ = 0.0;
  deferredMove_ = 0;
  deferredRelative_ = 0;
  scale_ = 1;
  previous_position_ = 0.0;
  previous_direction_ = 0;
  amp_enabled_ = 0;
  fatal_following_ = 0;
  encoder_axis_ = 0;
  limitsCheckDisable_ = 0;
  nowTimeSecs_ = 0.0;
  lastTimeSecs_ = 0.0;
  printNextError_ = false;

  /* Set an EPICS exit handler that will shut down polling before asyn kills the IP sockets */
  epicsAtExit(shutdownCallback, pC_);

  //Do an initial poll to get some values from the PMAC
  if (getAxisInitialStatus() != asynSuccess) {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR,
	      "%s: getAxisInitialStatus failed to return asynSuccess. Controller: %s, Axis: %d.\n", functionName, pC_->portName, axisNo_);
  }

  callParamCallbacks();

  /* Wake up the poller task which will make it do a poll, 
   * updating values for this axis to use the new resolution (stepSize_) */   
  pC_->wakeupPoller();
 
}

/**
 * Poll for initial axis status (soft limits, PID settings).
 * Set parameters needed for correct motor record behaviour.
 * @return asynStatus
 */
asynStatus pmacAxis::getAxisInitialStatus(void)
{
  char command[PMAC_MAXBUF] = {0};
  char response[PMAC_MAXBUF] = {0};
  int cmdStatus = 0;
  double low_limit = 0.0;
  double high_limit = 0.0;
  double pgain = 0.0;
  double igain = 0.0;
  double dgain = 0.0;
  int nvals = 0;

  static const char *functionName = "pmacAxis::getAxisInitialStatus";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  if (axisNo_ != 0) {

    sprintf(command, "I%d13 I%d14 I%d30 I%d31 I%d33", axisNo_, axisNo_, axisNo_, axisNo_, axisNo_);
    cmdStatus = pC_->lowLevelWriteRead(command, response);
    nvals = sscanf(response, "%lf %lf %lf %lf %lf", &high_limit, &low_limit, &pgain, &dgain, &igain);
    
    if (cmdStatus || nvals != 5) {
      asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR, "%s: Error: initial status poll failed on axis %d.\n", functionName, axisNo_);
      return asynError;
    } else {
      setDoubleParam(pC_->motorLowLimit_,  low_limit*scale_);
      setDoubleParam(pC_->motorHighLimit_, high_limit*scale_);
      setDoubleParam(pC_->motorPGain_,     pgain);
      setDoubleParam(pC_->motorIGain_,     igain);
      setDoubleParam(pC_->motorDGain_,     dgain);
      setIntegerParam(pC_->motorStatusHasEncoder_, 1);
      setIntegerParam(pC_->motorStatusGainSupport_, 1);
    }
    
  }
  
  return asynSuccess;
}


pmacAxis::~pmacAxis() 
{
  //Destructor
}


/**
 * See asynMotorAxis::move
 */
asynStatus pmacAxis::move(double position, int relative, double min_velocity, double max_velocity, double acceleration)
{
  asynStatus status = asynError;
  static const char *functionName = "pmacAxis::move";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  char acc_buff[PMAC_MAXBUF] = {0};
  char vel_buff[PMAC_MAXBUF] = {0};
  char command[PMAC_MAXBUF]  = {0};
  char response[PMAC_MAXBUF] = {0};

  if (max_velocity != 0) {
    sprintf(vel_buff, "I%d22=%f ", axisNo_, (max_velocity / (scale_ * 1000.0) ));
  }
  if (acceleration != 0) {
    if (max_velocity != 0) {
      sprintf(acc_buff, "I%d20=%f ", axisNo_, (fabs(max_velocity/acceleration) * 1000.0));
    }
  }
  
  if (pC_->movesDeferred_ == 0) {
    sprintf(command, "%s%s#%d %s%.2f", vel_buff, acc_buff, axisNo_,
	     (relative?"J^":"J="), position/scale_ );
  } else { /* deferred moves */
    sprintf(command, "%s%s", vel_buff, acc_buff);
    deferredPosition_ = position/scale_;
    deferredMove_ = 1;
    deferredRelative_ = relative;
  }
        
#ifdef REMOVE_LIMITS_ON_HOME
  if (limitsDisabled_) {
    char buffer[PMAC_MAXBUF] = {0};
    /* Re-enable limits */
    sprintf(buffer, " i%d24=i%d24&$FDFFFF", axisNo_, axisNo_);
    strncat(command, buffer, PMAC_MAXBUF-1);
    limitsDisabled_ = 0;
  }
#endif
  status = pC_->lowLevelWriteRead(command, response);
  
  return status;
}


/**
 * See asynMotorAxis::home
 */ 
asynStatus pmacAxis::home(double min_velocity, double max_velocity, double acceleration, int forwards)
{
  asynStatus status = asynError;
  char command[PMAC_MAXBUF] = {0};
  char response[PMAC_MAXBUF] = {0};
  static const char *functionName = "pmacAxis::home";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  sprintf(command, "#%d HOME", axisNo_);
  
#ifdef REMOVE_LIMITS_ON_HOME
 /* If homing onto an end-limit and home velocity is in the right direction, clear limits protection */
  int macro_station = ((axisNo_-1)/2)*4 + (axisNo_-1)%2;
  int home_type = 0;
  int home_flag = 0;
  int flag_mode = 0;
  int nvals = 0;
  int home_offset = 0;
  int controller_type = 0;
  double home_velocity = 0.0;
  char buffer[PMAC_MAXBUF] = {0};

  /* Discover type of controller */
  strncpy(buffer, "cid", PMAC_MAXBUF);
  status = pC_->lowLevelWriteRead(buffer, response);
  if (status != asynSuccess) {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR, 
	      "Controller %s Addr %d. %s: ERROR Reading Controller Type.\n", pC_->portName, axisNo_, functionName);
    return asynError;
  }
  nvals  = sscanf(response, "%d", &controller_type);
  
  if (controller_type == pC_->PMAC_CID_GEOBRICK_) {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, 
	      "Controller %s Addr %d. %s: This is a Geobrick LV.\n", pC_->portName, axisNo_, functionName);
  } else if (controller_type == pC_->PMAC_CID_PMAC_) {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, 
	      "Controller %s Addr %d. %s: This is a Turbo PMAC 2 Ultralite.\n", pC_->portName, axisNo_, functionName);
  } else {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR, 
	      "Controller %s Addr %d. %s: ERROR Unknown controller type = %d.\n", pC_->portName, axisNo_, functionName, controller_type);
    return asynError;
  }

  if (controller_type == pC_->PMAC_CID_GEOBRICK_) {
    /* Read home flags and home direction from Geobrick LV */ 
    if (axisNo_ < 5) {
      sprintf(buffer, "I70%d2 I70%d3 i%d24 i%d23 i%d26", axisNo_, axisNo_, axisNo_, axisNo_, axisNo_);
    } else {
      sprintf(buffer, "I71%d2 I71%d3 i%d24 i%d23 i%d26", axisNo_-4, axisNo_-4, axisNo_, axisNo_, axisNo_);
    }
    status = pC_->lowLevelWriteRead(buffer, response);
    nvals = sscanf(response, "%d %d $%x %lf %d", &home_type, &home_flag, &flag_mode, &home_velocity, &home_offset);
  }
  
  if( controller_type == pC_->PMAC_CID_PMAC_) {
    /* Read home flags and home direction from VME PMAC */ 
    sprintf(buffer, "ms%d,i912 ms%d,i913 i%d24 i%d23 i%d26", macro_station, macro_station, axisNo_, axisNo_, axisNo_);
    status = pC_->lowLevelWriteRead(buffer, response);
    nvals = sscanf(response, "$%x $%x $%x %lf %d", &home_type, &home_flag, &flag_mode, &home_velocity, &home_offset);
  }

  if ((status != asynSuccess) || (nvals != 5)) {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR,
	      "Controller %s Addr %d. %s: ERROR Cannot Read Home Flags.\n", pC_->portName, axisNo_, functionName);
    return asynError;
  }

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, 
	      "Controller %s Addr %d. %s: .home_type = %d, home_flag = %d, flag_mode = %x, home_velocity = %f, home_offset = %d\n", 
	    pC_->portName, axisNo_, functionName, home_type, home_flag, flag_mode, home_velocity, home_offset);
  
  if (max_velocity != 0) {
    home_velocity = (forwards?1:-1)*(fabs(max_velocity) / 1000.0);
  }

  if ( ( home_type <= 15 )      && 
       ( home_type % 4 >= 2 )   &&
       !( flag_mode & 0x20000 ) &&
       (( home_velocity > 0 && home_flag == 1 && home_offset <= 0 ) || 
	( home_velocity < 0 && home_flag == 2 && home_offset >= 0 )    )   ) {
      sprintf(buffer, " i%d24=i%d24|$20000", axisNo_, axisNo_ );
      strncat(command, buffer, PMAC_MAXBUF-1);
      limitsDisabled_ = 1;
      asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, 
		"%s. Disabling limits whilst homing PMAC controller %s, axis %d, type:%d, flag:$%x, vel:%f\n",
		functionName, pC_->portName, axisNo_, home_type, home_flag, home_velocity);
  } else {
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR,
	      "%s: Error: Cannot disable limits to home PMAC controller %s, axis %d, type:%x, flag:$%d, vel:%f, mode:0x%x, offset: %d\n", 
	      functionName, pC_->portName, axisNo_, home_type, home_flag, home_velocity, flag_mode, home_offset);
  }
#endif
  status = pC_->lowLevelWriteRead(command, response);

  return status;
}

/**
 * See asynMotorAxis::moveVelocity
 */
asynStatus pmacAxis::moveVelocity(double min_velocity, double max_velocity, double acceleration)
{
  asynStatus status = asynError;
  char acc_buff[PMAC_MAXBUF] = {0};
  char vel_buff[PMAC_MAXBUF] = {0};
  char command[PMAC_MAXBUF]  = {0};
  char response[PMAC_MAXBUF] = {0};
  static const char *functionName = "pmacAxis::moveVelocity";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  if (max_velocity != 0) {
    sprintf(vel_buff, "I%d22=%f ", axisNo_, (fabs(max_velocity) / (scale_ * 1000.0)));
  }
  if (acceleration != 0) {
    if (max_velocity != 0) {
      sprintf(acc_buff, "I%d20=%f ", axisNo_, (fabs(max_velocity/acceleration) * 1000.0));
    }
  }
  sprintf(command, "%s%s#%d %s", vel_buff, acc_buff, axisNo_, (max_velocity < 0 ? "J-": "J+") );

#ifdef REMOVE_LIMITS_ON_HOME
  if (limitsDisabled_) {
    char buffer[PMAC_MAXBUF];
    /* Re-enable limits */
    sprintf(buffer, " i%d24=i%d24&$FDFFFF", axisNo_, axisNo_);
    strncat(command, buffer, PMAC_MAXBUF-1);
    limitsDisabled_ = 0;
  }
#endif
  status = pC_->lowLevelWriteRead(command, response);

  return status;
}

/**
 * See asynMotorAxis::setPosition
 */
asynStatus pmacAxis::setPosition(double position)
{
  //int status = 0;
  static const char *functionName = "pmacAxis::setPosition";
  
  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  return asynSuccess;
}

/**
 * See asynMotorAxis::stop
 */
asynStatus pmacAxis::stop(double acceleration)
{
  asynStatus status = asynError;
  static const char *functionName = "pmacAxis::stopAxis";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  char command[PMAC_MAXBUF]  = {0};
  char response[PMAC_MAXBUF] = {0};

  /*Only send a J/ if the amplifier output is enabled. When we send a stop, 
    we don't want to power on axes that have been powered off for a reason.*/
  if ((amp_enabled_ == 1) || (fatal_following_ == 1)) {
    sprintf(command, "#%d J/ M%d40=1",  axisNo_, axisNo_ );
  } else {
    /*Just set the inposition bit in this case.*/
    sprintf(command, "M%d40=1",  axisNo_ );
  }
  deferredMove_ = 0;

  status = pC_->lowLevelWriteRead(command, response);
  return status;
}

/**
 * See asynMotorAxis::setClosedLoop
 */
asynStatus pmacAxis::setClosedLoop(bool closedLoop)
{
  asynStatus status = asynError;
  static const char *functionName = "pmacAxis::setClosedLoop";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);

  char command[PMAC_MAXBUF]  = {0};
  char response[PMAC_MAXBUF] = {0};
  
  if (closedLoop) {
    sprintf(command, "#%d J/",  axisNo_);
  } else {
    sprintf(command, "#%d K",  axisNo_);
  }
  status = pC_->lowLevelWriteRead(command, response);
  return status;
}

/**
 * See asynMotorAxis::poll
 */
asynStatus pmacAxis::poll(bool *moving)
{
  asynStatus status = asynSuccess;
  static const char *functionName = "pmacAxis::poll";

  asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s Polling axis: %d\n", functionName, this->axisNo_);

  if (axisNo_ != 0) {

    if (!pC_->lowLevelPortUser_) {
      setIntegerParam(pC_->motorStatusCommsError_, 1);
      return asynError;
    }
    
    //Now poll axis status
    if ((status = getAxisStatus(moving)) != asynSuccess) {
      asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR,
		"Controller %s Axis %d. %s: getAxisStatus failed to return asynSuccess.\n", pC_->portName, axisNo_, functionName);
    }
  }
  
  callParamCallbacks();
  return status;
}


/**
 * Read the axis status and set axis related parameters.
 * @param moving Boolean flag to indicate if the axis is moving. This is set by this function
 * to indcate to the polling thread how quickly to poll for status.
 * @return asynStatus
 */
asynStatus pmacAxis::getAxisStatus(bool *moving)
{
    char command[PMAC_MAXBUF] = {0};
    char response[PMAC_MAXBUF] = {0};
    int cmdStatus = 0;; 
    int done = 0;
    double position = 0; 
    double enc_position = 0;
    int nvals = 0;
    epicsUInt32 status[2] = {0};
    int axisProblemFlag = 0;
    int limitsDisabledBit = 0;
    bool printErrors = true;

    static const char *functionName = "pmacAxis::getAxisStatus";
    
    asynPrint(pC_->pasynUserSelf, ASYN_TRACE_FLOW, "%s\n", functionName);
    
    /* Get the time and decide if we want to print errors.*/
    epicsTimeGetCurrent(&nowTime_);
    nowTimeSecs_ = nowTime_.secPastEpoch;
    if ((nowTimeSecs_ - lastTimeSecs_) < pC_->PMAC_ERROR_PRINT_TIME_) {
      printErrors = 0;
    } else {
      printErrors = 1;
      lastTimeSecs_ = nowTimeSecs_;
    }
    
    if (printNextError_) {
      printErrors = 1;
    }
            
    /* Read all the status for this axis in one go */
    if (encoder_axis_ != 0) {
      /* Encoder position comes back on a different axis */
      sprintf(command, "#%d ? P #%d P", axisNo_,  encoder_axis_);
    } else {
      /* Encoder position comes back on this axis - note we initially read 
	 the following error into the position variable */
      sprintf(command, "#%d ? F P", axisNo_);
    }
    
    cmdStatus = pC_->lowLevelWriteRead(command, response);
    nvals = sscanf( response, "%6x%6x %lf %lf", &status[0], &status[1], &position, &enc_position );
	
    if ( cmdStatus || nvals != 4) {
      asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR,
		"drvPmacAxisGetStatus: not all status values returned. Status: %d\nCommand :%s\nResponse:%s",
		cmdStatus, command, response );
    } else {
      int homeSignal = ((status[1] & pC_->PMAC_STATUS2_HOME_COMPLETE) != 0);
      int direction = 0;
      
      /* For closed loop axes, position is actually following error up to this point */ 
      if ( encoder_axis_ == 0 ) {
	position += enc_position;
      }
      
      position *= scale_;
      enc_position  *= scale_;
      
      setDoubleParam(pC_->motorPosition_, position);
      setDoubleParam(pC_->motorEncoderPosition_, enc_position);
      
      /* Use previous position and current position to calculate direction.*/
      if ((position - previous_position_) > 0) {
	direction = 1;
      } else if (position - previous_position_ == 0.0) {
	direction = previous_direction_;
      } else {
	direction = 0;
      }
      setIntegerParam(pC_->motorStatusDirection_, direction);
      /*Store position to calculate direction for next poll.*/
      previous_position_ = position;
      previous_direction_ = direction;

      if(deferredMove_) {
	done = 0; 
      } else {
	done = (((status[1] & pC_->PMAC_STATUS2_IN_POSITION) != 0) || ((status[0] & pC_->PMAC_STATUS1_MOTOR_ON) == 0)); 
	/*If we are not done, but amp has been disabled, then set done (to stop when we get following errors).*/
	if ((done == 0) && ((status[0] & pC_->PMAC_STATUS1_AMP_ENABLED) == 0)) {
	  done = 1;
	}
      }

      if (!done) {
	*moving = true;
      } else {
	*moving = false;
      }

      setIntegerParam(pC_->motorStatusDone_, done);
      setIntegerParam(pC_->motorStatusHighLimit_, ((status[0] & pC_->PMAC_STATUS1_POS_LIMIT_SET) != 0) );
      setIntegerParam(pC_->motorStatusHomed_, homeSignal);
      /*If desired_vel_zero is false && motor activated (ix00=1) && amplifier enabled, set moving=1.*/
      setIntegerParam(pC_->motorStatusMoving_, ((status[0] & pC_->PMAC_STATUS1_DESIRED_VELOCITY_ZERO) == 0) && ((status[0] & pC_->PMAC_STATUS1_MOTOR_ON) != 0) && ((status[0] & pC_->PMAC_STATUS1_AMP_ENABLED) != 0) );
      setIntegerParam(pC_->motorStatusLowLimit_, ((status[0] & pC_->PMAC_STATUS1_NEG_LIMIT_SET)!=0) );
      setIntegerParam(pC_->motorStatusFollowingError_,((status[1] & pC_->PMAC_STATUS2_ERR_FOLLOW_ERR) != 0) );
      fatal_following_ = ((status[1] & pC_->PMAC_STATUS2_ERR_FOLLOW_ERR) != 0);

      axisProblemFlag = 0;
      /*Set any axis specific general problem bits.*/
      if ( ((status[0] & pC_->PMAX_AXIS_GENERAL_PROB1) != 0) || ((status[1] & pC_->PMAX_AXIS_GENERAL_PROB2) != 0) ) {
	axisProblemFlag = 1;
      }

      int globalStatus = 0;
      int feedrate_problem = 0;
      pC_->getIntegerParam(0, pC_->PMAC_C_GlobalStatus_, &globalStatus);
      pC_->getIntegerParam(0, pC_->PMAC_C_FeedRateProblem_, &feedrate_problem);
      if (globalStatus || feedrate_problem) {
	axisProblemFlag = 1;
      }
      /*Check limits disabled bit in ix24, and if we haven't intentially disabled limits
	because we are homing, set the motorAxisProblem bit. Also check the limitsCheckDisable
	flag, which the user can set to disable this feature.*/
      if (!limitsCheckDisable_) {
	/*Check we haven't intentially disabled limits for homing.*/
	if (!limitsDisabled_) {
	  sprintf(command, "i%d24", axisNo_);
	  cmdStatus = pC_->lowLevelWriteRead(command, response);
	  if (cmdStatus == asynSuccess) {
	    sscanf(response, "$%x", &limitsDisabledBit);
	    limitsDisabledBit = ((0x20000 & limitsDisabledBit) >> 17);
	    if (limitsDisabledBit) {
	      axisProblemFlag = 1;
	      if (printErrors) {
		asynPrint(pC_->pasynUserSelf, ASYN_TRACE_ERROR, "*** WARNING *** Limits are disabled on controller %s, axis %d\n", pC_->portName, axisNo_);
		printNextError_ = false;
	      }

	    }
	  }
	}
      }
      setIntegerParam(pC_->motorStatusProblem_, axisProblemFlag);
      
      /* Clear error print flag for this axis if problem has been removed.*/
      if (axisProblemFlag == 0) {
	printNextError_ = true;
      }
            

    }

#ifdef REMOVE_LIMITS_ON_HOME
    if (limitsDisabled_ && (status[1] & pC_->PMAC_STATUS2_HOME_COMPLETE) && (status[0] & pC_->PMAC_STATUS1_DESIRED_VELOCITY_ZERO) ) {
      /* Re-enable limits */
      sprintf(command, "i%d24=i%d24&$FDFFFF", axisNo_, axisNo_);
      cmdStatus = pC_->lowLevelWriteRead(command, response);
      limitsDisabled_ = (cmdStatus != 0);
    }
#endif
    /*Set amplifier enabled bit.*/
    if ((status[0] & pC_->PMAC_STATUS1_AMP_ENABLED) != 0) {
      amp_enabled_ = 1;
    } else {
      amp_enabled_ = 0;
    }
    setIntegerParam(pC_->motorStatusPowerOn_, amp_enabled_);
    
    return asynSuccess;
}


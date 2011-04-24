/* File:    drvSIS3820.cpp
 * Author:  Mark Rivers
 * Date:    22-Apr-2011
 *
 * Purpose: 
 * This module provides the driver support for the MCA asyn device support layer
 * for the SIS3820 and SIS3801 multichannel scalers.  This is the SIS3920 class.
 *
 * Acknowledgements:
 * This driver module is based on previous versions by Wayne Lewis and Ulrik Pedersen.
 *
 */

/*********/
/* To do */
/*********/

/*
 * - Track down problem with FIFO threshold interrupts.  
 *     Works fine with 1024 word threshold down to 100 microsecond dwell time,
 *     below that it messes up, 100% CPU time.
 */

/*******************/
/* System includes */
/*******************/

#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/******************/
/* EPICS includes */
/******************/

#include <cantProceed.h>
#include <devLib.h>
#include <epicsString.h>
#include <epicsThread.h>
#include <epicsTime.h>
#include <epicsExport.h>
#include <errlog.h>
#include <iocsh.h>
#include <initHooks.h>

/*******************/
/* Custom includes */
/*******************/

#include "drvMca.h"
#include "devScalerAsyn.h"
#include "drvSIS3820.h"
#include "sis3820.h"

static const char *driverName="drvSIS3820";
static void intFuncC(void *drvPvt);
static void readFIFOThreadC(void *drvPvt);

/***************/
/* Definitions */
/***************/

/*Constructor */
drvSIS3820::drvSIS3820(const char *portName, int baseAddress, int interruptVector, int interruptLevel, 
                       int maxChans, int maxSignals, int inputMode, int outputMode, 
                       bool useDma, int fifoBufferWords)
  :  drvSIS38XX(portName, maxChans,maxSignals)

{
  int status;
  int firmware;
  epicsUInt32 controlStatusReg;
  static const char* functionName="SIS3820";
  
  useDma_ = useDma;
  
  setStringParam(SIS38XXModel_, "SIS3820");
  
  /* Call devLib to get the system address that corresponds to the VME
   * base address of the board.
   */
  status = devRegisterAddress("drvSIS3820",
                               SIS3820_ADDRESS_TYPE,
                               (size_t)baseAddress,
                               SIS3820_BOARD_SIZE,
                               (volatile void **)&registers_);

  if (status) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: %s, Can't register VME address %p\n", 
              driverName, functionName, portName, baseAddress);
    return;
  }
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: Registered VME address: %p to local address: %p size: 0x%X\n", 
            driverName, functionName, baseAddress, registers_, SIS3820_BOARD_SIZE);

  /* Call devLib to get the system address that corresponds to the VME
   * FIFO address of the board.
   */
  status = devRegisterAddress("drvSIS3820",
                              SIS3820_ADDRESS_TYPE,
                              (size_t)(baseAddress + SIS3820_FIFO_BASE),
                              SIS3820_FIFO_BYTE_SIZE,
                              (volatile void **)&fifo_base_);

  if (status) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: %s, Can't register FIFO address %p\n", 
              driverName, functionName, portName, baseAddress + SIS3820_FIFO_BASE);
    return;
  }

  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: Registered VME FIFO address: %p to local address: %p size: 0x%X\n", 
            driverName, functionName, baseAddress + SIS3820_FIFO_BASE, 
            fifo_base_, SIS3820_FIFO_BYTE_SIZE);

  /* Probe VME bus to see if card is there */
  status = devReadProbe(4, (char *) registers_->control_status_reg,
                       (char *) &controlStatusReg);
  if (status) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: devReadProbe failure = %d\n", 
              driverName, functionName, status);
    return;
  }

  /* Get the module info from the card */
  moduleID_ = (registers_->moduleID_reg & 0xFFFF0000) >> 16;
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: module ID=%x\n", 
            driverName, functionName, moduleID_);
  firmware = registers_->moduleID_reg & 0x0000FFFF;
  setIntegerParam(SIS38XXFirmware_, firmware);
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: firmware=%d\n",
            driverName, functionName, firmware);

  /* Assume initially that the LNE is internal */
  lneSource_ = SIS3820_LNE_SOURCE_INTERNAL_10MHZ;
  softAdvance_ = 1;

  // Set a default value of lnePrescale for 1 second
  lnePrescale_ = 10000000;

  /* Enable channel 1 reference pulses by default */
  setIntegerParam(SIS38XXCh1RefEnable_, 1);

  // Allocate FIFO readout buffer
  // fifoBufferWords input argument is in words, must be less than SIS3820_FIFO_WORD_SIZE
  if (fifoBufferWords == 0) fifoBufferWords = SIS3820_FIFO_WORD_SIZE;
  if (fifoBufferWords > SIS3820_FIFO_WORD_SIZE) fifoBufferWords = SIS3820_FIFO_WORD_SIZE;
  fifoBufferWords_ = fifoBufferWords;
  fifoBuffer_ = (epicsUInt32*) memalign(8, fifoBufferWords_*sizeof(epicsUInt32));
  if (fifoBuffer_ == NULL) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: posix_memalign failure for fifoBuffer_ = %d\n", 
              driverName, functionName, status);
    return;
  }
  
  // Create the mutex used to lock access to the FIFO
  fifoLockId_ = epicsMutexCreate();

  /* Reset card */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: resetting port %s\n", 
            driverName, functionName, portName);
  registers_->key_reset_reg = 1;

  /* Clear FIFO */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
           "%s:%s: clearing FIFO\n",
           driverName, functionName);
  resetFIFO();

  /* Initialize board in MCS mode */
  setAcquireMode(MCS_MODE);

  /* Set up the interrupt service routine */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: interruptServiceRoutine pointer %p\n",
            driverName, functionName, intFuncC);

  status = devConnectInterruptVME(interruptVector, intFuncC, this);
  if (status) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: Can't connect to vector % d\n", 
              driverName, functionName, interruptVector);
    return;
  }
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: Connected interrupt vector: %d\n\n",
            driverName, functionName, interruptVector);
  
  /* Write interrupt level to hardware */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: irq before setting IntLevel= 0x%x\n", 
            driverName, functionName, registers_->irq_config_reg);

  registers_->irq_config_reg &= ~SIS3820_IRQ_LEVEL_MASK;
  registers_->irq_config_reg |= (interruptLevel << 8);

  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: IntLevel mask= 0x%x\n", 
            driverName, functionName, (interruptLevel << 8));
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: irq after setting IntLevel= 0x%x\n", 
             driverName, functionName, registers_->irq_config_reg);

  /* Write interrupt vector to hardware */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: irq before setting IntLevel= 0x%x\n", 
            driverName, functionName, registers_->irq_config_reg);

  registers_->irq_config_reg &= ~SIS3820_IRQ_VECTOR_MASK;
  registers_->irq_config_reg |= interruptVector;

  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: irq = 0x%08x\n", 
            driverName, functionName, registers_->irq_config_reg);

  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "m%s:%s: irq config register after enabling interrupts= 0x%08x\n", 
            driverName, functionName, registers_->irq_config_reg);

  /* Create the thread that reads the FIFO */
  if (epicsThreadCreate("SIS3820FIFOThread",
                         epicsThreadPriorityLow,
                         epicsThreadGetStackSize(epicsThreadStackMedium),
                         (EPICSTHREADFUNC)readFIFOThreadC,
                         this) == NULL) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: epicsThreadCreate failure\n", 
              driverName, functionName);
    return;
  }
  else
    asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: epicsThreadCreate success\n", 
            driverName, functionName, registers_->irq_config_reg);

  erase();

  /* Enable interrupts in hardware */  
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: irq before enabling interrupts= 0x%08x\n", 
            driverName, functionName, registers_->irq_config_reg);

  registers_->irq_config_reg |= SIS3820_IRQ_ENABLE;

  /* Enable interrupt level in EPICS */
  status = devEnableInterruptLevel(intVME, interruptLevel);
  if (status) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: Can't enable enterrupt level %d\n", 
              driverName, functionName, interruptLevel);
    return;
  }
  
  exists_ = true;
  return;
}



/* Report  parameters */
void drvSIS3820::report(FILE *fp, int details)
{

  fprintf(fp, "SIS3820: asyn port: %s, connected at VME base address %p, maxChans=%d\n",
          portName, registers_, maxChans_);
  if (details > 1) {
    fprintf(fp, "  Registers:\n");
    fprintf(fp, "    control_status_reg         = 0x%x\n",   registers_->control_status_reg);
    fprintf(fp, "    moduleID_reg               = 0x%x\n",   registers_->moduleID_reg);
    fprintf(fp, "    irq_config_reg             = 0x%x\n",   registers_->irq_config_reg);
    fprintf(fp, "    irq_control_status_reg     = 0x%x\n",   registers_->irq_control_status_reg);
    fprintf(fp, "    acq_preset_reg             = 0x%x\n",   registers_->acq_preset_reg);
    fprintf(fp, "    acq_count_reg              = 0x%x\n",   registers_->acq_count_reg);
    fprintf(fp, "    lne_prescale_factor_reg    = 0x%x\n",   registers_->lne_prescale_factor_reg);
    fprintf(fp, "    preset_group1_reg          = 0x%x\n",   registers_->preset_group1_reg);
    fprintf(fp, "    preset_group2_reg          = 0x%x\n",   registers_->preset_group2_reg);
    fprintf(fp, "    preset_enable_reg          = 0x%x\n",   registers_->preset_enable_reg);
    fprintf(fp, "    cblt_setup_reg             = 0x%x\n",   registers_->cblt_setup_reg);
    fprintf(fp, "    sdram_page_reg             = 0x%x\n",   registers_->sdram_page_reg);
    fprintf(fp, "    fifo_word_count_reg        = 0x%x\n",   registers_->fifo_word_count_reg);
    fprintf(fp, "    fifo_word_threshold_reg    = 0x%x\n",   registers_->fifo_word_threshold_reg);
    fprintf(fp, "    hiscal_start_preset_reg    = 0x%x\n",   registers_->hiscal_start_preset_reg);
    fprintf(fp, "    hiscal_start_counter_reg   = 0x%x\n",   registers_->hiscal_start_counter_reg);
    fprintf(fp, "    hiscal_last_acq_count_reg  = 0x%x\n",   registers_->hiscal_last_acq_count_reg);
    fprintf(fp, "    op_mode_reg                = 0x%x\n",   registers_->op_mode_reg);
  }
      // Call the base class method
  drvSIS38XX::report(fp, details);
}

void drvSIS3820::erase()
{
  //static const char *functionName="erase";

  if (!exists_) return;
  
  // Call the base class method
  drvSIS38XX::erase();
  
  /* Erase FIFO and counters on board */
  resetFIFO();
  registers_->key_counter_clear = 1;

  return;
}

void drvSIS3820::startMCSAcquire()
{
  setOpModeReg();
  int nChans;
  static const char *functionName="startMCSAcquire";
  
  getIntegerParam(mcaNumChannels_, &nChans);

  /* Set the number of channels to acquire */
  registers_->acq_preset_reg = nChans;


  if (lneSource_ == SIS3820_LNE_SOURCE_INTERNAL_10MHZ) {
    /* The SIS3820 requires the value in the LNE prescale register to be one
     * less than the actual number of incoming signals. We do this adjustment
     * here, so the user sees the actual number at the record level.
     */
    double dwellTime;
    getDoubleParam(mcaDwellTime_, &dwellTime);
    registers_->lne_prescale_factor_reg = 
      (epicsUInt32) (SIS3820_10MHZ_CLOCK * dwellTime) - 1;
    registers_->key_op_enable_reg = 1;

  }
  else if (lneSource_ == SIS3820_LNE_SOURCE_CONTROL_SIGNAL) {
    /* The SIS3820 requires the value in the LNE prescale register to be one
     * less than the actual number of incoming signals. We do this adjustment
     * here, so the user sees the actual number at the record level.
     */
    registers_->lne_prescale_factor_reg = lnePrescale_ - 1;
    registers_->key_op_arm_reg = 1;
  } 
  else {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
              "%s:%s: unsupported lneSource %d\n", 
              driverName, functionName, lneSource_);
  }
}

void drvSIS3820::stopMCSAcquire()
{
  /* Turn off hardware acquisition */
  registers_->key_op_disable_reg = 1;
}

void drvSIS3820::setChannelAdvanceSource()
{
  int source;
  static const char *functionName="setChannelAdvanceSource";
  
  getIntegerParam(mcaChannelAdvanceSource_, &source);
  if (source == mcaChannelAdvance_Internal) {
    /* set channel advance source to internal (timed) */
    /* Just cache this setting here, set it when acquisition starts */
    lneSource_ = SIS3820_LNE_SOURCE_INTERNAL_10MHZ;
  }
  else if (source == mcaChannelAdvance_External) {
    /* set channel advance source to external */
    /* Just cache this setting here, set it when acquisition starts */
    lneSource_ = SIS3820_LNE_SOURCE_CONTROL_SIGNAL;
  }
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: (mcaChannelAdvanceInternal) pPvt-lneSource = 0x%x\n", 
            driverName, functionName, lneSource_);
}


void drvSIS3820::startScaler()
{
  registers_->key_op_enable_reg = 1;
}

void drvSIS3820::stopScaler()
{
  registers_->key_op_disable_reg = 1;
  resetFIFO();
}

void drvSIS3820::readScalers()
{
  int i;
  for (i=0; i<maxSignals_; i++) {
    scalerData_[i] = registers_->counter_regs[i];
  }
}

void drvSIS3820::resetScaler()
{
  /* Reset scaler */
  registers_->key_op_disable_reg = 1;
  resetFIFO();
  registers_->key_counter_clear = 1;
}


void drvSIS3820::clearScalerPresets()
{
  registers_->preset_channel_select_reg &= ~SIS3820_FOUR_BIT_MASK;
  registers_->preset_channel_select_reg &= ~(SIS3820_FOUR_BIT_MASK << 16);
  registers_->preset_enable_reg &= ~SIS3820_PRESET_STATUS_ENABLE_GROUP1;
  registers_->preset_enable_reg &= ~SIS3820_PRESET_STATUS_ENABLE_GROUP2;
  registers_->preset_group2_reg = 0;
  registers_->preset_group2_reg = 0;
}

void drvSIS3820::setScalerPresets()
{
  int i;
  epicsUInt32 presetChannelSelectRegister;
  int preset;

  if (acquireMode_ == SCALER_MODE) {
    /* Disable all presets to start */
    clearScalerPresets();

    /* The logic implemented in this code is that the last channel with a non-zero
     * preset value in a given channel group (1-16, 17-32) will be the one that is
     * in effect
     */
    for (i=0; i<maxSignals_; i++) {
      getIntegerParam(i, scalerPresets_, &preset);
      if (preset != 0) {
        if (i < 16) {
          registers_->preset_group1_reg = preset;
          /* Enable this bank of counters for preset checking */
          registers_->preset_enable_reg |= SIS3820_PRESET_STATUS_ENABLE_GROUP1;
          /* Set the correct channel for checking against the preset value */
          presetChannelSelectRegister = registers_->preset_channel_select_reg;
          presetChannelSelectRegister &= ~SIS3820_FOUR_BIT_MASK;
          presetChannelSelectRegister |= i;
          registers_->preset_channel_select_reg = presetChannelSelectRegister;
        } else {
          registers_->preset_group2_reg = preset;
          /* Enable this bank of counters for preset checking */
          registers_->preset_enable_reg |= SIS3820_PRESET_STATUS_ENABLE_GROUP2;
          /* Set the correct channel for checking against the preset value */
          presetChannelSelectRegister = registers_->preset_channel_select_reg;
          presetChannelSelectRegister &= ~(SIS3820_FOUR_BIT_MASK << 16);
          presetChannelSelectRegister |= (i << 16);
          registers_->preset_channel_select_reg = presetChannelSelectRegister;
        }
      }
    }
  }
}

void drvSIS3820::setAcquireMode(SIS38XXAcquireMode acquireMode)
{
  int i;
  int ch1RefEnable;
  static const char* functionName="setAcquireMode";
  
  getIntegerParam(SIS38XXCh1RefEnable_, &ch1RefEnable);
  
  epicsUInt32 channelDisableRegister = SIS3820_CHANNEL_DISABLE_MASK;
  
  /* Enable or disable 50 MHz channel 1 reference pulses. */
  if (ch1RefEnable > 0)
    registers_->control_status_reg |= CTRL_REFERENCE_CH1_ENABLE;
  else
    registers_->control_status_reg |= CTRL_REFERENCE_CH1_DISABLE;

  if (acquireMode_ == acquireMode) return;  /* Nothing to do */
  acquireMode_ = acquireMode;
  setIntegerParam(SIS38XXAcquireMode_, acquireMode);
  callParamCallbacks();

  
  /* Initialize board and set the control status register */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: initialising control status register\n",
            driverName, functionName);
  setControlStatusReg();
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: control status register = 0x%08x\n",
            driverName, functionName, registers_->control_status_reg);

  /* Set the interrupt control register */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: setting interrupt control register\n",
            driverName, functionName);
  setIrqControlStatusReg();
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: interrupt control register = 0x%08x\n",
            driverName, functionName, registers_->irq_control_status_reg);

  /* Set the operation mode of the scaler */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: setting operation mode\n",
            driverName, functionName);
  setOpModeReg();
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: operation mode register = 0x%08x\n",
            driverName, functionName, registers_->op_mode_reg);

  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: control status register = 0x%08x\n",
            driverName, functionName, registers_->control_status_reg);

  /* Trigger an interrupt when 1024 FIFO registers have been filled */
  registers_->fifo_word_threshold_reg = 1024;
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: FIFO threshold = %d\n",
            driverName, functionName, registers_->fifo_word_threshold_reg);

  /* Set the LNE channel */
  if (acquireMode_ == MCS_MODE) {
    registers_->lne_channel_select_reg = SIS3820_LNE_SOURCE_INTERNAL_10MHZ;
  } else {
    registers_->lne_channel_select_reg = SIS3820_LNE_CHANNEL;
  }
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: set LNE signal generation to channel %d\n",
            driverName, functionName, registers_->lne_channel_select_reg);

  /*
   * Set number of readout channels to maxSignals
   * Assumes that the lower channels will be used first, and the only unused
   * channels will be at the upper end of the channel range.
   * Create a mask with zeros in the rightmost maxSignals bits,
   * 1 in all higher order bits.
   */
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: setting readout channels=%d\n",
            driverName, functionName, maxSignals_);
  for (i = 0; i < maxSignals_; i++)
  {
    channelDisableRegister <<= 1;
  }
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: setting readout disable register=0x%08x\n",
            driverName, functionName, channelDisableRegister);

  /* Disable channel in MCS mode. */
  registers_->copy_disable_reg = channelDisableRegister;
  /* Disable channel in scaler mode. */
  registers_->count_disable_reg = channelDisableRegister;
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: setting copy disable register=0x%08x\n",
            driverName, functionName, registers_->copy_disable_reg);

  /* Set the prescale factor to the desired value. */
  registers_->lne_prescale_factor_reg = lnePrescale_ - 1;
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW,
            "%s:%s: lne prescale register=%d\n",
            driverName, functionName, registers_->lne_prescale_factor_reg);

  if (acquireMode_ == MCS_MODE) {
    /* Clear all presets from scaler mode */
    clearScalerPresets();
  } else {
    /* Clear the preset register */
    registers_->acq_preset_reg = 0;
  }
}

void drvSIS3820::setInputMode()
{
  int inputMode;

  getIntegerParam(SIS38XXInputMode_, &inputMode);

  /* Check that input mode is in the allowable range. If so, shift the mode
   * requested by the correct number of bits, and add to the register. */
  if (inputMode < 0 || inputMode > 5) inputMode = 0;

  registers_->op_mode_reg |= (inputMode << SIS3820_INPUT_MODE_SHIFT);
}

void drvSIS3820::setOutputMode()
{
  int outputMode;
  int maxOutputMode;
  int firmware;

  getIntegerParam(SIS38XXOutputMode_, &outputMode);
  getIntegerParam(SIS38XXFirmware_, &firmware);

  /* Check that output mode is in the allowable range. If so, shift the mode
   * requested by the correct number of bits, and add to the register. */
  if (firmware >= 0x010A) maxOutputMode = 3;
  else maxOutputMode = 2;
  if (outputMode < 0 || outputMode > maxOutputMode) outputMode = 0;

  registers_->op_mode_reg |= (outputMode << SIS3820_OUTPUT_MODE_SHIFT);
}


void drvSIS3820::setLED()
{
  int value;
  
  getIntegerParam(SIS38XXLED_, &value);
  if (value == 0)
    registers_->control_status_reg = CTRL_USER_LED_OFF;
  else
    registers_->control_status_reg = CTRL_USER_LED_ON;
}

int drvSIS3820::getLED()
{
  int value;
  
  value = registers_->control_status_reg & CTRL_USER_LED_OFF;
  return (value == 0) ? 0:1;
}

void drvSIS3820::setMuxOut()
{
  int value;
  int outputMode;
  int firmware;
  static const char *functionName="setMuxOut";
  
  getIntegerParam(SIS38XXMuxOut_, &value);
  getIntegerParam(SIS38XXOutputMode_, &outputMode);
  getIntegerParam(SIS38XXFirmware_, &firmware);
  if (outputMode != 3) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: scalerMuxOutCommand: output-mode %d does not support MUX_OUT.\n", 
              driverName, functionName, outputMode);
     return;
  }
  if (value < 1 || value > 32) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: scalerMuxOutCommand: channel %d is not supported on the board.\n", 
              driverName, functionName, value);
    return;
  }
  if (firmware < 0x010A) {
    asynPrint(pasynUserSelf, ASYN_TRACE_ERROR,
              "%s:%s: scalerMuxOutCommand: MUX_OUT is not supported in firmware version 0x%4.4X\n", 
              driverName, functionName, firmware);
    return;
  }
  registers_->mux_out_channel_select_reg = value - 1;
  asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
            "%s:%s: scalerMuxOutCommand %d\n", 
            driverName, functionName, value);
}

int drvSIS3820::getMuxOut()
{
  return registers_->mux_out_channel_select_reg + 1;
}



void drvSIS3820::setControlStatusReg()
{
  /* Set up the default behaviour of the card */
  /* Initially, this will have the following:
   * User LED off
   * Counter test modes disabled
   * Reference pulser enabled to Channel 1
   * LNE prescaler active
   */

  epicsUInt32 controlRegister = 0;

  controlRegister |= CTRL_USER_LED_OFF;
  controlRegister |= CTRL_COUNTER_TEST_25MHZ_DISABLE;
  controlRegister |= CTRL_COUNTER_TEST_MODE_DISABLE;
  /*controlRegister |= CTRL_REFERENCE_CH1_ENABLE;*/

  registers_->control_status_reg = controlRegister;
}


void drvSIS3820::setOpModeReg()
{
  /* Need to set this up to be accessed from asyn interface to allow changes on
   * the fly. Prior to that, I should split it out to allow individual changes
   * to be made from iocsh.
   */

  /* Set up the operation mode of the SIS3820.
   * FIFO emulation mode
   * Arm/enable mode LNE front panel
   * LNE sourced from front panel
   * 32 bit data format
   * Clearing mode - want the incremental counts
   */

  epicsUInt32 operationRegister = 0;

  operationRegister |= SIS3820_MCS_DATA_FORMAT_32BIT;
  operationRegister |= SIS3820_SCALER_DATA_FORMAT_32BIT;

  if (acquireMode_ == MCS_MODE) {
    operationRegister |= SIS3820_CLEARING_MODE;
    operationRegister |= SIS3820_ARM_ENABLE_CONTROL_SIGNAL;
    operationRegister |= SIS3820_FIFO_MODE;
    operationRegister |= SIS3820_OP_MODE_MULTI_CHANNEL_SCALER;
    if (lneSource_ == SIS3820_LNE_SOURCE_INTERNAL_10MHZ) {
      operationRegister |= SIS3820_LNE_SOURCE_INTERNAL_10MHZ;
    }
    if (lneSource_ == SIS3820_LNE_SOURCE_CONTROL_SIGNAL) {
      operationRegister |= SIS3820_LNE_SOURCE_CONTROL_SIGNAL;
    }
  } else {
    operationRegister |= SIS3820_NON_CLEARING_MODE;
    operationRegister |= SIS3820_LNE_SOURCE_VME;
    operationRegister |= SIS3820_ARM_ENABLE_CONTROL_SIGNAL;
    operationRegister |= SIS3820_FIFO_MODE;
    operationRegister |= SIS3820_HISCAL_START_SOURCE_VME;
    operationRegister |= SIS3820_OP_MODE_SCALER;
  }
  registers_->op_mode_reg = operationRegister;
}

void drvSIS3820::setIrqControlStatusReg()
{
  /* Set the desired interrupts. The SIS3820 can generate interrupts on
   * the following conditions:
   * 0 = LNE/clock shadow
   * 1 = FIFO threshold
   * 2 = Acquisition completed
   * 3 = Overflow
   * 4 = FIFO almost full
   */
  epicsUInt32 interruptRegister = 0;

  /* This register is set up the same for MCS_MODE and SCALER_MODE) */

  interruptRegister |= SIS3820_IRQ_SOURCE0_DISABLE;
  /* NOTE: WE ARE NOT USING FIFO THRESHOLD INTERRUPTS FOR NOW */
  interruptRegister |= SIS3820_IRQ_SOURCE1_DISABLE;
  interruptRegister |= SIS3820_IRQ_SOURCE2_ENABLE;
  interruptRegister |= SIS3820_IRQ_SOURCE3_DISABLE;
  interruptRegister |= SIS3820_IRQ_SOURCE4_ENABLE;
  interruptRegister |= SIS3820_IRQ_SOURCE5_DISABLE;
  interruptRegister |= SIS3820_IRQ_SOURCE6_DISABLE;
  interruptRegister |= SIS3820_IRQ_SOURCE7_DISABLE;

  interruptRegister |= SIS3820_IRQ_SOURCE0_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE1_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE2_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE3_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE4_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE5_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE6_CLEAR;
  interruptRegister |= SIS3820_IRQ_SOURCE7_CLEAR;

  registers_->irq_control_status_reg = interruptRegister;
}

/**********************/
/* Interrupt handling */
/**********************/

void drvSIS3820::enableInterrupts()
{
 registers_->irq_config_reg &= ~SIS3820_IRQ_ENABLE;
}

void drvSIS3820::disableInterrupts()
{
  registers_->irq_config_reg |= SIS3820_IRQ_ENABLE;
}


static void intFuncC(void *drvPvt)
{
  drvSIS3820 *pSIS3820 = (drvSIS3820*)drvPvt;
  pSIS3820->intFunc();
}
  

void drvSIS3820::intFunc()
{
  /* Disable interrupts */
  registers_->irq_config_reg &= ~SIS3820_IRQ_ENABLE;

  /* Test which interrupt source has triggered this interrupt. */
  irqStatusReg_ = registers_->irq_control_status_reg;

  /* Check for the FIFO threshold interrupt */
  if (irqStatusReg_ & SIS3820_IRQ_SOURCE1_FLAG)
  {
    /* Note that this is a level-sensitive interrupt, not edge sensitive, so it can't be cleared */
    /* Disable this interrupt, since it is caused by FIFO threshold, and that
     * condition is only cleared in the readFIFO routine */
    registers_->irq_control_status_reg = SIS3820_IRQ_SOURCE1_DISABLE;
  }

  /* Check for the data acquisition complete interrupt */
  else if (irqStatusReg_ & SIS3820_IRQ_SOURCE2_FLAG)
  {
    /* Reset the interrupt source */
    registers_->irq_control_status_reg = SIS3820_IRQ_SOURCE2_CLEAR;
    acquiring_ = false;
  }

  /* Check for the FIFO almost full interrupt */
  else if (irqStatusReg_ & SIS3820_IRQ_SOURCE4_FLAG)
  {
    /* This interrupt represents an error condition of sorts. For the moment, I
     * will terminate data collection, as it is likely that data will have been
     * lost.
     * Note that this is a level-sensitive interrupt, not edge sensitive, so it can't be cleared.
     * Instead we disable the interrupt, and re-enable it at the end of readFIFO.
     */
    registers_->irq_control_status_reg = SIS3820_IRQ_SOURCE4_DISABLE;
  }

  /* Send an event to intTask to read the FIFO and perform any requested callbacks */
  epicsEventSignal(readFIFOEventId_);

  /* Reenable interrupts */
  registers_->irq_config_reg |= SIS3820_IRQ_ENABLE;

}

void drvSIS3820::resetFIFO()
{
  epicsMutexLock(fifoLockId_);
  registers_->key_fifo_reset_reg= 1;
  epicsMutexUnlock(fifoLockId_);
}  

void readFIFOThreadC(void *drvPvt)
{
  drvSIS3820 *pSIS3820 = (drvSIS3820*)drvPvt;
  pSIS3820->readFIFOThread();
}

/** This thread is woken up by an interrupt or a request to read status
  * It loops calling readFIFO until acquiring_ goes to false.
  * readFIFO only reads a limited amount of FIFO data at once in order
  * to avoid blocking the device support threads. */
void drvSIS3820::readFIFOThread()
{
  bool acquiring;
  int status;
  int count;
  int signal;
  int chan;
  int i;
  epicsUInt32 *pIn, *pOut;
  epicsTimeStamp t1, t2, t3;
  static const char* functionName="readFIFOThread";

  while(true)
  {
    epicsEventWait(readFIFOEventId_);
    // We got an event.  This can come from:
    //  Acquisition complete in scaler mode
    //  FIFO full in MCS mode
    //  Acquisition start in MCS mode
    // For scaler mode we just do callbacks
    lock();
    // Do callbacks on acquiring status when acquisition completes
    if ((acquiring_ == false) && (acquireMode_ == SCALER_MODE)) {
      setIntegerParam(scalerDone_, 1);
      callParamCallbacks();
      unlock();
      continue;
    }
    unlock();
    // MCS mode
    acquiring = true;
    while (acquiring) {
      disableInterrupts();
      lock();
      signal = nextSignal_;
      chan = nextChan_;
      // This block of code can be slow and does not require the asynPortDriver lock because we are not
      // accessing object data that could change.  
      // It does require the FIFO lock so no one resets the FIFO while it executes
      epicsMutexLock(fifoLockId_);
      unlock();
      count = registers_->fifo_word_count_reg;
      if (count > fifoBufferWords_) count = fifoBufferWords_;
      epicsTimeGetCurrent(&t1);
      if (useDma_ && (count >= MIN_DMA_TRANSFERS)) {
        asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                  "%s:%s: doing DMA transfer, fifoBuffer=%p, fifo_base=%p, count=%d\n",
                  driverName, functionName, fifoBuffer_, fifo_base_, count);
        status = sysDmaFromVme(dmaId_, fifoBuffer_, (int)fifo_base_, VME_AM_EXT_SUP_D64BLT, (count)*sizeof(int), 8);
        if (status) {
          asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                    "%s:%s: doing DMA transfer, error calling sysDmaFromVme, status=%d, buff=%p, fifo_base=%p, count=%d\n",
                    driverName, functionName, status, fifoBuffer_, fifo_base_, count);
        } 
        else {
          epicsEventWait(dmaDoneEventId_);
          status = sysDmaStatus(dmaId_);
          if (status)
             asynPrint(pasynUserSelf, ASYN_TRACE_ERROR, 
                       "%s:%s: DMA error, errno=%d, message=%s\n",
                       driverName, functionName, errno, strerror(errno));
        }
      } else {    
        asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                  "%s:%s: memcpy transfer, count=%d\n",
                  driverName, functionName, count);
        memcpy(fifoBuffer_, fifo_base_, count*sizeof(int));
      }
      epicsTimeGetCurrent(&t2);

      asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s:%s: read FIFO (%d) in %fs, fifo word count after=%d\n",
                driverName, functionName, count, epicsTimeDiffInSeconds(&t2, &t1), registers_->fifo_word_count_reg);
      // Release the FIFO lock, we are done accessing the FIFO
      epicsMutexUnlock(fifoLockId_);
      
      // Copy the data from the FIFO buffer to the mcsBuffer
      pOut = mcsData_ + signal*maxChans_ + chan;
      pIn = fifoBuffer_;
      for (i=0; i<count; i++) {
        *pOut = *pIn++;
        signal++;
        if (signal == maxSignals_) {
          signal = 0;
          chan++;
          pOut = mcsData_ + chan;
        } else {
          pOut += maxChans_;
        }
      }
      
      epicsTimeGetCurrent(&t2);
      // Take the lock since we are now changing object data
      lock();
      nextChan_ = chan;
      nextSignal_ = signal;
      asynPrint(pasynUserSelf, ASYN_TRACE_FLOW, 
                "%s:%s: copied data to mcsBuffer in %fs, nextChan=%d, nextSignal=%d\n",
                driverName, functionName, epicsTimeDiffInSeconds(&t3, &t3), nextChan_, nextSignal_);

      acquiring = checkDone(registers_->fifo_word_count_reg);
      /* Re-enable FIFO threshold and FIFO almost full interrupts */
      /* NOTE: WE ARE NOT USING FIFO THRESHOLD INTERRUPTS FOR NOW */
      /* registers_->irq_control_status_reg = SIS3820_IRQ_SOURCE1_ENABLE; */
      registers_->irq_control_status_reg = SIS3820_IRQ_SOURCE4_ENABLE;

      // Release the lock and sleep for a short time
      unlock();
      enableInterrupts();
      epicsThreadSleep(epicsThreadSleepQuantum());
    }
  }
}

extern "C" {
int drvSIS3820Config(const char *portName, int baseAddress, int interruptVector, int interruptLevel, 
                     int maxChans, int maxSignals, int inputMode, int outputMode, 
                     int useDma, int fifoBufferWords)
{
  drvSIS3820 *pSIS3820 = new drvSIS3820(portName, baseAddress, interruptVector, interruptLevel,
                                        maxChans, maxSignals, inputMode, outputMode, 
                                        useDma, fifoBufferWords);
  pSIS3820 = NULL;
  return 0;
}

/* iocsh config function */
static const iocshArg drvSIS3820ConfigArg0 = { "Asyn port name",   iocshArgString};
static const iocshArg drvSIS3820ConfigArg1 = { "Base address",     iocshArgInt};
static const iocshArg drvSIS3820ConfigArg2 = { "Interrupt vector", iocshArgInt};
static const iocshArg drvSIS3820ConfigArg3 = { "Interrupt level",  iocshArgInt};
static const iocshArg drvSIS3820ConfigArg4 = { "MaxChannels",      iocshArgInt};
static const iocshArg drvSIS3820ConfigArg5 = { "MaxSignals",       iocshArgInt};
static const iocshArg drvSIS3820ConfigArg6 = { "InputMode",        iocshArgInt};
static const iocshArg drvSIS3820ConfigArg7 = { "OutputMode",       iocshArgInt};
static const iocshArg drvSIS3820ConfigArg8 = { "Use DMA",          iocshArgInt};
static const iocshArg drvSIS3820ConfigArg9 = { "FIFO buffer words", iocshArgInt};

static const iocshArg * const drvSIS3820ConfigArgs[] = 
{ &drvSIS3820ConfigArg0,
  &drvSIS3820ConfigArg1,
  &drvSIS3820ConfigArg2,
  &drvSIS3820ConfigArg3,
  &drvSIS3820ConfigArg4,
  &drvSIS3820ConfigArg5,
  &drvSIS3820ConfigArg6,
  &drvSIS3820ConfigArg7,
  &drvSIS3820ConfigArg8,
  &drvSIS3820ConfigArg9
};

static const iocshFuncDef drvSIS3820ConfigFuncDef = 
  {"drvSIS3820Config",10,drvSIS3820ConfigArgs};

static void drvSIS3820ConfigCallFunc(const iocshArgBuf *args)
{
  drvSIS3820Config(args[0].sval, args[1].ival, args[2].ival, args[3].ival,
                   args[4].ival, args[5].ival, args[6].ival, args[7].ival,
                   args[8].ival, args[9].ival);
}

void drvSIS3820Register(void)
{
  iocshRegister(&drvSIS3820ConfigFuncDef,drvSIS3820ConfigCallFunc);
}

epicsExportRegistrar(drvSIS3820Register);

} // extern "C"


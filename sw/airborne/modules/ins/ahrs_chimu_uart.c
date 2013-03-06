/*
  C code to connect a CHIMU using uart
*/


#include <stdbool.h>

// Output
#include "state.h"

// For centripedal corrections
#include "subsystems/gps.h"
#include "subsystems/ahrs.h"

#include "generated/airframe.h"

#if CHIMU_DOWNLINK_IMMEDIATE
#ifndef DOWNLINK_DEVICE
#define DOWNLINK_DEVICE DOWNLINK_AP_DEVICE
#endif
#include "mcu_periph/uart.h"
#include "downlink_msg.h"
#include "subsystems/datalink/downlink.h"
#endif

#include "ins_module.h"
#include "imu_chimu.h"

#include "led.h"

CHIMU_PARSER_DATA CHIMU_DATA;

INS_FORMAT ins_roll_neutral;
INS_FORMAT ins_pitch_neutral;

void ahrs_init(void)
{
  ahrs.status = AHRS_UNINIT;

  uint8_t ping[7] = {CHIMU_STX, CHIMU_STX, 0x01, CHIMU_BROADCAST, MSG00_PING, 0x00, 0xE6 };
  uint8_t rate[12] = {CHIMU_STX, CHIMU_STX, 0x06, CHIMU_BROADCAST, MSG10_UARTSETTINGS, 0x05, 0xff, 0x79, 0x00, 0x00, 0x01, 0x76 };	// 50Hz attitude only + SPI
  uint8_t quaternions[7] = {CHIMU_STX, CHIMU_STX, 0x01, CHIMU_BROADCAST, MSG09_ESTIMATOR, 0x01, 0x39 }; // 25Hz attitude only + SPI
  //  uint8_t rate[12] = {CHIMU_STX, CHIMU_STX, 0x06, CHIMU_BROADCAST, MSG10_UARTSETTINGS, 0x04, 0xff, 0x79, 0x00, 0x00, 0x01, 0xd3 }; // 25Hz attitude only + SPI
  //  uint8_t euler[7] = {CHIMU_STX, CHIMU_STX, 0x01, CHIMU_BROADCAST, MSG09_ESTIMATOR, 0x00, 0xaf }; // 25Hz attitude only + SPI

  new_ins_attitude = 0;

  ins_roll_neutral = INS_ROLL_NEUTRAL_DEFAULT;
  ins_pitch_neutral = INS_PITCH_NEUTRAL_DEFAULT;

  CHIMU_Init(&CHIMU_DATA);

  // Request Software version
  for (int i=0;i<7;i++) {
    InsUartSend1(ping[i]);
  }

  // Quat Filter
  for (int i=0;i<7;i++) {
    InsUartSend1(quaternions[i]);
  }

  // 50Hz
  CHIMU_Checksum(rate,12);
  InsSend(rate,12);
}
void ahrs_align(void)
{
  ahrs.status = AHRS_RUNNING;
}


void parse_ins_msg( void )
{
  while (InsLink(ChAvailable())) {
    uint8_t ch = InsLink(Getch());

    if (CHIMU_Parse(ch, 0, &CHIMU_DATA)) {
      if(CHIMU_DATA.m_MsgID==0x03) {
        new_ins_attitude = 1;
        RunOnceEvery(25, LED_TOGGLE(3) );
        if (CHIMU_DATA.m_attitude.euler.phi > M_PI) {
          CHIMU_DATA.m_attitude.euler.phi -= 2 * M_PI;
        }

        struct FloatEulers att = {
          CHIMU_DATA.m_attitude.euler.phi,
          CHIMU_DATA.m_attitude.euler.theta,
          CHIMU_DATA.m_attitude.euler.psi
        };
        stateSetNedToBodyEulers_f(&att);
#if CHIMU_DOWNLINK_IMMEDIATE
        float foo = 0;
        DOWNLINK_SEND_ATTITUDE_EULER(DefaultChannel, DefaultDevice, &CHIMU_DATA.m_attitude.euler.phi, &CHIMU_DATA.m_attitude.euler.theta, &CHIMU_DATA.m_attitude.euler.psi, &foo, &foo, &foo);
#endif

      }
    }
  }
}

void ahrs_update_gps( void )
{
}

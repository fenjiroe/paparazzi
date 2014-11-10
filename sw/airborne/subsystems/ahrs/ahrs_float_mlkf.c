/*
 * Copyright (C) 2011-2012  Antoine Drouin <poinix@gmail.com>
 * Copyright (C) 2013       Felix Ruess <felix.ruess@gmail.com>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * @file subsystems/ahrs/ahrs_float_mlkf.c
 *
 * Multiplicative linearized Kalman Filter in quaternion formulation.
 *
 * Estimate the attitude, heading and gyro bias.
 */

#include "subsystems/ahrs/ahrs_float_mlkf.h"
#include "subsystems/ahrs/ahrs_float_utils.h"

#include <float.h>   /* for FLT_MIN     */
#include <string.h>  /* for memcpy      */
#include <math.h>    /* for M_PI        */

#include "state.h"

#include "math/pprz_algebra_float.h"
#include "math/pprz_algebra_int.h"
#include "math/pprz_simple_matrix.h"
#include "generated/airframe.h"

#include "subsystems/abi.h"

//#include <stdio.h>

#ifndef AHRS_MAG_NOISE_X
#define AHRS_MAG_NOISE_X 0.2
#define AHRS_MAG_NOISE_Y 0.2
#define AHRS_MAG_NOISE_Z 0.2
#endif

static inline void propagate_ref(struct Int32Rates* gyro, float dt);
static inline void propagate_state(float dt);
static inline void update_state(const struct FloatVect3 *i_expected, struct FloatVect3* b_measured, struct FloatVect3* noise);
static inline void reset_state(void);
static inline void set_body_state_from_quat(void);

struct AhrsMlkf ahrs_mlkf;

#if PERIODIC_TELEMETRY
#include "subsystems/datalink/telemetry.h"

static void send_geo_mag(struct transport_tx *trans, struct link_device *dev) {
  pprz_msg_send_GEO_MAG(trans, dev, AC_ID,
                        &ahrs_mlkf.mag_h.x, &ahrs_mlkf.mag_h.y, &ahrs_mlkf.mag_h.z);
}
#endif


/** ABI binding for IMU data.
 * Used for gyro, accel and mag ABI messages.
 */
#ifndef AHRS_MLKF_IMU_ID
#define AHRS_MLKF_IMU_ID ABI_BROADCAST
#endif
static abi_event gyro_ev;
static abi_event accel_ev;
static abi_event mag_ev;

static abi_event aligner_ev;

static void gyro_cb(uint8_t __attribute__((unused)) sender_id, const uint32_t* stamp,
                    const struct Int32Rates* gyro)
{
#if USE_AUTO_AHRS_FREQ || !defined(AHRS_PROPAGATE_FREQUENCY)
PRINT_CONFIG_MSG("Calculating dt for AHRS_MLKF propagation.")
  /* timestamp in usec when last callback was received */
  static uint32_t last_stamp = 0;

  if (last_stamp > 0 && ahrs_mlkf.status == AHRS_MLKF_RUNNING) {
    float dt = (float)(*stamp - last_stamp) * 1e-6;
    ahrs_mlkf_propagate((struct Int32Rates*)gyro, dt);
  }
  last_stamp = *stamp;
#else
PRINT_CONFIG_MSG("Using fixed AHRS_PROPAGATE_FREQUENCY for AHRS_MLKF propagation.")
PRINT_CONFIG_VAR(AHRS_PROPAGATE_FREQUENCY)
  if (ahrs_mlkf.status == AHRS_MLKF_RUNNING) {
    const float dt = 1. / (AHRS_PROPAGATE_FREQUENCY);
    ahrs_mlkf_propagate((struct Int32Rates*)gyro, dt);
  }
#endif
}

static void accel_cb(uint8_t sender_id __attribute__((unused)),
                     const uint32_t* stamp __attribute__((unused)),
                     const struct Int32Vect3* accel)
{
  if (ahrs_mlkf.status == AHRS_MLKF_RUNNING) {
    ahrs_mlkf_update_accel((struct Int32Vect3*)accel);
  }
}

static void mag_cb(uint8_t sender_id __attribute__((unused)),
                   const uint32_t* stamp __attribute__((unused)),
                   const struct Int32Vect3* mag)
{
  if (ahrs_mlkf.status == AHRS_MLKF_RUNNING) {
    ahrs_mlkf_update_mag((struct Int32Vect3*)mag);
  }
}

static void aligner_cb(uint8_t __attribute__((unused)) sender_id,
                       const uint32_t* stamp __attribute__((unused)),
                       const struct Int32Rates* lp_gyro, const struct Int32Vect3* lp_accel,
                       const struct Int32Vect3* lp_mag)
{
  if (ahrs_mlkf.status != AHRS_MLKF_RUNNING) {
    ahrs_mlkf_align((struct Int32Rates*)lp_gyro, (struct Int32Vect3*)lp_accel,
                    (struct Int32Vect3*)lp_mag);
  }
}


void ahrs_mlkf_register(void)
{
  ahrs_register_impl(ahrs_mlkf_init, NULL);
}

void ahrs_mlkf_init(struct OrientationReps* body_to_imu) {

  /* save body_to_imu pointer */
  ahrs_mlkf.body_to_imu = body_to_imu;

  ahrs_mlkf.status = AHRS_MLKF_UNINIT;

  /* Set ltp_to_imu so that body is zero */
  memcpy(&ahrs_mlkf.ltp_to_imu_quat, orientationGetQuat_f(ahrs_mlkf.body_to_imu),
         sizeof(struct FloatQuat));

  FLOAT_RATES_ZERO(ahrs_mlkf.imu_rate);

  VECT3_ASSIGN(ahrs_mlkf.mag_h, AHRS_H_X, AHRS_H_Y, AHRS_H_Z);

  /*
   * Initialises our state
   */
  FLOAT_RATES_ZERO(ahrs_mlkf.gyro_bias);
  const float P0_a = 1.;
  const float P0_b = 1e-4;
  float P0[6][6] = {{ P0_a, 0.,   0.,   0.,   0.,   0.  },
                    { 0.,   P0_a, 0.,   0.,   0.,   0.  },
                    { 0.,   0.,   P0_a, 0.,   0.,   0.  },
                    { 0.,   0.,   0.,   P0_b, 0.,   0.  },
                    { 0.,   0.,   0.,   0.,   P0_b, 0.  },
                    { 0.,   0.,   0.,   0.,   0.,   P0_b}};
  memcpy(ahrs_mlkf.P, P0, sizeof(P0));

  VECT3_ASSIGN(ahrs_mlkf.mag_noise, AHRS_MAG_NOISE_X, AHRS_MAG_NOISE_Y, AHRS_MAG_NOISE_Z);

  /*
   * Subscribe to scaled IMU measurements and attach callbacks
   */
  AbiBindMsgIMU_GYRO_INT32(AHRS_MLKF_IMU_ID, &gyro_ev, gyro_cb);
  AbiBindMsgIMU_ACCEL_INT32(AHRS_MLKF_IMU_ID, &accel_ev, accel_cb);
  AbiBindMsgIMU_MAG_INT32(AHRS_MLKF_IMU_ID, &mag_ev, mag_cb);
  AbiBindMsgIMU_LOWPASSED(ABI_BROADCAST, &aligner_ev, aligner_cb);

#if PERIODIC_TELEMETRY
  register_periodic_telemetry(DefaultPeriodic, "GEO_MAG", send_geo_mag);
#endif
}

bool_t ahrs_mlkf_align(struct Int32Rates* lp_gyro, struct Int32Vect3* lp_accel,
                       struct Int32Vect3* lp_mag)
{

  /* Compute an initial orientation from accel and mag directly as quaternion */
  ahrs_float_get_quat_from_accel_mag(&ahrs_mlkf.ltp_to_imu_quat, lp_accel, lp_mag);

  /* set initial body orientation */
  set_body_state_from_quat();

  /* used averaged gyro as initial value for bias */
  struct Int32Rates bias0;
  RATES_COPY(bias0, *lp_gyro);
  RATES_FLOAT_OF_BFP(ahrs_mlkf.gyro_bias, bias0);

  ahrs_mlkf.status = AHRS_MLKF_RUNNING;

  return TRUE;
}

void ahrs_mlkf_propagate(struct Int32Rates* gyro, float dt) {
  propagate_ref(gyro, dt);
  propagate_state(dt);
  set_body_state_from_quat();
}

void ahrs_mlkf_update_accel(struct Int32Vect3* accel) {
  struct FloatVect3 imu_g;
  ACCELS_FLOAT_OF_BFP(imu_g, *accel);
  const float alpha = 0.92;
  ahrs_mlkf.lp_accel = alpha * ahrs_mlkf.lp_accel +
    (1. - alpha) *(float_vect3_norm(&imu_g) - 9.81);
  const struct FloatVect3 earth_g = {0.,  0., -9.81 };
  const float dn = 250*fabs( ahrs_mlkf.lp_accel );
  struct FloatVect3 g_noise = {1.+dn, 1.+dn, 1.+dn};
  update_state(&earth_g, &imu_g, &g_noise);
  reset_state();
}


void ahrs_mlkf_update_mag(struct Int32Vect3* mag) {
  struct FloatVect3 imu_h;
  MAGS_FLOAT_OF_BFP(imu_h, *mag);
  update_state(&ahrs_mlkf.mag_h, &imu_h, &ahrs_mlkf.mag_noise);
  reset_state();
}


static inline void propagate_ref(struct Int32Rates* gyro, float dt) {
  /* converts gyro to floating point */
  struct FloatRates gyro_float;
  RATES_FLOAT_OF_BFP(gyro_float, *gyro);

  /* unbias measurement */
  RATES_SUB(gyro_float, ahrs_mlkf.gyro_bias);

#ifdef AHRS_PROPAGATE_LOW_PASS_RATES
  /* lowpass angular rates */
  const float alpha = 0.1;
  FLOAT_RATES_LIN_CMB(ahrs_mlkf.imu_rate, ahrs_mlkf.imu_rate,
                      (1.-alpha), gyro_float, alpha);
#else
  RATES_COPY(ahrs_mlkf.imu_rate, gyro_float);
#endif

  /* propagate reference quaternion */
  float_quat_integrate(&ahrs_mlkf.ltp_to_imu_quat, &ahrs_mlkf.imu_rate, dt);

}

/**
 * Progagate filter's covariance
 * We don't propagate state as we assume to have reseted
 */
static inline void propagate_state(float dt) {

  /* predict covariance */
  const float dp = ahrs_mlkf.imu_rate.p*dt;
  const float dq = ahrs_mlkf.imu_rate.q*dt;
  const float dr = ahrs_mlkf.imu_rate.r*dt;

  float F[6][6] = {{  1.,   dr,  -dq,  -dt,   0.,   0.  },
                   { -dr,   1.,   dp,   0.,  -dt,   0.  },
                   {  dq,  -dp,   1.,   0.,   0.,  -dt  },
                   {  0.,   0.,   0.,   1.,   0.,   0.  },
                   {  0.,   0.,   0.,   0.,   1.,   0.  },
                   {  0.,   0.,   0.,   0.,   0.,   1.  }};
  // P = FPF' + GQG
  float tmp[6][6];
  MAT_MUL(6,6,6, tmp, F, ahrs_mlkf.P);
  MAT_MUL_T(6,6,6,  ahrs_mlkf.P, tmp, F);
  const float dt2 = dt * dt;
  const float GQG[6] = {dt2*10e-3, dt2*10e-3, dt2*10e-3, dt2*9e-6, dt2*9e-6, dt2*9e-6 };
  for (int i=0;i<6;i++)
    ahrs_mlkf.P[i][i] += GQG[i];

}


/**
 *  Incorporate one 3D vector measurement
 */
static inline void update_state(const struct FloatVect3 *i_expected, struct FloatVect3* b_measured, struct FloatVect3* noise) {

  /* converted expected measurement from inertial to body frame */
  struct FloatVect3 b_expected;
  float_quat_vmult(&b_expected, &ahrs_mlkf.ltp_to_imu_quat, (struct FloatVect3*)i_expected);

  // S = HPH' + JRJ
  float H[3][6] = {{           0., -b_expected.z,  b_expected.y, 0., 0., 0.},
                   { b_expected.z,            0., -b_expected.x, 0., 0., 0.},
                   {-b_expected.y,  b_expected.x,            0., 0., 0., 0.}};
  float tmp[3][6];
  MAT_MUL(3,6,6, tmp, H, ahrs_mlkf.P);
  float S[3][3];
  MAT_MUL_T(3,6,3, S, tmp, H);

  /* add the measurement noise */
  S[0][0] += noise->x;
  S[1][1] += noise->y;
  S[2][2] += noise->z;

  float invS[3][3];
  MAT_INV33(invS, S);

  // K = PH'invS
  float tmp2[6][3];
  MAT_MUL_T(6,6,3, tmp2, ahrs_mlkf.P, H);
  float K[6][3];
  MAT_MUL(6,3,3, K, tmp2, invS);

  // P = (I-KH)P
  float tmp3[6][6];
  MAT_MUL(6,3,6, tmp3, K, H);
  float I6[6][6] = {{ 1., 0., 0., 0., 0., 0. },
                    {  0., 1., 0., 0., 0., 0. },
                    {  0., 0., 1., 0., 0., 0. },
                    {  0., 0., 0., 1., 0., 0. },
                    {  0., 0., 0., 0., 1., 0. },
                    {  0., 0., 0., 0., 0., 1. }};
  float tmp4[6][6];
  MAT_SUB(6,6, tmp4, I6, tmp3);
  float tmp5[6][6];
  MAT_MUL(6,6,6, tmp5, tmp4, ahrs_mlkf.P);
  memcpy(ahrs_mlkf.P, tmp5, sizeof(ahrs_mlkf.P));

  // X = X + Ke
  struct FloatVect3 e;
  VECT3_DIFF(e, *b_measured, b_expected);
  ahrs_mlkf.gibbs_cor.qx  += K[0][0]*e.x + K[0][1]*e.y + K[0][2]*e.z;
  ahrs_mlkf.gibbs_cor.qy  += K[1][0]*e.x + K[1][1]*e.y + K[1][2]*e.z;
  ahrs_mlkf.gibbs_cor.qz  += K[2][0]*e.x + K[2][1]*e.y + K[2][2]*e.z;
  ahrs_mlkf.gyro_bias.p  += K[3][0]*e.x + K[3][1]*e.y + K[3][2]*e.z;
  ahrs_mlkf.gyro_bias.q  += K[4][0]*e.x + K[4][1]*e.y + K[4][2]*e.z;
  ahrs_mlkf.gyro_bias.r  += K[5][0]*e.x + K[5][1]*e.y + K[5][2]*e.z;

}


/**
 * Incorporate errors to reference and zeros state
 */
static inline void reset_state(void) {

  ahrs_mlkf.gibbs_cor.qi = 2.;
  struct FloatQuat q_tmp;
  float_quat_comp(&q_tmp, &ahrs_mlkf.ltp_to_imu_quat, &ahrs_mlkf.gibbs_cor);
  float_quat_normalize(&q_tmp);
  memcpy(&ahrs_mlkf.ltp_to_imu_quat, &q_tmp, sizeof(ahrs_mlkf.ltp_to_imu_quat));
  float_quat_identity(&ahrs_mlkf.gibbs_cor);

}

/**
 * Compute body orientation and rates from imu orientation and rates
 */
static inline void set_body_state_from_quat(void) {
  struct FloatQuat *body_to_imu_quat = orientationGetQuat_f(ahrs_mlkf.body_to_imu);
  struct FloatRMat *body_to_imu_rmat = orientationGetRMat_f(ahrs_mlkf.body_to_imu);

  /* Compute LTP to BODY quaternion */
  struct FloatQuat ltp_to_body_quat;
  float_quat_comp_inv(&ltp_to_body_quat, &ahrs_mlkf.ltp_to_imu_quat, body_to_imu_quat);
  /* Set in state interface */
  stateSetNedToBodyQuat_f(&ltp_to_body_quat);

  /* compute body rates */
  struct FloatRates body_rate;
  float_rmat_transp_ratemult(&body_rate, body_to_imu_rmat, &ahrs_mlkf.imu_rate);
  /* Set state */
  stateSetBodyRates_f(&body_rate);

}

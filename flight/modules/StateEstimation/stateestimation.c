/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup State Estimation
 * @brief Acquires sensor data and computes state estimate
 * @{
 *
 * @file       stateestimation.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2013.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

// TODO goes in header


typedef enum {
    gyr_UPDATED     = 1 << 0,
        acc_UPDATED = 1 << 1,
        mag_UPDATED = 1 << 2,
        pos_UPDATED = 1 << 3,
        vel_UPDATED = 1 << 4,
        bar_UPDATED = 1 << 5,
        ias_UPDATED = 1 << 6
} sensorUpdates;

typedef struct {
    float gyr[3];
    float acc[3];
    float mag[3];
    float pos[3];
    float vel[3];
    float bar[1];
    float ias[1];
    sensorUpdates updated;
} stateEstimation;

#define ISSET(bitfield, bit) ((bitfield) & (bit) ? 1 : 0)
#define UNSET(bitfield, bit) (bitfield) &= ~(bit)


typedef struct stateFilterStruct {
    int32_t (*init)(void);
    int32_t (*update)(stateEstimation *state);
} stateFilter;

struct filterQueueStruct;
typedef struct filterQueueStruct {
    stateFilter filter;
    struct filterQueueStruct *next;
} filtereQueue;

//

#include <openpilot.h>
#include <gyrosensor.h>
#include <accelsensor.h>
#include <magnetosensor.h>
#include <barosensor.h>
#include <airspeedsensor.h>
#include <gpsposition.h>
#include <gpsvelocity.h>

#include "revosettings.h"
#include "homelocation.h"

#include "CoordinateConversions.h"

// Private constants
#define STACK_SIZE_BYTES  2048
#define CALLBACK_PRIORITY CALLBACK_PRIORITY_HIGH
#define TASK_PRIORITY     CALLBACK_TASKPRIORITY_FLIGHTCONTROL
#define TIMEOUT_MS        100

// Private types

// Private variables
static xTaskHandle attitudeTaskHandle;
static DelayedCallbackInfo *stateEstimationCallback;

static RevoSettingsData revoSettings;
static HomeLocationData homeLocation;
static float LLA2NEDM[3];
static volatile sensorUpdates updatedSensors;

// Private functions

static void settingsUpdatedCb(UAVObjEvent *objEv);
static void sensorUpdatedCb(UAVObjEvent *objEv);
static void StateEstimationCb(void);
static int32_t getNED(GPSPositionData *gpsPosition, float *NED);


/**
 * Initialise the module.  Called before the start function
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t StateEstimationInitialize(void)
{
    RevoSettingsInitialize();

    RevoSettingsConnectCallback(&settingsUpdatedCb);
    HomeLocationConnectCallback(&settingsUpdatedCb);

    GyroSensorConnectCallback(&sensorUpdatedCb);
    AccelSensorConnectCallback(&sensorUpdatedCb);
    MagnetoSensorConnectCallback(&sensorUpdatedCb);
    BaroSensorConnectCallback(&sensorUpdatedCb);
    AirspeedSensorConnectCallback(&sensorUpdatedCb);
    GPSPositionConnectCallback(&sensorUpdatedCb);
    GPSVelocityConnectCallback(&sensorUpdatedCb);

    stateEstimationCallback = DelayedCallbackCreate(&StateEstimationCb, CALLBACK_PRIORITY, TASK_PRIORITY, STACK_SIZE_BYTES);

    return 0;
}

/**
 * Start the task.  Expects all objects to be initialized by this point.
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t StateEstimationStart(void)
{
    RevoSettingsConnectCallback(&settingsUpdatedCb);

    // Force settings update to make sure rotation loaded
    settingsUpdatedCb(NULL);

    // Initialize Filters
    stateFilter magFilter   = filterMagInitialize();
    stateFilter baroFilter  = filterBaroInitialize();
    stateFilter stationaryFilter = filterStationaryInitialize();
    stateFilter cfFilter    = filterCFInitialize();
    stateFilter cfmFilter   = filterCFMInitialize();
    stateFilter ekf13Filter = filterEKF13Initialize();
    stateFilter ekf16Filter = filterEKF16Initialize();


    return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

/**
 * Module callback
 */
static void StateEstimationCb(void)
{
    // alarms flag
    uint8_t alarm = 0;

    // set alarm to warning if called through timeout
    if (updatedSensor == 0) {
        AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_WARNING);
        alarm = 1;
    }

    // read updated sensor UAVObjects and set initial state
    stateEstimation sensors;
    sensors.updated = updatedSensors;
    updatedSensors ^= sensors.updated;

    // most sensors get only rudimentary sanity checks
        #define SANITYCHECK3(sensorname, shortname, a1, a2, a3) \
    if (ISSET(sensors.updated, shortname##_UPDATED)) { \
        sensorname##Data s; \
        sensorname##GET(&s); \
        if (sane(s.a1) && sane(s.a2) && sane(s.a3)) { \
            sensors.shortname[0] = s.a1; \
            sensors.shortname[1] = s.a2; \
            sensors.shortname[2] = s.a3; \
        } \
        else { \
            UNSET(sensors.updated, shortname##_UPDATED); \
        } \
    }
    SANITYCHECK3(GyroSensor, gyr, x, y, z);
    SANITYCHECK3(AccelSensor, acc, x, y, z);
    SANITYCHECK3(MagnetoSensor, mag, x, y, z);
    SANITYCHECK3(GPSVelocity, vel, North, East, Down);
        #define SANITYCHECK1(sensorname, shortname, a1, EXTRACHECK) \
    if (ISSET(sensors.updated, shortname##_UPDATED)) { \
        sensorname##Data s; \
        sensorname##GET(&s); \
        if (sane(s.a1) && EXTRACHECK) { \
            sensors.shortname[0] = s.a1; \
        } \
        else { \
            UNSET(sensors.updated, shortname##_UPDATED); \
        } \
    }
    SANITYCHECK1(BaroSensor, bar, Altitude, 1);
    SANITYCHECK1(AirspeedSensor, ias, CalibratedAirspeed, s.SensorConnected == AIRSPEEDSENSOR_SENSORCONNECTED_TRUE);

    if (ISSET(sensors.updated, pos_UPDATED)) {
        GPSPositionData s;
        GPSPositionGet(&s);
        if (homeLocation.Set == HOMELOCATION_SET_TRUE && sane(s.Latitude) && sane(s.Longitude) && sane(s.Altitude) && (fabsf(s.Latitude) > 1e-5f || fabsf(s.Latitude) > 1e-5f || fabsf(s.Latitude) > 1e-5f)) {
            getNED(&s, sensors.pos);
        } else {
            UNSET(sensors.updated, pos_UPDATED);
        }
    }

    // at this point sensor state is stored in "sensors" with some rudimentary filtering applied

    // traverse filtering chain


    // clear alarms if everything is alright, then schedule callback execution after timeout
    if (!alarm) {
        AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
    }
    DelayedCallbackSchedule(stateEstimationCallback, TIMEOUT_MS, UPDATEMODE_SOONER);
}

static void settingsUpdatedCb(UAVObjEvent *ev)
{
    HomeLocationGet(&homeLocation);

    if (sane(homeLocation.Latitude) && sane(homeLocation.Longitude) && sane(homeLocation.Altitude) && sane(homeLocation.Be[0]) && sane(homeLocation.Be[1]) && sane(homeLocation.Be[2])) {
        // Compute matrix to convert deltaLLA to NED
        float lat, alt;
        lat = DEG2RAD(homeLocation.Latitude / 10.0e6f);
        alt = homeLocation.Altitude;

        LLA2NEDM[0] = alt + 6.378137E6f;
        LLA2NEDM[1] = cosf(lat) * (alt + 6.378137E6f);
        LLA2NEDM[2] = -1.0f;

        // TODO: convert positionState to new reference frame and gracefully update EKF state!
        // needed for long range flights where the reference coordinate is adjusted in flight
    }

    RevoSettingsGet(&revoSettings);

    if
}

static void sensorUpdatedCb(UAVObjEvent *ev)
{
    if (!ev) {
        return;
    }

    if (ev->obj == GyroSensorHandle()) {
        updatedSensors |= gyr_UPDATED;
    }

    if (ev->obj == AccelSensorHandle()) {
        updatedSensors |= acc_UPDATED;
    }

    if (ev->obj == MagnetoSensorHandle()) {
        updatedSensors |= mag_UPDATED;
    }

    if (ev->obj == GPSPositionHandle()) {
        updatedSensors |= pos_UPDATED;
    }

    if (ev->obj == GPSVelocityHandle()) {
        updatedSensors |= vel_UPDATED;
    }

    if (ev->obj == BaroSensorHandle()) {
        updatedSensors |= bar_UPDATED;
    }

    if (ev->obj == AirspeedSensorHandle()) {
        updatedSensors |= ias_UPDATED;
    }

    DelayedCallbackDispatch(stateEstimationCallback);
}

/**
 * @brief Convert the GPS LLA position into NED coordinates
 * @note this method uses a taylor expansion around the home coordinates
 * to convert to NED which allows it to be done with all floating
 * calculations
 * @param[in] Current GPS coordinates
 * @param[out] NED frame coordinates
 * @returns 0 for success, -1 for failure
 */
static void getNED(GPSPositionData *gpsPosition, float *NED)
{
    float dL[3] = { DEG2RAD((gpsPosition->Latitude - homeLocation.Latitude) / 10.0e6f),
                    DEG2RAD((gpsPosition->Longitude - homeLocation.Longitude) / 10.0e6f),
                    (gpsPosition->Altitude + gpsPosition->GeoidSeparation - homeLocation.Altitude) };

    NED[0] = LLA2NEDM[0] * dL[0];
    NED[1] = LLA2NEDM[1] * dL[1];
    NED[2] = LLA2NEDM[2] * dL[2];
}


/**
 * @}
 * @}
 */

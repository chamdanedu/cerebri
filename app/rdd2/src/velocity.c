/*
 * Copyright CogniPilot Foundation 2023
 * SPDX-License-Identifier: Apache-2.0
 */

#include "casadi/gen/rdd2.h"
#include "math.h"

#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include <cerebri/core/casadi.h>

#include "mixing.h"

#define MY_STACK_SIZE 3072
#define MY_PRIORITY 4

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(rdd2_velocity, CONFIG_CEREBRI_RDD2_LOG_LEVEL);

typedef struct _context {
    struct zros_node node;
    synapse_msgs_Twist cmd_vel;
    synapse_msgs_Status status;
    synapse_msgs_Actuators actuators;
    synapse_msgs_Actuators actuators_manual;
    synapse_msgs_Vector3 rates_sp;
    synapse_msgs_Imu imu;
    struct zros_sub sub_status, sub_cmd_vel, sub_rates_sp, sub_imu, sub_actuators_manual;
    struct zros_pub pub_actuators;
} context;

static context g_ctx = {
    .node = {},
    .cmd_vel = synapse_msgs_Twist_init_default,
    .status = synapse_msgs_Status_init_default,
    .actuators = synapse_msgs_Actuators_init_default,
    .actuators_manual = synapse_msgs_Actuators_init_default,
    .sub_status = {},
    .sub_cmd_vel = {},
    .sub_rates_sp = {},
    .sub_imu = {},
    .pub_actuators = {},
};

static void init_rdd2_vel(context* ctx)
{
    LOG_DBG("init vel");
    zros_node_init(&ctx->node, "rdd2_velocity");
    zros_sub_init(&ctx->sub_cmd_vel, &ctx->node, &topic_cmd_vel, &ctx->cmd_vel, 10);
    zros_sub_init(&ctx->sub_status, &ctx->node, &topic_status, &ctx->status, 10);
    zros_sub_init(&ctx->sub_rates_sp, &ctx->node,
        &topic_rates_sp, &ctx->rates_sp, 10);
    zros_sub_init(&ctx->sub_actuators_manual, &ctx->node,
        &topic_actuators_manual, &ctx->actuators_manual, 10);
    zros_sub_init(&ctx->sub_imu, &ctx->node,
        &topic_imu, &ctx->imu, 100);
    zros_pub_init(&ctx->pub_actuators, &ctx->node, &topic_actuators, &ctx->actuators);
}

// computes rc_input from V, omega
static void update_cmd_vel_manual(context* ctx)
{
    static const double deg2rad = M_PI / 180.0;
    double roll_rate_cmd = 60 * deg2rad * -ctx->actuators_manual.normalized[0];
    double pitch_rate_cmd = 60 * deg2rad * -ctx->actuators_manual.normalized[1];
    double yaw_rate_cmd = 60 * deg2rad * -ctx->actuators_manual.normalized[2];
    double thrust_cmd = ctx->actuators_manual.normalized[3];

    static const double kp_roll = 0.013;
    static const double kp_pitch = 0.013;
    static const double kp_yaw = 0.1;

    double roll = kp_roll * (roll_rate_cmd - ctx->imu.angular_velocity.x);
    double pitch = kp_pitch * (pitch_rate_cmd + ctx->imu.angular_velocity.y);
    double yaw = kp_yaw * (yaw_rate_cmd + ctx->imu.angular_velocity.z);

    rdd2_set_actuators(&ctx->actuators, roll, pitch, yaw, thrust_cmd);
}

static void update_cmd_vel_auto_level(context* ctx)
{
    static const double kp_x = 0.013;
    static const double kp_y = 0.013;
    static const double kp_z = 0.1;
    static const double ff_x = 0;
    static const double ff_y = 0;
    static const double ff_z = 0;

    double mx = kp_x * (ctx->rates_sp.x - ctx->imu.angular_velocity.x) + ff_x;
    double my = kp_y * (ctx->rates_sp.y + ctx->imu.angular_velocity.y) + ff_y;
    double mz = kp_z * (ctx->rates_sp.z + ctx->imu.angular_velocity.z) + ff_z;
    double thrust = ctx->actuators_manual.normalized[3];

    rdd2_set_actuators(&ctx->actuators, mx, my, mz, thrust);
}

static void stop(context* ctx)
{
    rdd2_set_actuators(&ctx->actuators, 0, 0, 0, 0);
}

static void rdd2_velocity_entry_point(void* p0, void* p1, void* p2)
{
    LOG_INF("init");
    context* ctx = p0;
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);

    init_rdd2_vel(ctx);

    while (true) {
        synapse_msgs_Status_Mode mode = ctx->status.mode;

        int rc = 0;
        if (mode == synapse_msgs_Status_Mode_MODE_MANUAL || mode == synapse_msgs_Status_Mode_MODE_CMD_VEL) {
            struct k_poll_event events[] = {
                *zros_sub_get_event(&ctx->sub_actuators_manual),
            };
            rc = k_poll(events, ARRAY_SIZE(events), K_MSEC(1000));
            if (rc != 0) {
                LOG_DBG("not receiving manual actuators");
            }
        } else {
            struct k_poll_event events[] = {
                *zros_sub_get_event(&ctx->sub_cmd_vel),
            };
            rc = k_poll(events, ARRAY_SIZE(events), K_MSEC(1000));
            if (rc != 0) {
                LOG_DBG("not receiving cmd_vel");
            }
        }

        if (zros_sub_update_available(&ctx->sub_status)) {
            zros_sub_update(&ctx->sub_status);
        }

        if (zros_sub_update_available(&ctx->sub_cmd_vel)) {
            zros_sub_update(&ctx->sub_cmd_vel);
        }

        if (zros_sub_update_available(&ctx->sub_actuators_manual)) {
            zros_sub_update(&ctx->sub_actuators_manual);
        }

        if (zros_sub_update_available(&ctx->sub_imu)) {
            zros_sub_update(&ctx->sub_imu);
        }
        if (zros_sub_update_available(&ctx->sub_rates_sp)) {
            zros_sub_update(&ctx->sub_rates_sp);
        }

        // handle modes
        if (rc < 0) {
            stop(ctx);
            LOG_DBG("no data, stopped");
        } else if (ctx->status.arming != synapse_msgs_Status_Arming_ARMING_ARMED) {
            stop(ctx);
            LOG_DBG("not armed, stopped");
        } else if (ctx->status.mode == synapse_msgs_Status_Mode_MODE_MANUAL) {
            LOG_DBG("manual mode");
            // ctx->actuators = ctx->actuators_manual;
            update_cmd_vel_manual(ctx);
        } else if (ctx->status.mode == synapse_msgs_Status_Mode_MODE_CMD_VEL) {
            LOG_DBG("autoo level mode");
            // ctx->actuators = ctx->actuators_manual;
            update_cmd_vel_auto_level(ctx);
        } else {
        }

        // publish
        zros_pub_update(&ctx->pub_actuators);
    }
}

K_THREAD_DEFINE(rdd2_velocity, MY_STACK_SIZE,
    rdd2_velocity_entry_point, &g_ctx, NULL, NULL,
    MY_PRIORITY, 0, 1000);

/* vi: ts=4 sw=4 et */

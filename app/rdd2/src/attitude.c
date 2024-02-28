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

#include <synapse_topic_list.h>

#define MY_STACK_SIZE 3072
#define MY_PRIORITY 4

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(rdd2_attitude, CONFIG_CEREBRI_RDD2_LOG_LEVEL);

typedef struct _context {
    struct zros_node node;
    synapse_msgs_Twist cmd_vel;
    synapse_msgs_Status status;
    synapse_msgs_Actuators actuators_manual;
    synapse_msgs_Odometry estimator_odometry;
    synapse_msgs_Vector3 rates_sp;
    struct zros_sub sub_status, sub_actuators_manual, sub_estimator_odometry;
    struct zros_pub pub_cmd_vel;
    struct zros_pub pub_rates_sp;
} context;

static context g_ctx = {
    .node = {},
    .cmd_vel = {
        .has_linear = false,
        .has_angular = true,
        .angular = synapse_msgs_Vector3_init_default,
    },
    .status = synapse_msgs_Status_init_default,
    .actuators_manual = synapse_msgs_Actuators_init_default,
    .rates_sp = synapse_msgs_Vector3_init_default,
    .estimator_odometry = synapse_msgs_Odometry_init_default,
    .sub_status = {},
    .sub_actuators_manual = {},
    .sub_estimator_odometry = {},
    .pub_cmd_vel = {},
    .pub_rates_sp = {},
};

static void init_rdd2_vel(context* ctx)
{
    LOG_DBG("init attitude");
    zros_node_init(&ctx->node, "rdd2_attiude");
    zros_sub_init(&ctx->sub_status, &ctx->node, &topic_status, &ctx->status, 10);
    zros_sub_init(&ctx->sub_actuators_manual, &ctx->node,
        &topic_actuators_manual, &ctx->actuators_manual, 10);
    zros_sub_init(&ctx->sub_estimator_odometry, &ctx->node,
        &topic_estimator_odometry, &ctx->estimator_odometry, 100);
    zros_pub_init(&ctx->pub_cmd_vel, &ctx->node, &topic_cmd_vel, &ctx->cmd_vel);
    zros_pub_init(&ctx->pub_rates_sp, &ctx->node, &topic_rates_sp, &ctx->rates_sp);
}

// computes rc_input from V, omega
static void update_cmd_vel(context* ctx)
{
    /* attitude_error:(q[4],yaw_r,pitch_r,roll_r)->(omega[3]) */
    CASADI_FUNC_ARGS(attitude_error);
    double q[4];
    double omega[3];
    double euler[3];
    euler[0] = ctx->actuators_manual.normalized[0];
    euler[1] = ctx->actuators_manual.normalized[1];
    euler[2] = ctx->actuators_manual.normalized[2];

    q[0] = ctx->estimator_odometry.pose.pose.orientation.w;
    q[1] = ctx->estimator_odometry.pose.pose.orientation.x;
    q[2] = ctx->estimator_odometry.pose.pose.orientation.y;
    q[3] = ctx->estimator_odometry.pose.pose.orientation.z;
    args[0] = q;
    args[1] = &euler[0];
    args[2] = &euler[1];
    args[3] = &euler[2];
    res[0] = omega;
    CASADI_FUNC_CALL(attitude_error);

    // LOG_INF("q: %10.4f %10.4f %10.4f %10.4f", q[0], q[1], q[2], q[3]);
    // LOG_INF("euler: %10.4f %10.4f %10.4f", euler[0], euler[1], euler[2]);
    // LOG_INF("omega: %10.4f %10.4f %10.4f", omega[0], omega[1], omega[2]);

    // set cmd_vel
    ctx->cmd_vel.angular.x = omega[0];
    ctx->cmd_vel.angular.y = omega[1];
    ctx->cmd_vel.angular.z = omega[2];

    // publish
    ctx->rates_sp.x = omega[0];
    ctx->rates_sp.y = omega[1];
    ctx->rates_sp.z = omega[2];
    zros_pub_update(&ctx->pub_rates_sp);
}

static void stop(context* ctx)
{
    (void)ctx;
    ctx->cmd_vel.angular.x = 0;
    ctx->cmd_vel.angular.y = 0;
    ctx->cmd_vel.angular.z = 0;
}

static void rdd2_attitude_entry_point(void* p0, void* p1, void* p2)
{
    LOG_INF("init");
    context* ctx = p0;
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);

    init_rdd2_vel(ctx);

    while (true) {
        // synapse_msgs_Status_Mode mode = ctx->status.mode;

        int rc = 0;
        struct k_poll_event events[] = {
            *zros_sub_get_event(&ctx->sub_estimator_odometry),
        };
        rc = k_poll(events, ARRAY_SIZE(events), K_MSEC(1000));
        if (rc != 0) {
            LOG_DBG("not receiving estimator_odometry");
        }

        if (zros_sub_update_available(&ctx->sub_status)) {
            zros_sub_update(&ctx->sub_status);
        }

        if (zros_sub_update_available(&ctx->sub_estimator_odometry)) {
            zros_sub_update(&ctx->sub_estimator_odometry);
        }

        if (zros_sub_update_available(&ctx->sub_actuators_manual)) {
            zros_sub_update(&ctx->sub_actuators_manual);
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
            update_cmd_vel(ctx);
        } else if (ctx->status.mode == synapse_msgs_Status_Mode_MODE_CMD_VEL) {
            LOG_DBG("cmd_vel mode");
            update_cmd_vel(ctx);
        }

        // publish
        // zros_pub_update(&ctx->pub_cmd_vel);
    }
}

K_THREAD_DEFINE(rdd2_attitude, MY_STACK_SIZE,
    rdd2_attitude_entry_point, &g_ctx, NULL, NULL,
    MY_PRIORITY, 0, 1000);

/* vi: ts=4 sw=4 et */

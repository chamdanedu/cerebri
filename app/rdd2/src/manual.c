/*
 * Copyright CogniPilot Foundation 2023
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zros/private/zros_node_struct.h>
#include <zros/private/zros_pub_struct.h>
#include <zros/private/zros_sub_struct.h>
#include <zros/zros_node.h>
#include <zros/zros_pub.h>
#include <zros/zros_sub.h>

#include <synapse_topic_list.h>

#include "mixing.h"

#define MY_STACK_SIZE 1024
#define MY_PRIORITY 4

LOG_MODULE_REGISTER(rdd2_manual, CONFIG_CEREBRI_RDD2_LOG_LEVEL);

typedef struct _context {
    struct zros_node node;
    synapse_msgs_Joy joy;
    synapse_msgs_Actuators actuators_manual;
    struct zros_sub sub_joy;
    struct zros_pub pub_actuators_manual;
} context;

static context g_ctx = {
    .node = {},
    .joy = synapse_msgs_Joy_init_default,
    .actuators_manual = synapse_msgs_Actuators_init_default,
    .sub_joy = {},
    .pub_actuators_manual = {},
};

static void init(context* ctx)
{
    zros_node_init(&ctx->node, "rdd2_manual");
    zros_sub_init(&ctx->sub_joy, &ctx->node, &topic_joy, &ctx->joy, 10);
    zros_pub_init(&ctx->pub_actuators_manual, &ctx->node,
        &topic_actuators_manual, &ctx->actuators_manual);
}

static void rdd2_manual_entry_point(void* p0, void* p1, void* p2)
{
    LOG_INF("init");
    context* ctx = p0;
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);

    init(ctx);

    struct k_poll_event events[] = {
        *zros_sub_get_event(&ctx->sub_joy),
    };

    while (true) {
        // wait for joystick input event, publish at 1 Hz regardless
        int rc = 0;
        rc = k_poll(events, ARRAY_SIZE(events), K_MSEC(1000));
        if (rc != 0) {
            // LOG_DBG("manual not receiving joy");
        }

        if (zros_sub_update_available(&ctx->sub_joy)) {
            zros_sub_update(&ctx->sub_joy);
        }

        // set normalized inputs
        ctx->actuators_manual.normalized_count = 4;
        ctx->actuators_manual.normalized[0] = ctx->joy.axes[JOY_AXES_ROLL];
        ctx->actuators_manual.normalized[1] = ctx->joy.axes[JOY_AXES_PITCH];
        ctx->actuators_manual.normalized[2] = ctx->joy.axes[JOY_AXES_YAW];
        ctx->actuators_manual.normalized[3] = ctx->joy.axes[JOY_AXES_THRUST] * 0.1 + 0.5;

        // saturation
        for (int i = 0; i < 4; i++) {
            if (ctx->actuators_manual.normalized[i] < -1) {
                ctx->actuators_manual.normalized[i] = -1;
            } else if (ctx->actuators_manual.normalized[i] > 1) {
                ctx->actuators_manual.normalized[i] = 1;
            }
        }
        zros_pub_update(&ctx->pub_actuators_manual);
    }
}

K_THREAD_DEFINE(rdd2_manual, MY_STACK_SIZE,
    rdd2_manual_entry_point, (void*)&g_ctx, NULL, NULL,
    MY_PRIORITY, 0, 1000);

/* vi: ts=4 sw=4 et */

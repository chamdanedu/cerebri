/*
 * Copyright CogniPilot Foundation 2023
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <cerebri/synapse/zbus/common.h>
#include <cerebri/synapse/zbus/syn_pub_sub.h>

#define MY_STACK_SIZE 3072
#define MY_PRIORITY 4

LOG_MODULE_REGISTER(control_ackermann, CONFIG_CEREBRI_CONTROL_ACKERMANN_LOG_LEVEL);

typedef struct _context {
    // node
    syn_node_t node;
    // data
    synapse_msgs_Joy joy;
    synapse_msgs_BatteryState battery_state;
    synapse_msgs_Safety safety;
    synapse_msgs_Fsm fsm;
    // subscriptions
    syn_sub_t sub_joy, sub_battery_state, sub_safety;
    // publications
    syn_pub_t pub_fsm;
} context;

static context g_ctx = {
    .joy = synapse_msgs_Joy_init_default,
    .battery_state = synapse_msgs_BatteryState_init_default,
    .safety = synapse_msgs_Safety_init_default,
    .fsm = {
        .has_header = true,
        .header = {
            .frame_id = "map",
            .has_stamp = true,
            .seq = 0,
            .stamp = synapse_msgs_Time_init_default,
        },
        .armed = synapse_msgs_Fsm_Armed_DISARMED,
        .mode = synapse_msgs_Fsm_Mode_UNKNOWN_MODE,
    },
    .sub_joy = { 0 },
    .sub_battery_state = { 0 },
    .pub_fsm = { 0 },
    .node = { 0 },
};

static void control_ackermann_fsm_init(context* ctx)
{
    syn_node_init(&ctx->node, "control_ackermann_fsm");
    syn_node_add_sub(&ctx->node, &ctx->sub_joy, &ctx->joy, &chan_in_joy);
    syn_node_add_sub(&ctx->node, &ctx->sub_battery_state,
        &ctx->battery_state, &chan_out_battery_state);
    syn_node_add_sub(&ctx->node, &ctx->sub_safety, &ctx->safety, &chan_out_safety);
    syn_node_add_pub(&ctx->node, &ctx->pub_fsm, &ctx->fsm, &chan_out_fsm);
}

static void update_fsm(
    const synapse_msgs_Joy* joy,
    const synapse_msgs_BatteryState* battery_state,
    const synapse_msgs_Safety* safety,
    synapse_msgs_Fsm* fsm)
{
    // arming
    if (joy->buttons[JOY_BUTTON_ARM] == 1 && fsm->armed != synapse_msgs_Fsm_Armed_ARMED) {

#ifdef CONFIG_SENSE_SAFETY
        if (safety->status != synapse_msgs_Safety_Status_UNSAFE) {
            LOG_WRN("safety: %s, cannot arm", safety_str(safety->status));
            return;
        }
#endif

        if (fsm->mode == synapse_msgs_Fsm_Mode_UNKNOWN_MODE) {
            LOG_WRN("cannot arm until mode selected");
            return;
        }
        LOG_INF("armed in mode: %s", fsm_mode_str(fsm->mode));
        LOG_INF("battery voltage: %f", battery_state->voltage);
        fsm->armed = synapse_msgs_Fsm_Armed_ARMED;
    } else if (joy->buttons[JOY_BUTTON_DISARM] == 1 && fsm->armed == synapse_msgs_Fsm_Armed_ARMED) {
        LOG_INF("disarmed");
        fsm->armed = synapse_msgs_Fsm_Armed_DISARMED;
    }

    // handle modes
    synapse_msgs_Fsm_Mode prev_mode = fsm->mode;
    if (joy->buttons[JOY_BUTTON_MANUAL] == 1) {
        fsm->mode = synapse_msgs_Fsm_Mode_MANUAL;
    } else if (joy->buttons[JOY_BUTTON_AUTO] == 1) {
        fsm->mode = synapse_msgs_Fsm_Mode_AUTO;
    } else if (joy->buttons[JOY_BUTTON_CMD_VEL] == 1) {
        fsm->mode = synapse_msgs_Fsm_Mode_CMD_VEL;
    }

    // notify on mode change
    if (fsm->mode != prev_mode) {
        LOG_INF("mode changed to: %s", fsm_mode_str(fsm->mode));
    }

    // update fsm
    stamp_header(&fsm->header, k_uptime_ticks());
    fsm->header.seq++;
}

static void control_ackermann_fsm_run(context* ctx)
{
    control_ackermann_fsm_init(ctx);

    while (true) {

        // wait for joystick input event, publish at 1 Hz regardless
        RC(syn_sub_poll(&ctx->sub_joy, K_MSEC(1000)),
            LOG_DBG("fsm not receiving joy"));

        // perform processing
        syn_node_lock_all(&ctx->node, K_MSEC(1));
        update_fsm(&ctx->joy, &ctx->battery_state, &ctx->safety, &ctx->fsm);
        syn_node_publish_all(&ctx->node, K_MSEC(1));
        syn_node_unlock_all(&ctx->node);
    }
}

K_THREAD_DEFINE(control_ackermann_fsm, MY_STACK_SIZE,
    control_ackermann_fsm_run, &g_ctx, NULL, NULL,
    MY_PRIORITY, 0, 0);

static void listener_control_ackermann_fsm_callback(const struct zbus_channel* chan)
{
    syn_node_listen(&g_ctx.node, chan, K_MSEC(100));
}

ZBUS_LISTENER_DEFINE(listener_control_ackermann_fsm, listener_control_ackermann_fsm_callback);
// ZBUS_CHAN_ADD_OBS(chan_out_battery_state, listener_control_ackermann_fsm, 1);
ZBUS_CHAN_ADD_OBS(chan_in_joy, listener_control_ackermann_fsm, 1);

/* vi: ts=4 sw=4 et */
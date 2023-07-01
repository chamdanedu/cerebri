/*
 * Copyright CogniPilot Foundation 2023
 * SPDX-License-Identifier: Apache-2.0
 */

#include <synapse/zbus/channels.h>
#include <synapse_tinyframe/SynapseTopics.h>
#include <synapse_tinyframe/TinyFrame.h>

#include <pb_encode.h>
#include <synapse_protobuf/sim_clock.pb.h>
#include <synapse_tinyframe/utils.h>

#include <zephyr/sys/ring_buffer.h>

#define MY_STACK_SIZE 500
#define MY_PRIORITY -10

// private data
struct private_module_context {
    const char* module_name;
};

static struct private_module_context g_priv = {
    .module_name = "dream_sitl_zephyr",
};

extern synapse_msgs_SimClock g_sim_clock;
extern TinyFrame g_tf;
extern bool g_clock_initialized;
extern synapse_msgs_Time g_clock_offset;
extern synapse_msgs_NavSatFix g_in_nav_sat_fix;
extern synapse_msgs_BatteryState g_in_battery_state;
extern synapse_msgs_Imu g_in_imu;
extern struct ring_buf g_msg_updates;

void listener_dream_sitl_callback(const struct zbus_channel* chan)
{
    if (chan == &chan_out_actuators) {
        TF_Msg msg;
        TF_ClearMsg(&msg);
        uint8_t buf[synapse_msgs_Actuators_size];
        pb_ostream_t stream = pb_ostream_from_buffer((pu8)buf, sizeof(buf));
        int status = pb_encode(&stream, synapse_msgs_Actuators_fields, chan->message);
        if (status) {
            msg.type = SYNAPSE_OUT_ACTUATORS_TOPIC;
            msg.data = buf;
            msg.len = stream.bytes_written;
            TF_Send(&g_tf, &msg);
        } else {
            printf("dream_sitl: encoding failed: %s\n", PB_GET_ERROR(&stream));
        }
    }
}
ZBUS_LISTENER_DEFINE(listener_dream_sitl, listener_dream_sitl_callback);

static void zephyr_sim_entry_point(void)
{
    printf("%s: zephyr sim entry point\n", g_priv.module_name);
    printf("%s: waiting for sim clock\n", g_priv.module_name);
    while (true) {
        synapse_msgs_SimClock sim_clock;
        struct timespec request, remaining;
        request.tv_sec = 1;
        request.tv_nsec = 0;
        nanosleep(&request, &remaining);

        bool clock_init;
        sim_clock = g_sim_clock;
        clock_init = g_clock_initialized;

        // if clock not initialized, wait 1 second
        if (clock_init) {
            printf("%s: sim clock initialized\n", g_priv.module_name);
            zbus_chan_pub(&chan_in_clock_offset, &g_clock_offset, K_NO_WAIT);
            break;
        }
    }

    printf("%s: running main loop\n", g_priv.module_name);
    while (true) {

        //  publish new messages
        uint8_t topic;
        while (!ring_buf_is_empty(&g_msg_updates)) {
            ring_buf_get(&g_msg_updates, &topic, 1);
            if (topic == SYNAPSE_IN_NAVSAT_TOPIC) {
                zbus_chan_pub(&chan_in_nav_sat_fix, &g_in_nav_sat_fix, K_NO_WAIT);
            } else if (topic == SYNAPSE_IN_MAG_TOPIC) {
                // zbus_chan_pub(&chan_in_mag, &g_in_mag, K_NO_WAIT);
            } else if (topic == SYNAPSE_IN_IMU_TOPIC) {
                zbus_chan_pub(&chan_in_imu, &g_in_imu, K_NO_WAIT);
            } else if (topic == SYNAPSE_IN_BATTERY_STATE_TOPIC) {
                zbus_chan_pub(&chan_in_battery_state, &g_in_battery_state, K_NO_WAIT);
            }
        }

        // compute board time
        uint64_t uptime = k_uptime_get();
        struct timespec ts_board;
        ts_board.tv_sec = uptime / 1.0e3;
        ts_board.tv_nsec = (uptime - ts_board.tv_sec * 1e3) * 1e6;
        ts_board.tv_sec += g_clock_offset.sec;
        ts_board.tv_nsec += g_clock_offset.nanosec;

        // compute time delta from sim
        int64_t delta_sec = g_sim_clock.sim.sec - ts_board.tv_sec;
        int32_t delta_nsec = g_sim_clock.sim.nanosec - ts_board.tv_nsec;
        int64_t wait_msec = delta_sec * 1e3 + delta_nsec * 1e-6;

        /*
        printf("%s, sim: sec %lld nsec %d\n",
                g_priv.module_name, g_sim_clock.sim.sec, g_sim_clock.sim.nsec);
        printf("%s, board: sec %ld nsec %ld\n",
                g_priv.module_name, ts_board.tv_sec, ts_board.tv_nsec);
        printf("%s, wait: msec %lld\n", g_priv.module_name, wait_msec);
        */

        // sleep to match clocks
        if (wait_msec > 0) {
            k_msleep(wait_msec);
        } else {
            struct timespec request, remaining;
            request.tv_sec = 0;
            request.tv_nsec = 1000000;
            nanosleep(&request, &remaining);
        }
    }
}

// zephyr threads
K_THREAD_DEFINE(zephyr_sim_thread, MY_STACK_SIZE, zephyr_sim_entry_point,
    NULL, NULL, NULL, MY_PRIORITY, 0, 0);

// vi: ts=4 sw=4 et
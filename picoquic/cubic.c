/*
 *
 * Copyright (c) 2017 Private Octopus
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions: 

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* NOTE :
 * This file implements the CUBIC congestion control algorithm.
 * The CUBIC congestion control was developed by Ijong Rhee and Lisong Xu.
 * See :
 * [2]  Ijong Rhee and Lisong Xu,
 *      CUBIC: A New TCP-Friendly High-Speed TCP Variant
 *      Available at : http://www4.ncsu.edu/~rhee/export/bictcp/cubic-paper.pdf
 * 
 * This file copies and adapts the implementation of the CUBIC congestion control 
 * for Chromium by the Chromium Authors. See :
 * [1]  The Chromium Authors,
 *      The Chromium Projects,
 *      Source code available at https://chromium.googlesource.com/chromium/src
 * 
 */
#include "picoquic_internal.h"

#include <collectagent.h>

#include <stdlib.h>
#include <math.h>

#define DEFAULT_TCP_MSS 1460
#define MICROSEC_PER_SEC 1000000

/**
 * Following constants are in 2^10 fractions of a second instead of ms to
 * allow a 10 shift right to divide.
 */
static const int kCubeScale = 40;   //1024*1024^3 (first 1024 is from 0.100^3)
                                    //where 0.100 is 100ms which is the scaling RTT.
static const int kCubeCongestionWindowScale = 410;
//The cube factor for packets in bytes.
static int kCubeFactor;

const float kBeta = 0.7f; //Default Cubic backoff factor.
const float kBetaLastMax = 0.85f; //Backoff factor for Wmax for fast convergence when another competing flow is starting.

/**
 * @brief State of the congestion controller : 
 * Slow start : performing traditionnal slow start without threshold
 * Congestion avoidance : Performing the true Cubic algorithm
 * 
 */
typedef enum {
    picoquic_cubic_alg_slow_start = 0,
    picoquic_cubic_alg_congestion_avoidance
} picoquic_cubic_alg_state_t;

/**
 * @brief Struct holding the data for the congestion controller
 * 
 */
typedef struct st_picoquic_cubic_state_t {
    picoquic_cubic_alg_state_t alg_state;   // The state of the CC
    uint64_t epoch_start_time;              // Absolute time of the beginning of the current epoch, or 0 if we are not in any epoch
    uint64_t estimated_nr_cwnd;             // Estimation of the New Reno congestion window. Used to ensure TCP friendliness. See [2]
    uint64_t last_max_cwnd;                 // The max of the congestion window during last epoch. 
                                            // This is also the value of the congestion window when the loss occured. 
                                            // The value is potentially modified with a fast convergence backoff factor. 
    uint64_t time_of_origin;                // The time of the origin point of the cubic function of the current epoch, relative to the start of the epoch.
    uint64_t origin_cwnd;                   // The congestion window at the origin point of the cubic function of the current epoch.
    uint64_t last_target_cwnd;              // The target cwnd on the last computation step (that is the congestion window computed with cubic algorithm, potentially limited by the increase limitation heuristic).
} picoquic_cubic_state_t;

/**
 * @brief Initializes the congestion controller
 * 
 * @param[in,out] path_x The path on which the congestion controller shall apply.
 */
void picoquic_cubic_init(picoquic_path_t* path_x)
{
    fprintf(stderr, "-\nThis is cubic congestion controller\n-\n");
    if(!collect_agent_register_collect("picoquic_cubic_collectagent.conf"))
    {
        fprintf(stderr, "[ERR] Unable to connect to deamon\n");
    }
    collect_agent_send_log(1,"Congestion controller started\n");
    kCubeFactor = (UINT64_C(1) << kCubeScale) / kCubeCongestionWindowScale / DEFAULT_TCP_MSS;
    picoquic_cubic_state_t* cu_state = (picoquic_cubic_state_t*)malloc(sizeof(picoquic_cubic_state_t));
    path_x->congestion_alg_state = (void*)cu_state;

    if (path_x->congestion_alg_state != NULL) {
        cu_state->alg_state = picoquic_cubic_alg_slow_start;
        cu_state->epoch_start_time = 0;
        cu_state->estimated_nr_cwnd = 0;
        cu_state->last_max_cwnd = 0;
        cu_state->time_of_origin = 0;
        cu_state->origin_cwnd = 0;
        cu_state->last_target_cwnd = 0;

        path_x->cwin = PICOQUIC_CWIN_INITIAL;
    }
}

/**
 * @brief Computes the backoff factor
 * after the loss of a packet in our n-stream connection.
 * This simulates the effective backoff of an ensemble of N tcp
 * cubic connections on a single loss event
 * *(path_x->total_stream_count) is the number of streams.
 * 
 * @param[in] path_x. An initialized path on which to apply congestion control 
 * @return float . The backoff factor for our connection on this path.
 */
static float Beta(picoquic_path_t* path_x)
{
    return (((*(path_x->total_stream_count)) - 1 + kBeta) /  (*(path_x->total_stream_count))); 
}
/**
 * @brief Computes the TCP Friendly alpha for a connection with n streams.
 * See section 3.3 of [2]
 * Beta here is a cwnd multiplier, and is equal to 1-beta in [2].
 * @param[in] path_x. An initialized path on which to apply congestion control  
 * 
 * @return float the TCP Friendly alpha
 */
static float Alpha(picoquic_path_t* path_x)
{
    float beta = Beta(path_x);
    return (3 * (*(path_x->total_stream_count)) * (*(path_x->total_stream_count)) * (1-beta) / (1-beta));
}

/**
 * @brief Computes the Wmax backoff for fast convergence with a connection of n streams.
 * This emulates the Wmax backoff of n cubic streams on a single loss event.
 * 
 * @param[in] path_x. An initialized path on which to apply congestion control  
 * @return float. The Wmax backoff factor. 
 */
static float BetaLastMax(picoquic_path_t* path_x)
{
    return (((*(path_x->total_stream_count)) - 1 + kBetaLastMax) / (*(path_x->total_stream_count)));
}

/**
 * @brief Processes the reception of an ACK for the Cubic congestion controller.
 * It updates the new value of the congestion window in bytes in path_x->cwin.
 * 
 * @param[in,out] path_x The path on which the congestion controller shall apply. It shall have an initialized congestion_alg_state member.
 * @param[in] current_time Reception time of the ACK. In the same common unit of picoquic (usec). 
 * @param[in] nb_bytes_acknowledged Number of bytes acknowledged 
 */
static void picoquic_cubic_process_ack(picoquic_path_t* path_x, uint64_t current_time, uint64_t nb_bytes_acknowledged)
{
    picoquic_cubic_state_t* cu_state = (picoquic_cubic_state_t*)path_x->congestion_alg_state;

    if(!cu_state)
    {
        return;
    }

    if(path_x->bytes_in_transit < path_x->cwin)
    {
        // When an ACK arrives BUT the sender is unable to use the available
        // congestion window, we reset the cubic period. This freezes the window 
        // growth trough application-limited periods and allows Cubic growth to 
        // continue when the entire window is being used.
        // In picoquic, congestion control is notified before the acknowledged packets
        // are removed from retransmit queue. According to what is visible for the
        // Congestion Controller (CC) in path_x, we cannot determine whether the application
        // will be able to send more data after receiving *this* ACK. 
        // What we can do is determine if the sender was application limited when it received
        // the *last* ACK because path_x->bytes_in_transit is the cwnd usage after the *last* ACK
        // reception and before the reception of *this* ACK. 
        cu_state->epoch_start_time = 0;
        fprintf(stderr, "-\nCongestion window not fully used, freezing\n-\n");
        return;
    }

    // Cwin is fully used (at least until this ACK) so proceed the CUBIC algorithm.
    if(cu_state->epoch_start_time == 0)
    {
        fprintf(stderr, "-\nStarting new epoch \n-\n");
        //Not in an epoch, start a new epoch
        //Because we received an ACK, the period has actually begun one RTT ago, so we take
        //this into account. (Taking into account the RTT here is clearer than what's done line 144 of [1]).
        cu_state->epoch_start_time = current_time - path_x->rtt_min;
        //Reset the estimate of new reno (nr) congestion window to be in sync with cubic.
        cu_state->estimated_nr_cwnd = path_x->cwin;
        if(cu_state->last_max_cwnd <= path_x->cwin)
        {
            //Border case when the epoch is initialized with cwin already greater than last_max_cwnd
            //Current (time/cwin) tuple is the new origin.
            cu_state->time_of_origin = 0;
            cu_state->origin_cwnd = path_x->cwin;
        }else{
            // General case : at t = 0, the origin point is at (K,W_max)
            cu_state->time_of_origin = (uint64_t)cbrt(kCubeFactor * (cu_state->last_max_cwnd - path_x->cwin));
            cu_state->origin_cwnd = cu_state->last_max_cwnd;
        }
    }
    // Now we build a time metric whose reference is the start of the epoch and which
    // is in 2^10 fractions of a second. This allows to use the shift as a divide operator. 
    // Default time metric in picoquic is the microsecond.
    // Elapsed time since epoch start
    int64_t elapsed_time = (current_time - cu_state->epoch_start_time) << 10 / MICROSEC_PER_SEC;

    //We compute the offset (time to/since origin) and force it to be positive, to deal with
    //implementation-dependant shifts (see [1]).
    uint64_t offset = abs((int64_t)(cu_state->time_of_origin) - elapsed_time);
    
    //Now we compute the absolute value of the congestion window delta (compared to Wmax)
    uint64_t delta_congestion_window = (kCubeCongestionWindowScale * offset * offset *offset * DEFAULT_TCP_MSS) >> kCubeScale;

    //Add if we are after the origin point, substract if we are before the origin point
    uint8_t addDelta = (elapsed_time > cu_state->time_of_origin);
    uint64_t target_congestion_window = (addDelta ? cu_state->origin_cwnd + delta_congestion_window
                                                    : cu_state->origin_cwnd - delta_congestion_window);
    
    /* Now, apply the different heuristics */
    //Growth limitation : CWD increase is limited by half the acked bytes
    if(target_congestion_window > (path_x->cwin + (nb_bytes_acknowledged / 2)))
    {
        target_congestion_window = (path_x->cwin + (nb_bytes_acknowledged / 2));
    }
    //Store the congestion window
    cu_state->last_target_cwnd = target_congestion_window;
    //Computation of estimated Tcp New Reno congestion window.
    //From [1] :
    // Increase the window by approximately Alpha * 1 MSS of bytes every
    // time we ack an estimated tcp window of bytes. For small
    // congestion windows (less than 25), the formula below will
    // increase slightly slower than linearly per estimated tcp window
    // of bytes. 
    cu_state->estimated_nr_cwnd += nb_bytes_acknowledged * (Alpha(path_x) * DEFAULT_TCP_MSS) / cu_state->estimated_nr_cwnd;
    // Use highest of target and estimated_nr
    if(target_congestion_window < cu_state->estimated_nr_cwnd)
    {
        target_congestion_window = cu_state->estimated_nr_cwnd;
    }
    path_x->cwin = target_congestion_window;
}

/**
 * @brief Processes the loss of a packet for the Cubic congestion controller
 * It updates the value of the congestion window in bytes in path_x->cwin
 * 
 * @param[in,out] path_x The path on which the congestion controller shall apply. It shall have an initialized congestion_alg_state member.
 */
static void picoquic_cubic_process_loss(picoquic_path_t* path_x)
{
    picoquic_cubic_state_t* cu_state = (picoquic_cubic_state_t*)path_x->congestion_alg_state;
    if(!cu_state)
    {
        return;
    }

    /* Fast convergence */
    // If the congestion window + a slight margin (in order not to interpret slight 
    // under-estimation over a RTT as a competing traffic) is below Wmax, we haven't reach
    // the old max, so we assume another flow is competing. We are backing of a little more
    if(path_x->cwin + DEFAULT_TCP_MSS < cu_state->last_max_cwnd)
    {
        cu_state->last_max_cwnd = BetaLastMax(path_x) * path_x->cwin;
    }else{
        cu_state->last_max_cwnd = path_x->cwin;
    }
    //Reset epoch
    cu_state->epoch_start_time = 0;
    // Set the current congestion window backoff
    path_x->cwin = (path_x->cwin * Beta(path_x));
}


/**
 * @brief Processes a notification for the congestion controller.
 * 
 * @param[in,out] path_x                The path on which the congestion controller shall apply. It shall have an initialized congestion_alg_state member.
 * @param[in] notification              The type of notification to send to the controller.
 * @param[in] rtt_measurement           Unused.
 * @param[in] nb_bytes_acknowledged     The number of acknowledged bytes.
 * @param[in] lost_packet_number        Unused.
 * @param[in] current_time              Time of the event, usec.
 */
void picoquic_cubic_notify(picoquic_path_t* path_x,
    picoquic_congestion_notification_t notification,
    uint64_t rtt_measurement,
    uint64_t nb_bytes_acknowledged,
    uint64_t lost_packet_number,
    uint64_t current_time)
{
    fprintf(stderr, "-\n%d streams\n-\n", (*(path_x->total_stream_count)));
#ifdef _WINDOWS
    UNREFERENCED_PARAMETER(rtt_measurement);
    UNREFERENCED_PARAMETER(lost_packet_number);
#endif
    picoquic_cubic_state_t* cu_state = (picoquic_cubic_state_t*)path_x->congestion_alg_state;

    if (cu_state != NULL) {
        switch (cu_state->alg_state) {
        case picoquic_cubic_alg_slow_start:
            switch (notification) {
            case picoquic_congestion_notification_acknowledgement:
                // Proceed with traditionnal slow start
                path_x->cwin += nb_bytes_acknowledged;
                // Without threshold for the moment.
                break;
            case picoquic_congestion_notification_repeat:
            case picoquic_congestion_notification_timeout:
                //Switch to congestion avoidance
                cu_state->alg_state = picoquic_cubic_alg_congestion_avoidance;
                //Process loss
                picoquic_cubic_process_loss(path_x);
                break;
            case picoquic_congestion_notification_spurious_repeat:
                break;
            case picoquic_congestion_notification_rtt_measurement:
                break;
            default:
                break;
            }
            break;
        case picoquic_cubic_alg_congestion_avoidance:
            switch (notification) {
                case picoquic_congestion_notification_acknowledgement:
                    picoquic_cubic_process_ack(path_x, current_time, nb_bytes_acknowledged);
                    break;
                case picoquic_congestion_notification_repeat:
                case picoquic_congestion_notification_timeout:
                    picoquic_cubic_process_loss(path_x);
                    break;
                case picoquic_congestion_notification_spurious_repeat:
                    break;
                case picoquic_congestion_notification_rtt_measurement:
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
        }

        /* Compute pacing data */
        picoquic_update_pacing_data(path_x);
    }
}

/**
 * @brief Releases the states of the congestion control algorithm.
 * 
 * @param[in,out] path_x The path on which to delete the cubic congestion controller.
 */
void picoquic_cubic_delete(picoquic_path_t* path_x)
{
    if (path_x->congestion_alg_state != NULL) {
        free(path_x->congestion_alg_state);
        path_x->congestion_alg_state = NULL;
    }
}

/* Definition record for the cubic algorithm */
#define PICOQUIC_CUBIC_ID 0x0f0f0f0f /* NR88 */

picoquic_congestion_algorithm_t picoquic_cubic_algorithm_struct = {
    PICOQUIC_CUBIC_ID,
    picoquic_cubic_init,
    picoquic_cubic_notify,
    picoquic_cubic_delete
};

picoquic_congestion_algorithm_t* picoquic_cubic_algorithm = &picoquic_cubic_algorithm_struct;

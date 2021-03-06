/*
 * Copyright (C) 2005 Jan Kiszka <jan.kiszka@web.de>.
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/module.h>
#include <asm/semaphore.h>
#ifdef CONFIG_IPIPE_TRACE
#include <linux/ipipe_trace.h>
#endif /* CONFIG_IPIPE_TRACE */

#include <rtdm/rttesting.h>
#include <rtdm/rtdm_driver.h>

struct rt_tmbench_context {
    int                             mode;
    unsigned long                   period;
    int                             freeze_max;
    int                             warmup_loops;
    int                             samples_per_sec;
    long                            *histogram_min;
    long                            *histogram_max;
    long                            *histogram_avg;
    int                             histogram_size;
    int                             bucketsize;

    rtdm_task_t                     timer_task;

    xntimer_t                       timer;
    int                             warmup;
    uint64_t                        start_time;
    uint64_t                        date;
    struct rttst_bench_res          curr;

    rtdm_event_t                    result_event;
    struct rttst_interm_bench_res   result;

    struct semaphore                nrt_mutex;
};

static unsigned int start_index;

module_param(start_index, uint, 0400);
MODULE_PARM_DESC(start_index, "First device instance number to be used");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("jan.kiszka@web.de");


static inline void add_histogram(struct rt_tmbench_context *ctx,
                                 long *histogram, long addval)
{
    /* bucketsize steps */
    long inabs = (addval >= 0 ? addval : -addval) / ctx->bucketsize;
    histogram[inabs < ctx->histogram_size ? inabs : ctx->histogram_size-1]++;
}

static inline long long slldiv(long long s, unsigned d)
{
    return s >= 0 ? xnarch_ulldiv(s,d,NULL) : -xnarch_ulldiv(-s,d,NULL);
}

void eval_inner_loop(struct rt_tmbench_context *ctx, long dt)
{
    if (ctx->date <= ctx->start_time)
        ctx->curr.overruns++;

    if (dt > ctx->curr.max)
        ctx->curr.max = dt;
    if (dt < ctx->curr.min)
        ctx->curr.min = dt;
    ctx->curr.avg += dt;

#ifdef CONFIG_IPIPE_TRACE
    if (ctx->freeze_max && (dt > ctx->result.overall.max) && !ctx->warmup) {
        ipipe_trace_frozen_reset();
        ipipe_trace_freeze(dt);
        ctx->result.overall.max = dt;
    }
#endif /* CONFIG_IPIPE_TRACE */

    ctx->date += ctx->period;

    if (!ctx->warmup && ctx->histogram_size)
        add_histogram(ctx, ctx->histogram_avg, dt);
}


void eval_outer_loop(struct rt_tmbench_context *ctx)
{
    if (!ctx->warmup) {
        if (ctx->histogram_size) {
            add_histogram(ctx, ctx->histogram_max, ctx->curr.max);
            add_histogram(ctx, ctx->histogram_min, ctx->curr.min);
        }

        ctx->result.last.min = ctx->curr.min;
        if (ctx->curr.min < ctx->result.overall.min)
            ctx->result.overall.min = ctx->curr.min;

        ctx->result.last.max = ctx->curr.max;
        if (ctx->curr.max > ctx->result.overall.max)
            ctx->result.overall.max = ctx->curr.max;

        ctx->result.last.avg = slldiv(ctx->curr.avg, ctx->samples_per_sec);
        ctx->result.overall.avg      += ctx->result.last.avg;
        ctx->result.overall.overruns += ctx->curr.overruns;
        rtdm_event_pulse(&ctx->result_event);
    }

    if (ctx->warmup &&
        (ctx->result.overall.test_loops == ctx->warmup_loops)) {
        ctx->result.overall.test_loops = 0;
        ctx->warmup = 0;
    }

    ctx->curr.min        = 10000000;
    ctx->curr.max        = -10000000;
    ctx->curr.avg        = 0;
    ctx->curr.overruns   = 0;

    ctx->result.overall.test_loops++;
}


void timer_task_proc(void *arg)
{
    struct rt_tmbench_context   *ctx = (struct rt_tmbench_context *)arg;
    int                         count;


    /* start time: one millisecond from now. */
    ctx->date = rtdm_clock_read() + 1000000;

    while (1) {
        int err;


        for (count = 0; count < ctx->samples_per_sec; count++) {
            RTDM_EXECUTE_ATOMICALLY(
                ctx->start_time = rtdm_clock_read();
                err = rtdm_task_sleep_until(ctx->date);
            );

            if (err)
                return;

            eval_inner_loop(ctx, (long)(rtdm_clock_read() - ctx->date));
        }
        eval_outer_loop(ctx);
    }
}


void timer_proc(xntimer_t *timer)
{
    struct rt_tmbench_context   *ctx =
        container_of(timer, struct rt_tmbench_context, timer);


    eval_inner_loop(ctx, (long)(rtdm_clock_read() - ctx->date));

    ctx->start_time = rtdm_clock_read();
    /* FIXME: convert to RTDM timers */
    xntimer_start(&ctx->timer, xntbase_ns2ticks(rtdm_tbase, ctx->date),
                  XN_INFINITE, XN_ABSOLUTE);

    if (++ctx->curr.test_loops < ctx->samples_per_sec)
        return;

    ctx->curr.test_loops = 0;
    eval_outer_loop(ctx);
}


int rt_tmbench_open(struct rtdm_dev_context *context,
                    rtdm_user_info_t *user_info, int oflags)
{
    struct rt_tmbench_context   *ctx;


    ctx = (struct rt_tmbench_context *)context->dev_private;

    ctx->mode = -1;
    init_MUTEX(&ctx->nrt_mutex);

    return 0;
}


int rt_tmbench_close(struct rtdm_dev_context *context,
                     rtdm_user_info_t *user_info)
{
    struct rt_tmbench_context   *ctx;


    ctx = (struct rt_tmbench_context *)context->dev_private;

    down(&ctx->nrt_mutex);

    if (ctx->mode >= 0) {
        if (ctx->mode == RTTST_TMBENCH_TASK)
            rtdm_task_destroy(&ctx->timer_task);
        else
            /* FIXME: convert to RTDM timers */
            xntimer_destroy(&ctx->timer);

        rtdm_event_destroy(&ctx->result_event);

        if (ctx->histogram_size)
            kfree(ctx->histogram_min);

        ctx->mode           = -1;
        ctx->histogram_size = 0;
    }

    up(&ctx->nrt_mutex);

    return 0;
}


int rt_tmbench_ioctl_nrt(struct rtdm_dev_context *context,
                         rtdm_user_info_t *user_info,
                         unsigned int request, void *arg)
{
    struct rt_tmbench_context   *ctx;
    int                         err = 0;


    ctx = (struct rt_tmbench_context *)context->dev_private;

    switch (request) {
        case RTTST_RTIOC_TMBENCH_START: {
            struct rttst_tmbench_config config_buf;
            struct rttst_tmbench_config *config;


            config = (struct rttst_tmbench_config *)arg;
            if (user_info) {
                if (rtdm_safe_copy_from_user(user_info, &config_buf, arg,
                                    sizeof(struct rttst_tmbench_config)) < 0)
                    return -EFAULT;

                config = &config_buf;
            }

            down(&ctx->nrt_mutex);

            ctx->period          = config->period;
            ctx->warmup_loops    = config->warmup_loops;
            ctx->samples_per_sec = 1000000000 / ctx->period;
            ctx->histogram_size  = config->histogram_size;
            ctx->freeze_max      = config->freeze_max;

            if (ctx->histogram_size > 0) {
                ctx->histogram_min =
                    kmalloc(3 * ctx->histogram_size * sizeof(long),
                            GFP_KERNEL);
                ctx->histogram_max =
                    ctx->histogram_min + config->histogram_size;
                ctx->histogram_avg =
                    ctx->histogram_max + config->histogram_size;

                if (!ctx->histogram_min) {
                    up(&ctx->nrt_mutex);
                    return -ENOMEM;
                }

                memset(ctx->histogram_min, 0,
                       3 * ctx->histogram_size * sizeof(long));
                ctx->bucketsize = config->histogram_bucketsize;
            }

            ctx->result.overall.min        = 10000000;
            ctx->result.overall.max        = -10000000;
            ctx->result.overall.avg        = 0;
            ctx->result.overall.test_loops = 1;
            ctx->result.overall.overruns   = 0;

            ctx->warmup = 1;

            ctx->curr.min        = 10000000;
            ctx->curr.max        = -10000000;
            ctx->curr.avg        = 0;
            ctx->curr.overruns   = 0;

            rtdm_event_init(&ctx->result_event, 0);

            if (config->mode == RTTST_TMBENCH_TASK) {
                if (!test_bit(RTDM_CLOSING, &context->context_flags)) {
                    ctx->mode = RTTST_TMBENCH_TASK;
                    err = rtdm_task_init(&ctx->timer_task, "timerbench",
                                         timer_task_proc, ctx,
                                         config->priority, 0);
                }
            } else {
                /* FIXME: convert to RTDM timers */
		xntimer_init(&ctx->timer, rtdm_tbase, timer_proc);

                ctx->curr.test_loops = 0;

                if (!test_bit(RTDM_CLOSING, &context->context_flags)) {
                    ctx->mode = RTTST_TMBENCH_HANDLER;
                    RTDM_EXECUTE_ATOMICALLY(
                        /* start time: one millisecond from now. */
                        ctx->start_time = rtdm_clock_read() + 1000000;
                        ctx->date       = ctx->start_time + ctx->period;

                        /* FIXME: convert to RTDM timers */
                        xntimer_start(&ctx->timer, xntbase_ns2ticks(rtdm_tbase, ctx->date),
                                      XN_INFINITE, XN_ABSOLUTE);
                    );
                }
            }

            up(&ctx->nrt_mutex);

            break;
        }

        case RTTST_RTIOC_TMBENCH_STOP: {
            struct rttst_overall_bench_res *usr_res;


            usr_res = (struct rttst_overall_bench_res *)arg;

            down(&ctx->nrt_mutex);

            if (ctx->mode < 0) {
                up(&ctx->nrt_mutex);
                return -EINVAL;
            }

            if (ctx->mode == RTTST_TMBENCH_TASK)
                rtdm_task_destroy(&ctx->timer_task);
            else
                /* FIXME: convert to RTDM timers */
                xntimer_destroy(&ctx->timer);

            rtdm_event_destroy(&ctx->result_event);

            ctx->mode = -1;

            ctx->result.overall.avg =
                slldiv(ctx->result.overall.avg,
                       ((ctx->result.overall.test_loops) > 1 ?
                       ctx->result.overall.test_loops : 2) - 1);

            if (user_info)
                err = rtdm_safe_copy_to_user(user_info, &usr_res->result,
                                             &ctx->result.overall,
                                             sizeof(struct rttst_bench_res));
                /* Do not break on error here - we may have to free a
                   histogram buffer first. */
            else
                memcpy(&usr_res->result, &ctx->result.overall,
                       sizeof(struct rttst_bench_res));

            if (ctx->histogram_size > 0) {
                int size = ctx->histogram_size * sizeof(long);

                if (user_info) {
                    if ((rtdm_safe_copy_to_user(user_info,
                                                usr_res->histogram_min,
                                                ctx->histogram_min,
                                                size) < 0) ||
                        (rtdm_safe_copy_to_user(user_info,
                                                usr_res->histogram_max,
                                                ctx->histogram_max,
                                                size) < 0) ||
                        (rtdm_safe_copy_to_user(user_info,
                                                usr_res->histogram_avg,
                                                ctx->histogram_avg,
                                                size) < 0))
                        err = -EFAULT;
                } else {
                    memcpy(usr_res->histogram_min, ctx->histogram_min, size);
                    memcpy(usr_res->histogram_max, ctx->histogram_max, size);
                    memcpy(usr_res->histogram_avg, ctx->histogram_avg, size);
                }

                kfree(ctx->histogram_min);
            }

            up(&ctx->nrt_mutex);

            break;
        }

        case RTTST_RTIOC_INTERM_BENCH_RES:
            err = -ENOSYS;
            break;

        default:
            err = -ENOTTY;
    }

    return err;
}


int rt_tmbench_ioctl_rt(struct rtdm_dev_context *context,
                        rtdm_user_info_t *user_info,
                        unsigned int request, void *arg)
{
    struct rt_tmbench_context   *ctx;
    int                         err = 0;


    ctx = (struct rt_tmbench_context *)context->dev_private;

    switch (request) {
        case RTTST_RTIOC_INTERM_BENCH_RES: {
            struct rttst_interm_bench_res *usr_res;


            usr_res = (struct rttst_interm_bench_res *)arg;

            err = rtdm_event_wait(&ctx->result_event);
            if (err)
                return err;

            if (user_info)
                err = rtdm_safe_copy_to_user(user_info, usr_res, &ctx->result,
                                       sizeof(struct rttst_interm_bench_res));
            else
                memcpy(usr_res, &ctx->result,
                       sizeof(struct rttst_interm_bench_res));

            break;
        }

        case RTTST_RTIOC_TMBENCH_START:
        case RTTST_RTIOC_TMBENCH_STOP:
            err = -ENOSYS;
            break;

        default:
            err = -ENOTTY;
    }

    return err;
}


static struct rtdm_device device = {
    struct_version:     RTDM_DEVICE_STRUCT_VER,

    device_flags:       RTDM_NAMED_DEVICE,
    context_size:       sizeof(struct rt_tmbench_context),
    device_name:        "",

    open_rt:            NULL,
    open_nrt:           rt_tmbench_open,

    ops: {
        close_rt:       NULL,
        close_nrt:      rt_tmbench_close,

        ioctl_rt:       rt_tmbench_ioctl_rt,
        ioctl_nrt:      rt_tmbench_ioctl_nrt,

        read_rt:        NULL,
        read_nrt:       NULL,

        write_rt:       NULL,
        write_nrt:      NULL,

        recvmsg_rt:     NULL,
        recvmsg_nrt:    NULL,

        sendmsg_rt:     NULL,
        sendmsg_nrt:    NULL,
    },

    device_class:       RTDM_CLASS_TESTING,
    device_sub_class:   RTDM_SUBCLASS_TIMERBENCH,
    profile_version:    RTTST_PROFILE_VER,
    driver_name:        "xeno_timerbench",
    driver_version:     RTDM_DRIVER_VER(0, 2, 1),
    peripheral_name:    "Timer Latency Benchmark",
    provider_name:      "Jan Kiszka",
    proc_name:          device.device_name,
};

int __init __timerbench_init(void)
{
    int err;

    do {
        snprintf(device.device_name, RTDM_MAX_DEVNAME_LEN, "rttest%d",
                 start_index);
        err = rtdm_dev_register(&device);

        start_index++;
    } while (err == -EEXIST);

    return err;
}


void __timerbench_exit(void)
{
    rtdm_dev_unregister(&device, 1000);
}


module_init(__timerbench_init);
module_exit(__timerbench_exit);

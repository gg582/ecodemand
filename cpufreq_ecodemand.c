// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/cpufreq/cpufreq_ecodemand.c
 * ecodemand: Hybrid schedutil/conservative governor
 *
 * Copyright (C) 2025 Lee Yunjin <gzblues61@daum.net>
 *
 * Overview:
 * - Load is computed in a frequency-invariant way similar to schedutil:
 *   load = raw_cpu_usage * cur_freq / max_freq
 * - Frequency selection follows a conservative, step-based policy for
 *   both up and down transitions.
 * - No AC/battery split; the governor applies a single policy.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/cpufreq.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/tick.h>
#include <linux/sched/cpufreq.h>
#include <linux/units.h>
#include <linux/delay.h>

/* Tunables defaults */
#define DEF_UP_THRESHOLD		80
#define DEF_DOWN_THRESHOLD		20
#define DEF_FREQ_STEP			5
#define DEF_SAMPLING_RATE		10000 /* 10 ms in us */
#define DEF_SAMPLING_DOWN_FACTOR	1
#define DEF_POWERSAVE_BIAS		0

struct eco_cpu_stats {
	u64 prev_total_time;
	u64 prev_busy_time;
};

struct eco_tuners {
	unsigned int up_threshold;
	unsigned int down_threshold;
	unsigned int freq_step;
	unsigned int sampling_rate; /* in us */
	unsigned int sampling_down_factor;
	int powersave_bias; /* -100 to 100, applied to calculated load */
};

struct eco_policy_dbs {
	struct cpufreq_policy *policy;
	struct eco_tuners tuners;
	struct mutex timer_mutex;
	struct delayed_work work;
	struct eco_cpu_stats *cpu_stats; /* Per-CPU stats for this policy */
	unsigned int down_count;
};

static void eco_dbs_timer(struct work_struct *work);

/*
 * eco_calculate_load - frequency-invariant load
 *
 * For the policy's CPU mask, compute a single frequency-invariant
 * utilization value. The idle accounting follows the existing
 * cpufreq governors and schedutil.
 *
 * Load = (busy_time / total_time) * (cur_freq / max_freq) * 100
 */
static unsigned int eco_calculate_load(struct eco_policy_dbs *dbs)
{
	struct cpufreq_policy *policy = dbs->policy;
	unsigned int cpu;
	u64 total_busy_time = 0;
	u64 total_wall_time = 0;
	unsigned int load = 0;

	for_each_cpu(cpu, policy->cpus) {
		struct eco_cpu_stats *st = &dbs->cpu_stats[cpu];
		u64 cur_wall_time, cur_idle_time;
		u64 dt, db;

		cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, 1);
		if (cur_wall_time == 0) /* Fallback if arch does not support jiffy-based API */
			cur_idle_time = get_cpu_idle_time_us(cpu, &cur_wall_time);

		dt = cur_wall_time - st->prev_total_time;
		db = dt - (cur_idle_time - (st->prev_total_time - st->prev_busy_time));

		/* Update stats for next sample */
		st->prev_total_time = cur_wall_time;
		st->prev_busy_time = cur_wall_time - cur_idle_time;

		if (dt > 0) {
			total_busy_time += db;
			total_wall_time += dt;
		}
	}

	if (likely(total_wall_time > 0)) {
		unsigned int raw_usage;

		/* Raw usage in [0, 100] */
		raw_usage = div64_u64(total_busy_time * 100, total_wall_time);

		/* Scale by current vs. maximum frequency */
		load = (raw_usage * policy->cur) / policy->max;
	}

	return load;
}

static void eco_dbs_update(struct eco_policy_dbs *dbs)
{
	struct cpufreq_policy *policy = dbs->policy;
	unsigned int load;
	unsigned int next_freq = policy->cur;
	unsigned int freq_step_khz;
	int effective_load;

	load = eco_calculate_load(dbs);

	/*
	 * Apply powersave bias:
	 * Positive bias moves the effective load downward (more power saving),
	 * negative bias upward (more performance). The result is clamped to
	 * [0, 100].
	 */
	effective_load = (int)load - dbs->tuners.powersave_bias;
	if (effective_load < 0)
		effective_load = 0;
	if (effective_load > 100)
		effective_load = 100;

	/* Step size relative to policy->max */
	freq_step_khz = (policy->max * dbs->tuners.freq_step) / 100;
	if (freq_step_khz == 0)
		freq_step_khz = 1000; /* minimum 1 MHz step */

	/* Conservative-style step-up / step-down logic */
	if (effective_load > dbs->tuners.up_threshold) {
		dbs->down_count = 0;
		if (next_freq < policy->max) {
			next_freq += freq_step_khz;
			if (next_freq > policy->max)
				next_freq = policy->max;
			__cpufreq_driver_target(policy, next_freq, CPUFREQ_RELATION_H);
		}
	} else if (effective_load < dbs->tuners.down_threshold) {
		dbs->down_count++;
		if (dbs->down_count >= dbs->tuners.sampling_down_factor) {
			if (next_freq > policy->min) {
				if (next_freq > freq_step_khz)
					next_freq -= freq_step_khz;
				else
					next_freq = policy->min;

				if (next_freq < policy->min)
					next_freq = policy->min;

				__cpufreq_driver_target(policy, next_freq,
							CPUFREQ_RELATION_L);
			}
			dbs->down_count = 0;
		}
	} else {
		/* Within the hysteresis band, keep current frequency */
		dbs->down_count = 0;
	}
}

static void eco_dbs_timer(struct work_struct *work)
{
	struct eco_policy_dbs *dbs =
		container_of(to_delayed_work(work),
			     struct eco_policy_dbs, work);
	struct cpufreq_policy *policy = dbs->policy;

	mutex_lock(&dbs->timer_mutex);

	if (!policy) {
		mutex_unlock(&dbs->timer_mutex);
		return;
	}

	eco_dbs_update(dbs);

	mutex_unlock(&dbs->timer_mutex);

	schedule_delayed_work(&dbs->work,
			      usecs_to_jiffies(dbs->tuners.sampling_rate));
}

static int eco_init(struct cpufreq_policy *policy)
{
	struct eco_policy_dbs *dbs;
	unsigned int cpu;

	dbs = kzalloc(sizeof(*dbs), GFP_KERNEL);
	if (!dbs)
		return -ENOMEM;

	dbs->cpu_stats = kcalloc(nr_cpu_ids, sizeof(struct eco_cpu_stats),
				 GFP_KERNEL);
	if (!dbs->cpu_stats) {
		kfree(dbs);
		return -ENOMEM;
	}

	/* Initialize tunables */
	dbs->tuners.up_threshold = DEF_UP_THRESHOLD;
	dbs->tuners.down_threshold = DEF_DOWN_THRESHOLD;
	dbs->tuners.freq_step = DEF_FREQ_STEP;
	dbs->tuners.sampling_rate = DEF_SAMPLING_RATE;
	dbs->tuners.sampling_down_factor = DEF_SAMPLING_DOWN_FACTOR;
	dbs->tuners.powersave_bias = DEF_POWERSAVE_BIAS;

	dbs->policy = policy;
	dbs->down_count = 0;
	mutex_init(&dbs->timer_mutex);
	INIT_DELAYED_WORK(&dbs->work, eco_dbs_timer);

	/* Initialize per-CPU accounting state */
	for_each_cpu(cpu, policy->cpus) {
		struct eco_cpu_stats *st = &dbs->cpu_stats[cpu];
		u64 cur_wall_time;
		u64 cur_idle = get_cpu_idle_time(cpu, &cur_wall_time, 1);

		if (cur_wall_time == 0)
			cur_idle = get_cpu_idle_time_us(cpu, &cur_wall_time);

		st->prev_total_time = cur_wall_time;
		st->prev_busy_time = cur_wall_time - cur_idle;
	}

	policy->governor_data = dbs;
	return 0;
}

static void eco_exit(struct cpufreq_policy *policy)
{
	struct eco_policy_dbs *dbs = policy->governor_data;

	if (!dbs)
		return;

	cancel_delayed_work_sync(&dbs->work);

	kfree(dbs->cpu_stats);
	kfree(dbs);
	policy->governor_data = NULL;
}

static int eco_start(struct cpufreq_policy *policy)
{
	struct eco_policy_dbs *dbs = policy->governor_data;

	if (!dbs)
		return -EINVAL;

	/* Initial activation */
	schedule_delayed_work(&dbs->work,
			      usecs_to_jiffies(dbs->tuners.sampling_rate));
	return 0;
}

static void eco_stop(struct cpufreq_policy *policy)
{
	struct eco_policy_dbs *dbs = policy->governor_data;

	if (dbs)
		cancel_delayed_work_sync(&dbs->work);
}

static struct cpufreq_governor cpufreq_gov_ecodemand = {
	.name			= "ecodemand",
	.init			= eco_init,
	.exit			= eco_exit,
	.start			= eco_start,
	.stop			= eco_stop,
	/* .limits is optional; left as NULL */
	.owner			= THIS_MODULE,
};

static int __init ecodemand_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_ecodemand);
}

static void __exit ecodemand_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_ecodemand);
}

MODULE_AUTHOR("Lee Yunjin <gzblues61@daum.net>");
MODULE_DESCRIPTION("ecodemand cpufreq governor (frequency-invariant, step-based)");
MODULE_LICENSE("GPL");

module_init(ecodemand_init);
module_exit(ecodemand_exit);

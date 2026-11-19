# CPU and memory-bus tuning, from Android's /vendor/bin/init.kernel.post_boot-volcano_default_4_3_1.sh (this phone's
# 4+3+1 clusters), which Android's init runs at boot completed and HarmonyOS never does. Without it the kernel keeps
# the boot command line's cpufreq.default_governor=performance: every core at its top frequency all the time (a warm
# phone at ~2% load), and no touch boost. booted.sh runs this once the desktop is up.
# Left out: Android's cpusets (HarmonyOS has its own), the soc0 image info and getprop, printk, and the DDR mbps zones
# (Android's DDR type test never matches this phone's " 07000000" either).
w() { [ -e "$2" ] && echo "$1" > "$2"; }
P=/sys/devices/system/cpu
W=/proc/sys/walt

# A long-running RT task may run 1.2 s.
w 1350000 /proc/sys/kernel/sched_rt_period_us
w 1250000 /proc/sys/kernel/sched_rt_runtime_us

if [ -d $W ]; then
    w 1190400 $W/sched_max_freq_partial_halt
    # Core control: silver always on; gold keeps two cores, prime may go offline.
    w 0xFF $P/cpu0/core_ctl/nrrun_cpu_mask
    w 0x00 $P/cpu0/core_ctl/nrrun_cpu_misfit_mask
    w 0x00 $P/cpu0/core_ctl/assist_cpu_mask
    w 0x00 $P/cpu0/core_ctl/assist_cpu_misfit_mask
    w 2 $P/cpu4/core_ctl/min_cpus
    w 60 $P/cpu4/core_ctl/busy_up_thres
    w 30 $P/cpu4/core_ctl/busy_down_thres
    w 100 $P/cpu4/core_ctl/offline_delay_ms
    w 3 $P/cpu4/core_ctl/task_thres
    w "0 0 0" $P/cpu4/core_ctl/not_preferred
    w 0xF0 $P/cpu4/core_ctl/nrrun_cpu_mask
    w 0x0F $P/cpu4/core_ctl/nrrun_cpu_misfit_mask
    w 0x00 $P/cpu4/core_ctl/assist_cpu_mask
    w 0x00 $P/cpu4/core_ctl/assist_cpu_misfit_mask
    w 0 $P/cpu7/core_ctl/min_cpus
    w 60 $P/cpu7/core_ctl/busy_up_thres
    w 30 $P/cpu7/core_ctl/busy_down_thres
    w 100 $P/cpu7/core_ctl/offline_delay_ms
    w 1 $P/cpu7/core_ctl/task_thres
    w 1 $P/cpu7/core_ctl/not_preferred
    w 0x80 $P/cpu7/core_ctl/nrrun_cpu_mask
    w 0x70 $P/cpu7/core_ctl/nrrun_cpu_misfit_mask
    w 0x70 $P/cpu7/core_ctl/assist_cpu_mask
    w 0x0F $P/cpu7/core_ctl/assist_cpu_misfit_mask
    w 0 $P/cpu0/core_ctl/enable
    w 1 $P/cpu4/core_ctl/enable
    w 1 $P/cpu7/core_ctl/enable
    # Task placement.
    w "65 85" $W/sched_downmigrate
    w "71 95" $W/sched_upmigrate
    w 85 $W/sched_group_downmigrate
    w 100 $W/sched_group_upmigrate
    w 1 $W/sched_walt_rotate_big_tasks
    w 51 $W/sched_min_task_util_for_boost
    w 35 $W/sched_min_task_util_for_colocation
    w 0 $W/sched_coloc_busy_hysteresis_enable_cpus
    w "8500000 8500000 8500000 8500000 5000000 5000000 5000000 2000000" $W/sched_util_busy_hyst_cpu_ns
    w 255 $W/sched_util_busy_hysteresis_enable_cpus
    w "1 1 1 1 15 15 15 15" $W/sched_util_busy_hyst_cpu_util
    w 40 $W/sched_cluster_util_thres_pct
    w 30 $W/sched_idle_enough
    w 112 $W/sched_pipeline_cpus
    w 325 $W/walt_low_latency_task_threshold
    w "1708800 2208000 2147483647" $W/sched_fmax_cap
    w 0 $W/sched_boost
    # Touch boost: silver to 1.1 GHz for 120 ms on input.
    w "1113600 0 0 0 0 0 0 0" $W/input_boost/input_boost_freq
    w 120 $W/input_boost/input_boost_ms
    for p in 0 4 7; do
        w walt $P/cpufreq/policy$p/scaling_governor
        w 0 $P/cpufreq/policy$p/walt/down_rate_limit_us
        w 0 $P/cpufreq/policy$p/walt/up_rate_limit_us
        w 0 $P/cpufreq/policy$p/walt/pl
    done
    w 1 $W/sched_conservative_pl
    w 595200 $P/cpufreq/policy0/walt/rtg_boost_freq
    w 0 $P/cpufreq/policy4/walt/rtg_boost_freq
    w 0 $P/cpufreq/policy7/walt/rtg_boost_freq
    w 1113600 $P/cpufreq/policy0/walt/hispeed_freq
    w 1190400 $P/cpufreq/policy4/walt/hispeed_freq
    w 1459200 $P/cpufreq/policy7/walt/hispeed_freq
    w 85 $P/cpufreq/policy4/walt/hispeed_load
    w 85 $P/cpufreq/policy7/walt/hispeed_load
else
    for p in 0 4 7; do w schedutil $P/cpufreq/policy$p/scaling_governor; done
    w 1 /proc/sys/kernel/sched_pelt_multiplier
fi
w 595200 $P/cpufreq/policy0/scaling_min_freq
w 633600 $P/cpufreq/policy4/scaling_min_freq
w 633600 $P/cpufreq/policy7/scaling_min_freq
w 0 /proc/sys/kernel/sched_util_clamp_min_rt_default

# Memory bus: boost floors at the hardware minimum, and the DDR bandwidth monitor's sampling.
B=$P/bus_dcvs
for d in $B/*; do [ -f $d/hw_min_freq ] && cat $d/hw_min_freq > $d/boost_freq; done
for d in $B/DDR/*bwmon-ddr; do
    w 4 $d/sample_ms
    w 68 $d/io_percent
    w 20 $d/hist_memory
    w 80 $d/down_thres
    w 0 $d/guard_band_mbps
    w 250 $d/up_scale
    w 1600 $d/idle_mbps
    w 48 $d/window_ms
done
for d in $B/L3/*gold; do w 4000 $d/ipm_ceil; w 60 $d/wb_pct_thres; done
for d in $B/DDRQOS/*gold; do w 4000 $d/ipm_ceil; done
w s2idle /sys/power/mem_sleep
w N $P/qcom_lpm/parameters/sleep_disabled
echo "hmos-cpu-tuning: governors $(cat $P/cpufreq/policy*/scaling_governor | tr '\n' ' ')" > /dev/kmsg

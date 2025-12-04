# Ecodemand CPUFreq Governor

`ecodemand` is a custom CPU frequency scaling governor for the Linux kernel. It is designed as a hybrid, taking inspiration from both the `schedutil` and `conservative` governors to offer a balance between performance responsiveness and power efficiency.

## How it Works

The governor's logic is based on two main principles:

1.  **Frequency-Invariant Load Calculation:** Like `schedutil`, `ecodemand` calculates CPU load independently of the current operating frequency. This provides a more accurate measure of the actual demand on the processor. The load is calculated as:
    `load = (raw_cpu_usage * current_frequency) / maximum_frequency`

2.  **Step-Based Frequency Scaling:** Similar to the `conservative` governor, `ecodemand` adjusts the CPU frequency in incremental steps rather than jumping directly to a target frequency. This provides smoother transitions and can help avoid rapid frequency oscillations.

It operates on a single policy, applying the same logic whether the system is on AC power or battery.

## Features

-   Hybrid `schedutil`/`conservative` design.
-   Frequency-invariant CPU load tracking.
-   Smooth, step-based frequency adjustments for both scaling up and down.
-   Tunable parameters for fine-grained control (e.g., thresholds, sampling rate).
-   A `powersave_bias` option to favor either power savings or performance.

## Installation

This module is designed to be managed with DKMS (Dynamic Kernel Module Support).

1.  **Install DKMS:** Ensure you have `dkms` and the appropriate kernel headers installed for your distribution.

2.  **Run the Installer:**
    ```bash
    sudo ./install.sh
    ```
    This script will copy the source files to `/usr/src/ecodemand-1.0` and use DKMS to build and install the module for your current kernel.

## Usage

Once the module is loaded, you can enable the `ecodemand` governor for all CPU cores with the following command:

```bash
for policy in /sys/devices/system/cpu/cpufreq/policy*; do
    echo "ecodemand" | sudo tee "${policy}/scaling_governor"
done
```

You can verify that it is active by checking the contents of the `scaling_governor` file.

## License

This software is licensed under the **GNU General Public License v2.0**.

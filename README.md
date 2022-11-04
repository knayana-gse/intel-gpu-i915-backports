
#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported i915 Source Code of intel GPUs on various OSV Kernels. You can create Dynamic Kernel Module Support(DKMS) based packages, which can be installed on supported OS distributions.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.

This repo is a code snapshot of particular version of backports and does not contain individual git change history.

# Dependencies

This driver is part of a collection of kernel-mode drivers that enable support for Intel graphics. The backports collection within https://github.com/intel-gpu includes:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
- [Intel® Converged Security Engine Backports](https://github.com/intel-gpu/intel-gpu-cse-backports) - Converged Security Engine
- [Intel® Platform Monitoring Technology Backports](https://github.com/intel-gpu/intel-gpu-pmt-backports/) - Intel Platform Telemetry
- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.


# Branches
backport/main will point to the currently supported version of Ubuntu® and SLES.

# Supported OS Kernel/Distribution
  Our current backport supports the following OS Distribution.

| OS Distribution | OS Version | Kernel Version  | Installation Instructions | 
|---  |---  |---  |--- |
| Ubuntu® | 22.04 | Kernel 5.17 oem | [UBUNTU_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/UBUNTU_README.md) |
| | 20.04 |  Kernel 5.15 generic | [UBUNTU_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/UBUNTU_README.md) |
| SLES | 15SP4 | Kernel 5.14 |  [SLES_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/SLES_README.md) |


#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported i915 Source Code of intel GPUs on various OSV Kernels. You can create Dynamic Kernel Module Support(DKMS) based packages, which can be installed on supported OS distributions.

We are using [backport project](https://backports.wiki.kernel.org/index.php/Main_Page) to generate out of tree i915 kernel module source codes.

This repo is a code snapshot of particular version of backports and does not contain individual git change history.

# Dependencies

This driver is part of a collection of kernel-mode drivers that enable support for Intel graphics. The backports collection within https://github.com/intel-gpu includes:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
- [Intel® Converged Security Engine Backports](https://github.com/intel-gpu/intel-gpu-cse-backports) - Converged Security Engine
- [Intel® Platform Monitoring Technology Backports](https://github.com/intel-gpu/intel-gpu-pmt-backports/) - Intel Platform Telemetry
- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag.


# Branches
backport/main will point to the currently supported version of Ubuntu® and SLES.

# Supported OS Kernel/Distribution
  Our current backport supports the following OS Distribution.

| OS Distribution | OS Version | Kernel Version  | Installation Instructions |
|---  |---  |---  |--- |
| Ubuntu® | 22.04 | Kernel 5.17 oem | [UBUNTU_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/UBUNTU_README.md) |
| | 22.04/20.04 |  Kernel 5.15 generic | [UBUNTU_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/UBUNTU_README.md) |
| | Mainline |  Kernel 5.15.72 | [UBUNTU_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/UBUNTU_README.md) |
| SLES | 15SP4 | Kernel 5.14 |  [SLES_README](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/SLES_README.md) |

# Key Released DKMS versions

| DKMS Released Tag | OS Version | Kernel Version  | DKMS Building | CI Testing |
|---  |---  |---  |--- |--- |
|I915_22WW45.5_476.14_6213_220914.2 |Ubuntu 22.04 | Kernel 5.17 oem | Yes |Yes |
| | Ubuntu 22.04/20.04 |  Kernel 5.15 generic |Yes |Yes |
| | Mainline |  Kernel 5.15.72 |Yes |No |
|I915_22WW45.5_476.14_6213_220914.2 | SLES15SP4 | Kernel 5.14 | Yes | Yes |

#  Intel® Graphics Driver Backports for Linux® OS (intel-gpu-i915-backports)

Contains the backported i915 Source Code of intel GPUs on various OSV Kernels.
You can create Dynamic Kernel Module Support(DKMS) based packages, which can be installed on supported OS distributions.

These out of tree i915 kernel module source codes are generated by the backpoprt project.
"https://backports.wiki.kernel.org/index.php/Main_Page" 

This repo is a code snapshot of particular version of backports and does not contain individual git change history
# Dependencies

This driver is part of a collection of kernel-mode drivers that enable support for Intel graphics. The backports collection within https://github.com/intel-gpu includes:

- [Intel® Graphics Driver Backports for Linux](https://github.com/intel-gpu/intel-gpu-i915-backports) - The main graphics driver (includes a compatible DRM subsystem and dmabuf if necessary)
- [Intel® Converged Security Engine Backports](https://github.com/intel-gpu/intel-gpu-cse-backports) - Converged Security Engine
- [Intel® Platform Monitoring Technology Backports](https://github.com/intel-gpu/intel-gpu-pmt-backports/) - Intel Platform Telemetry
- [Intel® GPU firmware](https://github.com/intel-gpu/intel-gpu-firmware) - Firmware required by intel GPUs.

Each project is tagged consistently, so when pulling these repos, pull the same tag. 

# Supported OS Distributions

|   OSV |Branch         | Installation Instructions | Building | Testing|
|---    |---    | --- | --- | --- |
| Red Hat® Enterprise Linux® 8.6       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/README.md)| Yes | Yes |
| Red Hat® Enterprise Linux® 8.5       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/README.md)| Yes | No |
| Mainline Kernel (5.10 LTS)       | [redhat/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/redhat/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/redhat/main/README.md)| Yes | No |
| SUSE® Linux® Enterprise Server 15SP3 | [suse/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/suse/main) |[Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/suse/main/README.md)| Yes | Yes |
| SUSE® Linux® Enterprise Server 15SP4 | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_sles.md)| Yes | Yes |
| Ubuntu® 22.04 (linux-oem image 5.17)  | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | No |
| Ubuntu® 20.04 (5.15 generic)  | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | Yes |
| Mainline Kernel (5.15 LTS) | [backport/main](https://github.com/intel-gpu/intel-gpu-i915-backports/tree/backport/main) | [Readme](https://github.com/intel-gpu/intel-gpu-i915-backports/blob/backport/main/docs/README_ubuntu.md)| Yes | No |


# Product Releases:
Please refer [Releases](https://dgpu-docs.intel.com/releases/index.html)

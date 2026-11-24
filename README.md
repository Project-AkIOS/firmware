# AkIOS firmware

EDK2 firmware for the Google Pixel 8a (akita, Tensor G3 "zuma"), replacing U-Boot: Google's bootloader starts it from
RAM as it would start Linux. It brings up the display, the UFS storage (HS gear 4) and the A/B slot state, and its boot
manager starts UniXNU from the phone's ESP partition (see the `loader` repository).

- `Silicon/Google/ZumaPkg`: the SoC (memory map helpers, timer, GIC, the UFS driver `ZumaUfsDxe`, the pstore log)
- `Platforms/Google/akitaPkg`: the phone (memory map, drivers list, `AkitaSlotDxe`)
- `Resources/Configs/akita.toml`: where the firmware lives in RAM

Built inside [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) (these packages started from its Exynos 2400
ones); the `akios` repository's `./ak firmware` links them into a Mu-Silicium checkout and builds `out/firmware.img`.

## Licences

Everything here is BSD (BSD-2-Clause-Patent; files from Mu-Silicium keep their notices, see `LICENSE.Mu-Silicium`),
except `Silicon/Google/ZumaPkg/Library/ZumaUfsCalLib`: Samsung's UFS calibration code for zuma and the glue derived
from Google's Linux driver, **GPL-2.0-only** (see the `LICENSE` file there). It is kept apart from the BSD code; a
firmware binary built from this repository therefore carries the GPL's obligations: its source must be offered with it.

# AMD FidelityFX SDK 2.4.0: the FSR API headers

Only the API headers kiln.ui's AMD FSR backend (`native/services/ui/upscale_amd.cpp`) compiles against, unchanged from
`GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` at tag `v2.4.0`, `Kits/FidelityFX/`:
`api/include/ffx_api.h`, `api/include/ffx_api_types.h`, `api/include/vk/ffx_api_vk.h`, `upscalers/include/ffx_upscale.h`.
MIT licence (`LICENSE.txt`). No AMD code or DLL is part of Kiln: the player's launcher downloads AMD's signed DLLs on
request into `<game>/kiln/vendor/amd/2.4.0/`.

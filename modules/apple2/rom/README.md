# Bundled firmware

These binary images are copied unchanged from Reinette II Plus snapshot
`b272efd8e38ba9ff71376d0db3846a417bc304ca`:

| Image | Bytes | SHA-256 |
| --- | ---: | --- |
| [Apple II Plus ROM](https://github.com/ArthurFerreira2/reinette-II-plus/blob/b272efd8e38ba9ff71376d0db3846a417bc304ca/rom/appleII%2B.rom) | 12288 | `fc3e9d41e9428534a883df5aa10eb55b73ea53d2fcbb3ee4f39bed1b07a82905` |
| [Disk II ROM](https://github.com/ArthurFerreira2/reinette-II-plus/blob/b272efd8e38ba9ff71376d0db3846a417bc304ca/rom/diskII.rom) | 256 | `de1e3e035878bab43d0af8fe38f5839c527e9548647036598ee6fe7ec74d2a7d` |

The build embeds both images in the module ELF. Disk images are supplied
separately. The emulator source's MIT notice is retained in `main/vendor`.

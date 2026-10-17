# Block of Sky

A native mod for **Minecraft Bedrock on Android** that adds a block which always renders the sky.

Place it underground, in a box, or inside a build, and the block shows the open sky as if you were looking through a window. Great for underground bases, indoor builds, and cozy sky rooms.

Inspired by the Java Edition mod [Block of Sky](https://www.curseforge.com/minecraft/mc-mods/block-of-sky).

## Features

- Adds the **Block of Sky**
- Always shows the sky, no redstone or setup needed
- Native C++ mod, built for LeviLauncher

## Requirements

- Android 9 or newer, arm64-v8a
- [LeviLauncher](https://github.com/LiteLDev/LeviLaunchroid)
- Minecraft Bedrock **1.26.52.3**

## Installation

1. Install LeviLauncher and select Minecraft 1.26.52.3.
2. Import the Block of Sky package from LeviLauncher's mod manager.
3. Enable it and launch the game through LeviLauncher.

## Building

Requires Android NDK r28c and xmake.

```bash
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake
```

## Notes

- Hooks depend on the game version. Other versions may need updated signatures.
- Use at your own risk and follow Minecraft's terms of service.

## Credits

- Original Java mod: [Block of Sky](https://www.curseforge.com/minecraft/mc-mods/block-of-sky) by LuminaSapphira
- Based on the structure of [BedrockTools](https://github.com/QYCottage/BedrockTools)
- [LeviLauncher / LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid) and its Preloader

## License

MIT, see [LICENSE](LICENSE).

This project is not affiliated with Mojang or Microsoft.

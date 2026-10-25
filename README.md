# YouTube AdBlock + SponsorBlock for PS4

A GoldHEN plugin that skips ads and sponsored segments on the PS4 YouTube Cobalt app.

**Compatible with:** YouTube v02.32 from pkg-zone.com (CUSA01015)

## Features
- ✅ Automatic ad skipping
- ✅ SponsorBlock integration - Skip sponsored segments, intros, outros, and more
- ✅ Native YouTube TV toast notifications
- ✅ Local HTTP proxy server for SponsorBlock API access
- ✅ Clean, silent operation (no debug logs)

## Installation

1. **Download the plugin**
   - Download `yt_adblock.prx` from the [latest release](https://github.com/FourZeroCode/yt_adblock_sponsorblock_ps4/releases)

2. **Copy to PS4**
   - Copy `yt_adblock.prx` to `/data/GoldHEN/plugins/` on your PS4

3. **Configure GoldHEN**
   - Edit `/data/GoldHEN/plugins.ini` to add the following:
   ```ini
   [CUSA01015]
   /data/GoldHEN/plugins/yt_adblock.prx
   ```

4. **Download the configuration file**
   - Download `config.json` from the repository root.

5. **Copy to PS4**
   - Copy `config.json` to `/data/youtube/` on your PS4

6. **Restart YouTube**
   - Close and reopen the YouTube app
   - You should see a "YT AdBlock + SponsorBlock" toast notification

## SponsorBlock Configuration

The optional `config.json` file lets you choose which SponsorBlock categories are enabled and how matching segments are handled.

- **Categories:** To enable a category, include its name in the `categories` array. To disable a category, remove its name from the array. You can add a category again at any time by adding its name back.
- **Skip mode:** Set `skipMode` to `auto` to skip matching segments automatically, or `confirm` to ask for confirmation before skipping.

For example, the `categories` array contains the category names you want enabled, and `skipMode` controls the skipping behavior. By default all categories are enabled and skipped automatically, you can edit `config.json` before copying it to `/data/youtube/` on your PS4.

### Available SponsorBlock Categories

Use these exact category names in the `categories` array. Remove a name to disable that category, or add it back to enable it.

| Category | What it covers |
|---|---|
| `sponsor` | Paid sponsorships and sponsored promotions within a video. |
| `selfpromo` | Creators promoting their own products, services, or projects. |
| `interaction` | Requests to like, subscribe, or otherwise interact with the channel. |
| `intro` | The opening section of a video before its main content. |
| `outro` | The ending section of a video after its main content. |
| `preview` | Previews or teasers of upcoming content within the video. |
| `music_offtopic` | Music that is unrelated to the main topic of the video. |
| `filler` | Filler or tangential content that is not relevant to the main topic. |
| `hook` | An opening hook intended to grab the viewer's attention before the main content. |
| `poi_highlight` | Highlights of a point of interest, such as a notable moment or location. |
| `exclusive_access` | Content showing access or material available exclusively through a promotion or arrangement. |
| `chapter` | Chapter transitions or sections identified as chapter segments. |

Category labels and intended use follow SponsorBlock's category definitions. The exact segments skipped depend on the submitted segment data and how each segment is categorized.

## How it Works

### Ad Skipping
The plugin hooks into the YouTube Cobalt app's HTML script execution engine and injects custom JavaScript that detects and skips ads automatically.

### SponsorBlock Integration
The plugin creates a local HTTP proxy server on `localhost:4040` that:
1. Receives requests from the injected JavaScript
2. Fetches segment data from the SponsorBlock API using PS4's native HTTP library
3. Returns the data to JavaScript
4. Displays a toast with segment information, letting you skip or continue watching, or skips automatically, depending on your configuration.

This approach bypasses Cobalt browser's XHR security restrictions without needing CSP bypass.

## Toast Notifications

You'll see native YouTube TV toast notifications for:
- **Plugin enabled**: When YouTube loads
- **Segments found**: When SponsorBlock segments are detected (e.g., "SponsorBlock: 3 segment(s) found")
- **Segment skipped**: When a segment is automatically skipped (e.g., "Segment Skipped: sponsor (15s)")

## Technical Details

- Hooks `HTMLScriptExecute` at address `0x0097fb40`
- Local HTTP proxy server on port 4040
- Uses PS4 native networking (`sceNet`, `sceHttp`, `sceSsl`)
- JavaScript injection via local buffer for stability
- SponsorBlock API: https://sponsor.ajay.app

## Building from Source

Make sure `$OO_PS4_TOOLCHAIN` is in your PATH (see OpenOrbis for install and setup instructions)

```bash
cd GoldHEN_Plugins_Repository/plugin_src/yt_adblock_sponsorblock_ps4
./build.sh
```

The compiled plugin will be in `../../bin/plugins/prx_final/yt_adblock_sponsorblock_ps4.prx`

### Debug Build

To enable debug logging:
```bash
DEBUG=1 ./build.sh
```

## Credits

- Original yt_adskip_ps4 by [earthonion](https://github.com/earthonion/yt_adskip_ps4)
- Built using the [GoldHEN Plugins SDK](https://github.com/GoldHEN/GoldHEN_Plugins_SDK)
- Built using [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain)
- SponsorBlock API by [Ajay Ramachandran](https://sponsor.ajay.app)
- Toast notification implementation inspired by TizenTube

## License

MIT License - See LICENSE file for details

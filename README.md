<img width="4000" height="1570" alt="PancakeSlicer" src="https://github.com/user-attachments/assets/f6981d62-8efd-4ead-937a-9fa43abb82ce" />

PancakeSlicer is a Self-hostable and Non-planar 3D Printer Slicer with a native web UI built on the OrcaSlicer C++ engine. Try the [demo](https://demo.pancake.cooking/) (pre-sliced sample, no backend engine attached), watch the [video explainer (yt)](https://youtu.be/cRSoVq_reGQ), or read the docs and blog at [pancake.cooking](https://pancake.cooking/).

Run it in a single Docker container (`linux/amd64` and `linux/arm64`, no VNC streaming) or as a desktop app on Windows, macOS, and Linux, with direct send to Bambu Lab (LAN), Klipper (Moonraker), OctoPrint, PrusaLink, Duet, Flashforge, Elegoo Link (Centauri), and Repetier Server. Licensed under AGPL-3.0.

> [!NOTE]
> Heard us at FormNext? Check out the WIP [5-axis support branch](https://github.com/ZimengXiong/PancakeSlicer/tree/dev), view the gcode simulation [here](https://goethelab.pancake.cooking/?demo=floating_pour&size=large) and the PancakeSlicer Advanced demo [here](http://demo5x.pancake.cooking/) (if it's down, try checking back later, still spinning things up)

Come talk about Non-Planar (or PancakeSlicer issues) on [Discord](https://discord.gg/fubwyFYsen). [Contributions](https://github.com/ZimengXiong/PancakeSlicer/compare) (we are looking to extend our printer compatibility list) or [Sponsorships](https://github.com/sponsors/ZimengXiong) are welcome and appreciated.


## Table of Contents

- [Install](#install)
- [Supported Printers](#supported-printers)
- [Non-planar](#non-planar)
- [Self-hostable](#self-hostable)
- [Dynamic Profiles](#dynamic-profiles)
- [A familiar user experience for users and developers](#a-familiar-user-experience-for-users-and-developers)
- [Other Features](#other-features)
- [Too long, you probably shouldn't read](#too-long-you-probably-shouldnt-read)

## Install

### Desktop
Download the latest Windows, macOS, or Linux build from [Releases](https://github.com/ZimengXiong/PancakeSlicer/releases/latest).

PancakeSlicer is not signed for Mac (yet), you should run `xattr -dr com.apple.quarantine /Applications/PancakeSlicer.app` after installing and before first launch.

### Docker
Running via compose file is recommended (see [`deploy/docker-compose.yml`](deploy/docker-compose.yml), or [`deploy/docker-compose.proxy.yml`](deploy/docker-compose.proxy.yml) for automatic HTTPS with Caddy). Native `linux/amd64` and `linux/arm64` images are on Docker Hub (`zimengxiong/pancakeslicer`) and GHCR (`ghcr.io/zimengxiong/pancakeslicer`).

```yaml
services:
  pancakeslicer:
    image: zimengxiong/pancakeslicer:latest
    restart: unless-stopped
    ports:
      - "6868:6868"
    environment:
      ORCA_WEB_PRINTER_NETWORK: lan
      # ORCA_WEB_BEARER_TOKEN: change-me
    volumes:
      - pancakeslicer-data:/data

volumes:
  pancakeslicer-data:
```

Run `docker compose up -d` and open `http://<host>:6868`. Full Docker setup and env vars live at [pancake.cooking/self-hosted](https://pancake.cooking/self-hosted/).

## Supported Printers
Printers listed below have their non-planar paths checked against a 3D model of their print head for collisions (54 bundled head models across 32 printer families, nozzle variants, and test heads). Printers not listed here can still slice flat or non-planar, falling back to a generic (oversized) head that caps curve angles, or you can dial in a custom head with the 8-cone head-clearance calibration print (`Calibration > Non-planar > Head clearance`).

> [!CAUTION]
> Non-Planar in PancakeSlicer is experimental. **Use common sense and review the G-Code preview.** We recommend you only use non-planar under supervision (cartesian printers are better for visibility sake).

| Brand | Printers | Curved tops |
|---|---|---|
| Bambu Lab | A1, A1 mini, A2L, P1P, P1S, P2S, X1, X1 Carbon, X1E, H2S | ✅ |
| Bambu Lab | H2D, H2D Pro, H2C, X2D | ⚠️ very limited/not recommended (second nozzle drastically limits clearance) |
| Prusa | MK4, MK4S, MK4S HF, MK4 MMU3, MK4 MMU3 HF, CORE One (incl. HF / MMU3) | ✅ |
| Prusa | MINI, MINI IS, MK3S | ⚠️ very limited/not recommended (probe sits ~1.5 mm above the nozzle) |
| Creality | Ender-3 V3, V3 Plus, V3 KE, K1, K1C, K1 SE, K1 Max (incl. CFS-C), K2, K2 Plus, K2 Pro, K2 SE | ✅ |
| Voron | 0.1, 2.4, Trident, Switchwire (Stealthburner) | ✅ |
| RatRig | V-Core 3, V-Core 4, V-Core 4 HYBRID | ✅ |
| Sovol | SV06, SV06 Plus, SV06 ACE, SV06 Plus ACE, SV08, SV08 MAX | ✅ |
| Anycubic | Kobra 2 (Neo / Plus / Pro / Max), Kobra 3, Kobra 3 V2, Kobra 3 Max | ✅ |
| Elegoo | Centauri Carbon, Neptune 4, 4 Plus, 4 Pro, 4 Max | ✅ |
| Flashforge | Adventurer 5M, 5M Pro, A5, AD5X, Creator 5, Creator 5 Pro, Guider4, Guider4 Pro | ✅ |
| Qidi | Q1 Pro, X-Max 3 | ✅ |
| Snapmaker | U1 | ✅ |

## Non-Planar

Accessible non-planar slicing is the main goal of this project (everything else surrounding aims to just make this experience pleasant).

https://github.com/user-attachments/assets/be7bebe1-bed5-4b60-bb04-9381344389ad

Right now PancakeSlicer supports Flat layers, Curved tops, Smooth morph, Slant (experimental), Conformal layers (experimental), and Auto, plus Z contour and non-planar waves. You can read how the non-planar modes and print-head collision checking work at [pancake.cooking/blog/how-non-planar-slicing-works](https://pancake.cooking/blog/how-non-planar-slicing-works/), watch the [yt video](https://youtu.be/cRSoVq_reGQ), or poke around the pre-sliced sample in the [demo](https://demo.pancake.cooking/):

<img width="3840" height="2160" alt="image" src="https://github.com/user-attachments/assets/341f383d-551d-4d7f-a7bc-3c459c9d09ef" />

## Self-hostable
<img width="6024" height="4024" alt="ipad_image_btw" src="https://github.com/user-attachments/assets/b14fbeb9-f755-4a1c-acb1-b47890c3da86" />

You can self-host PancakeSlicer as a native web app (no VNC or remote-desktop streaming, works cleanly on desktop browsers and iPadOS)!

This means:
- your entire makerspace/homelab/wtv can have ONE central slicer and ONE set of profiles and configuration
  - so you don't need to sync between computers
  - and so your family members or friends can use your printers remotely without needing to log into your Bambu account
- you don't need to individually setup remote access for all of your printers
  - just proxy PancakeSlicer and you can send straight to Bambu Lab (LAN), Klipper (Moonraker), OctoPrint, PrusaLink, Duet, Flashforge, Elegoo Link, and Repetier Server from anywhere
    - I have my instance behind SSO on my domain, e.g. `slice.your-homelab.com` (built-in auth is a single shared `ORCA_WEB_BEARER_TOKEN`)
  - this is the first thing that has made me switch off of Bambu Connect, I run all my printers `LAN Only` now.

It imports STL, OBJ, 3MF, and STEP, and exports G-code, Bambu `.gcode.3mf`, project `.3mf`, and STL.

## Dynamic Profiles
<p width="100%" align="center">
  <img src="https://github.com/user-attachments/assets/08a25910-3786-46e2-821f-f12725ff9685" width="49%" alt="">
  <img src="https://github.com/user-attachments/assets/da0b5ba5-5b79-4141-94e8-9dec7a5a6565" width="49%" alt="">
</p>
<p align="center"><sub><i>(left: PancakeSlicer; right: BambuStudio)</i></sub></p>

One of the major things that I wanted to get right in a slicer are profiles. Profile management has largely remained the same since Slic3r. Slicer profiles have been forever tangled as a result.

Here are some things that I find horrible UX with traditional slicers:

- They reveal by default all the "System Profiles," including the ones you never use (for me, that's all the 0.12, 0.15, and 0.18mm profiles)
- Why are layer heights coupled with strength and speed settings? **Heck, why are profiles even coupled with printers?**
  - To add on, as there is a profile for every single layer height AND nozzle size AND printer, settings drift. Let's say you have a preferred infill type, you change it on the 0.20mm profile, but then you want to use 0.24mm, or you want to change to another printer, or both! So you switch to your 0.24mm @BRAND \<Printer\> \<Nozzle Size\> profile, and then you have to reconfigure the infill again.
  - This is because there is no concept of "Intent" or "Modifiers". If you want a "Structural" setup (5 walls, 40% gyroid infill) or a "Draft" setup, you have to bake that into a specific layer-height profile
- You don't want to see the implementation mess that is string matching and profile management in the application, you can only imagine how tangled it all is.

I think the solution is quite obvious, rethink what profiles should be: they should be settings you *care* about and want to configure. This includes things like infill, perimeters, etc, etc. By default, PancakeSlicer only has four profiles:

- Draft
- Everyday
- Fine detail
- Strong

Before we dive into dynamic profiles, I can't help but mention how much better this profile management UI is (I bet you didn't even know your slicer had one)

<img width="1600" height="891" alt="image" src="https://github.com/user-attachments/assets/36b95b56-3455-4fac-86af-6bb027110529" />

What's special about these profiles is that they are, wait for it, ***dynamic*.** They work across all your printers, nozzles, and layer heights. We can do this because the effects that nozzle size and layer height have on print settings (i.e. line widths) are predictable and can be formulaically calculated.

They are pretty much just regular profiles that just have certain fields calculated based on nozzle size, layer height, and certain printer-specific overrides (for things like speed). This reduces the amount of profiles by some factor as well as reducing cognitive load in not having to grapple with a billion profiles and make you actually want to care about them.

<img width="413" height="214" alt="image" src="https://github.com/user-attachments/assets/69ff399c-4a32-4f07-891a-088ef02028d5" />

## A familiar user experience for users and developers
Because PancakeSlicer is built for the web, it means we follow web standards for UX that you are accustomed to. Slicer UIs have been written in C++ using the wxWidgets framework to build native-looking applications for the better part of two decades. It wasn't until RECENTLY that you could even COPY AND PASTE code into the custom G-code section, as a matter of fact, this is what that UI looks like right now:

<img width="1600" height="1540" alt="image" src="https://github.com/user-attachments/assets/a4042bb4-44a2-4cbb-a4b8-2d1719ff90e6" />

This is just…unacceptable (and that's not me shrinking the text boxes, their size literally cannot be adjusted, they are ALWAYS that cramped). Text boxes have been resizable✓ (without ANY developer configuration) on the web since 2010. Here are some other things that are still broken (try them yourself!)

- lack of Undo/Redo in text boxes✓
- cannot reliably ESC from modals, dropdowns, popups, overlays✓
- zero syntax highlighting
- no validation (not even syntax) for things like Machine G-code and post-processing scripts, you only get error messages at slice time
- inconsistent TAB traversal✓

✓=solved by web standards

Building our UI on web standards solves 4 out of 7 issues above outright, and makes the remaining ones much easier to approach. Also not to mention, developers get live-reloading dev servers (which I think is what is limiting the amount of effort developers want to put in on UI for things like OrcaSlicer)

If we want to iterate on slicers faster (which I think will be inevitable, we are still SO early in what can be done with additive manufacturing), I think we will have to leave C++ UI behind. For reference, here is what G-code editing looks like in PancakeSlicer:

<img width="1600" height="891" alt="image" src="https://github.com/user-attachments/assets/877d6564-8c2a-4f4a-96ab-11346d24912d" />

## Other Features
You can play around with them in the [demo](https://demo.pancake.cooking/).

1/ Centralized manager for filaments, printers, and profiles
<img width="1600" height="891" alt="image" src="https://github.com/user-attachments/assets/dee93343-2ab5-4c33-b51a-380d9d72e097" />

2/ Side-by-side slicing and comparison with surface error estimation
<img width="1600" height="1005" alt="image" src="https://github.com/user-attachments/assets/42ee6c93-28a0-4593-9755-8494976674e9" />

3/ Version history!
<img width="1600" height="823" alt="image" src="https://github.com/user-attachments/assets/06d1cff7-a5ae-45cf-82cc-1cc44a8e3e31" />

4/ Expansive drop downs for printers, profiles, and filaments
<img width="1600" height="1163" alt="image" src="https://github.com/user-attachments/assets/9700a388-aa25-46de-8310-088ef02028d5" />

## Too long, you probably shouldn't read

### Great Expectations

I have daily driven PancakeSlicer exclusively for all my printing, mostly single nozzle, single extruder, and single filament. 3MF import/export, STEP files, multi-plate projects, Bambu AMS, and toolchangers are all in there now (though I still print mostly single-material myself, so multi-nozzle and multi-plate workflows haven't seen nearly as many hours on my own machines).

### Electron and other Technicals
To the anti-Electron crowd, I am sorry to disappoint you. I think it would be interesting to experiment with a slicer built in Swift and work natively on iPad and maybe even iPhone (Duo?), and a couple of agents would easily do it in a reasonable time span (if anyone wants to take a crack at it, I would love to see it), but I really wanted to maintain one set of UI. Then you may ask, why not Tauri? Rust! Native browser! All the buzzwords oh my. But for a 3D slicer viewport and a web-native stack, Chromium just wins on graphics and WebGL and performance and consistent font rendering and off-thread canvas rendering and the fact that our entire frontend is already built for the web. Electron lets us ship the exact application code everywhere, and I prefer consistency over individually tailoring to native apps for complex interfaces.

Our engine is built on top of OrcaSlicer's slicing engine, written in C++. This allows us to easily merge slicing improvements from upstream when necessary.

Finally, the actual service is written in Golang, managing projects, 3D models, drafts, printer profiles, preferences, and send history. We spin up a slicing engine (max 2 concurrently by default via `ORCA_WEB_MAX_JOBS`) on demand. These engines are isolated child processes with their own working directory, and stream progress to the browser via SSE (full architecture write-up at [pancake.cooking/blog/self-hosted-orcaslicer-architecture](https://pancake.cooking/blog/self-hosted-orcaslicer-architecture/)). We chose not to run the C++ engine inside of the Go service with something like CGo (I had the idea and asked some agents to go (haha, get it) implement it) because it introduced severe crash risks (a LOT of stalls during development) and shared engine mutations. Moving them to child processes increased slice times on average by only ~0.2s due to cold starts. Isolating the engine is increasingly important as we will continue to make major changes to the core slicing engine, and we don't want segfaults or memory spikes killing the entire service.

Here were the UI mockups I did :) and some BTS. Enjoy.
<img width="1600" height="943" alt="image" src="https://github.com/user-attachments/assets/7ea30137-ba8c-4f39-867d-f3beee2aa945" />
<img width="828" height="584" alt="image" src="https://github.com/user-attachments/assets/8dc6b38e-131b-4329-b8ee-61443cb201a9" />
<img width="820" height="648" alt="image" src="https://github.com/user-attachments/assets/7c1b212f-02e4-4b2a-9e3b-fd66049854fc" />
<img width="556" height="745" alt="image" src="https://github.com/user-attachments/assets/d1986b74-42f3-4ad9-ac9c-97692a8fcfce" />

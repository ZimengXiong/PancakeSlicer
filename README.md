<img width="4000" height="1570" alt="file-fb400ae5d364dd356a93be6d37d2c888" src="https://github.com/user-attachments/assets/f6981d62-8efd-4ead-937a-9fa43abb82ce" />

PancakeSlicer is a Self-hostable and Non-planar 3D Printer Slicer built on the OrcaSlicer Engine. Try a demo [here](https://demo.pancake.cooking/)! Watch the video explainer [here (yt)](https://youtu.be/cRSoVq_reGQ). 

Everything is a WIP. Come talk about Non Planar (or PancakeSlicer issues) on [Discord](https://discord.gg/tyfAV6R7DW). [Contributions](https://github.com/ZimengXiong/PancakeSlicer/compare) (we are looking to extend our comptability list) or [Sponsorships](https://github.com/sponsors/ZimengXiong) are welcome and appreciated


## Table of Contents

- [Install](#install)
- [Supported Printers](#supported-printers)
- [Non-planar](#non-planar)
- [Self-hostable](#self-hostable)
- [Dynamic Profiles](#dynamic-profiles)
- [Other Features](#other-features)
- [A familiar user experience for users and developers](#a-familiar-user-experience-for-users-and-developers)
- [Too long, you probably shouldn't read](#too-long-you-probably-shouldnt-read)

## Install
### Desktop
Download the latest build from [Releases](https://github.com/ZimengXiong/PancakeSlicer/releases/latest).

PancakeSlicer is not signed for Mac (yet), you should run  `xattr -dr com.apple.quarantine /Applications/PancakeSlicer.app` after installing and before first launch

### Docker
Running via compose file is recommended. See [`deploy/docker-compose.yml`](deploy/docker-compose.yml). An image is also avail. at `ghcr.io/zimengxiong/pancakeslicer`.

## Supported Printers
Printers listed below have their non-planar paths checked against a model of their print head for collisons. Printers not listed here are still able to slice, but they're checked against a generic (oversized) head, which may limit non-planarness and be incorrect (watch out for collisions!).

> [!CAUTION]
> Non-Planar in PancakeSlicer is experimental. We make no guarantees that, even when listed below, that this WON'T cause PERMANENT DAMAGE to your printer. **Use common sense and review the G-Code preview.** We recommend you only use non-planar under supervision (cartesian printers are better for visiblity sake).

| Brand | Printers | Curved tops |
|---|---|---|
| Bambu Lab | A1, A1 mini, A2L, P1P, P1S, P2S, X1, X1 Carbon, X1E, H2S | ✅ |
| Bambu Lab | H2D, H2D Pro, H2C, X2D | ⚠️ very limited/not recommended (second nozzle drastically limits clearance) |
| Prusa | MK4, MK4S, CORE One (incl. HF / MMU3) | ✅ |
| Prusa | MINI, MINI IS, MK3S | ⚠️ very limited/not recommended (probe sits 1.5 mm above the nozzle) |
| Creality | Ender-3 V3, V3 Plus, V3 KE, K1, K1C, K1 SE, K1 Max, K2, K2 Plus, K2 Pro, K2 SE | ✅ |
| Voron | 0.1, 2.4, Trident, Switchwire (Stealthburner) | ✅ |
| RatRig | V-Core 3, V-Core 4, V-Core 4 HYBRID | ✅ |
| Sovol | SV06, SV06 Plus, SV06 ACE, SV06 Plus ACE, SV08, SV08 MAX | ✅ |
| Anycubic | Kobra 2 (Neo / Plus / Pro / Max), Kobra 3, Kobra 3 V2, Kobra 3 Max | ✅ |
| Elegoo | Neptune 4, 4 Plus, 4 Pro, 4 Max | ✅ |
| Flashforge | Adventurer 5M, 5M Pro, A5, AD5X, Creator 5, Creator 5 Pro, Guider4, Guider4 Pro | ✅ |
| Qidi | Q1 Pro, X-Max 3 | ✅ |

## Non-Planar

Accessible non-planar slicing is the main goal of this project (everything else surrounding aims to just make this experience pleasant).

https://github.com/user-attachments/assets/be7bebe1-bed5-4b60-bb04-9381344389ad

Currently, we have support for four modes, you can play around with all of them in the [demo](https://demo.pancake.cooking/) (I'm wayy too tired and I don't want to capture more screenshots so please watch the yt video or play with the demo sorry maybe sometime soon I'll update this section):

<img width="3840" height="2160" alt="image" src="https://github.com/user-attachments/assets/341f383d-551d-4d7f-a7bc-3c459c9d09ef" />

## Self-hostable
<img width="6024" height="4024" alt="ipad_image_btw" src="https://github.com/user-attachments/assets/b14fbeb9-f755-4a1c-acb1-b47890c3da86" />

You can self-host PancakeSlicer! 

This means:
- your entire makerspace/homelab/wtv can have ONE central slicer and ONE set of profiles and configuration
  - so you don't need to sync between computers
  - and so your family members or friends can use your printers remotely without needing to log into your Bambu account
  - opens some cool possibilities with OIDC user scoping
- you don't need to individually setup remote access for all of your printers
  - just proxy PancakeSlicerand you can enjoy all your printers from anywhere.
    - I have my instance behind OIDC on my domain, e.g. `slice.your-homelab.com` 
  - this is the first thing that has made me switch off of Bambu Connect, I run all my printers `LAN Only` now.)

## Dynamic Profiles
<p width="100%" align="center">
  <img src="https://github.com/user-attachments/assets/08a25910-3786-46e2-821f-f12725ff9685" width="49%" alt="">
  <img src="https://github.com/user-attachments/assets/da0b5ba5-5b79-4141-94e8-9dec7a5a6565" width="49%" alt="">
</p>
<p align="center"><sub><i>(left: PancakeSlicer; right: BambuStudio)</i></sub></p>

One of the major things that I wanted to get right in a slicer are profiles. Profile management has largely remained the same since Slic3r. Slicer profiles have been forever tangled as a result.

Here are some things that I find horrible UX with traditional slicers:

- They reveal by default all the "System Profiles," including the ones you never use (for me, that's all the 0.12, 0.15, and 0.18mm profiles)
- Why are layer heights coupled with strength and speed settings? **Heck, why are profiles even coupled with printers?†**
  - To add on, as there is a profile for every single layer height AND nozzle size AND printer, settings drift. Let's say you have a preferred infill type, you change it on the 0.20mm profile, but then you want to use 0.24mm, or you want to change to another printer, or both! So you switch to your 0.24mm @BRAND \<Printer\> \<Nozzle Size\> profile, and then you have to reconfigure the infill again.
  - This is because there is no concept of "Intent" or "Modifiers". If you want a "Structural" setup (5 walls, 40% gyroid infill) or a "Draft" setup, you have to bake that into a specific layer-height profile
- You don't want to see the implementation mess that is string matching and profile management in the application, you can only imagine how tangled it all is.

I think the solution is quite obvious, rethink what profiles should be: they should be settings you *care* about and want to configure. This includes things like infill, perimeters, etc, etc. By default, PancakeSlicer only has four profiles:

- Draft
- Everyday
- Fine Detail
- Strong

Before we dive into dynamic profiles, I can't help but mention how much better this profile management UI is (I bet you didn't even know your slicer had one)

<img width="1600" height="891" alt="image" src="https://github.com/user-attachments/assets/36b95b56-3455-4fac-86af-6bb027110529" />

What's special about these profiles is that they are, wait for it, ***dynamic*.** They work across all your printers, nozzles, and layer heights. We can do this because the effects that nozzle size and layer height have on print settings (i.e. line widths) are predictable and can be formulaically calculated.

They are pretty much just regular profiles that just have certain fields calculated based on nozzle size, layer height, and certain printer-specific overrides (for things like speed). This reduces the amount of profiles by some factor as well as reducing cognitive load in not having to grapple with a billion profiles and make you actually want to care about them.

<img width="413" height="214" alt="image" src="https://github.com/user-attachments/assets/69ff399c-4a32-4f07-891a-088ef02028d5" />

# A familiar user experience for users and developers
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
<img width="1600" height="1163" alt="image" src="https://github.com/user-attachments/assets/9700a388-aa25-46de-8310-b629a44e2ade" />

## Too long, you probably shouldn't read

### Great Expectations

I have daily driven PancakeSlicer for the past month and have used it exclusively for all my printing. The best analogy I can give is that it covers everything in Prusa/SuperSlicer circa 2021 (everything you could do on a  single nozzle, single extruder, and single filament).

The way I use slicers has not changed much since then, I do not utilize many of the features in Orca/Bambu. There are some things that I don't understand fully and have chose not to implement:

#### 1. 3MFs

I really don't like 3MFs, they import with profiles and mess up all my settings and printers and filaments. B/c I don't have enough experience. I have decided to not include support for them. I get that they may be useful for multicolor printing folks where you want some configuration baked in, but I think profiles are a bit too far? I think we will support filament import/mapping + object modifiers and things like support painting, but not allow profile or printer changes.

#### 2. Multi-nozzle printing

I have not tested any multi-nozzle printing (I don't have any use cases for them, I don't print multicolor on my H2D, I just like having it for when I print TPU to avoid having to reload filament). All printers operate in single-extruder mode (one extruder per print only) currently.

The following are implemented with best effort, but I have not done extensive testing on them:

1. Multiple plates within a project
2. Exporting as 3MF
3. Exporting Presets

### Electron and other Technicals
To the anti-Electron crowd, I am sorry to disappoint you. I think it would be interesting to experiment with a slicer built in Swift and work natively on iPad and maybe even iPhone (Duo?), and a couple of agents would easily do it in a reasonable time span (if anyone wants to take a crack at it, I would love to see it), but I really wanted to maintain one set of UI. Then you may ask, why not Tauri? Rust! Native browser! All the buzzwords oh my. But for a 3D slicer viewport and a web-native stack, Chromium just wins on graphics and WebGL and performance and consistent font rendering and off-thread canvas rendering and the fact that our entire frontend is already built for the web. Electron lets us ship the exact application code everywhere, and I prefer consistency over individually tailoring to native apps for complex interfaces.

Our engine is built on top of OrcaSlicer's slicing engine, written in C++. This allows us to easily merge slicing improvements from upstream when necessary.

Finally, the actual service is written in Golang, managing projects, 3D models, drafts, printer profiles, preferences, and send history. We spin up a slicing engine (max 2 concurrently) on demand. These engines are isolated child processes with their own working directory, and stream progress to the browser via SSE. We chose not to run the C++ engine inside of the Go service with something like CGo (I had the idea and asked some agents to go (haha, get it) implement it) because it introduced severe crash risks (a LOT of stalls during development) and shared engine mutations. Moving them to child processes increased slice times on average by only ~0.2s due to cold starts. Isolating the engine is increasingly important as we will continue to make major changes to the core slicing engine, and we don't want segfaults or memory spikes killing the entire service.

Here were the UI mockups I did :) and some BTS. Enjoy.
<img width="1600" height="943" alt="image" src="https://github.com/user-attachments/assets/7ea30137-ba8c-4f39-867d-f3beee2aa945" />
<img width="828" height="584" alt="image" src="https://github.com/user-attachments/assets/8dc6b38e-131b-4329-b8ee-61443cb201a9" />
<img width="820" height="648" alt="image" src="https://github.com/user-attachments/assets/7c1b212f-02e4-4b2a-9e3b-fd66049854fc" />
<img width="556" height="745" alt="image" src="https://github.com/user-attachments/assets/d1986b74-42f3-4ad9-ac9c-97692a8fcfce" />





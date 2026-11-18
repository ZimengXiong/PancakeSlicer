export const MODE_LABELS: Record<string, string> = {
  off: 'Flat layers', auto: 'Auto', manual: 'Manual curved settings', oneshot: 'One-shot field', nonplanar_top: 'Curved tops', smooth_morph: 'Smooth morph', slant: 'Slant',
  conformal_layers: 'Conformal layers',
};
export const MODE_HELP: Record<string, string> = {
  off: 'Flat layers: classic horizontal layers from bottom to top. Fastest to slice and print.',
  auto: 'Auto: tests different slicing styles on each part and picks the cleanest result.',
  nonplanar_top: 'Curved tops: smoothly curves the top layers (3 by default) over gentle slopes where the print head has room.',
  smooth_morph: 'Smooth morph: gradually blends layers from flat at the bed to curved at the top. Great for domes and lids (up to 1.4× print time).',
  conformal_layers: 'Conformal layers (experimental): top shells follow every gentle curve at an even thickness, all in one unbroken layer.',
  slant: 'Slant (experimental): starts with a flat first layer, then tilts each layer a bit more up to your print head\'s limit.',
};

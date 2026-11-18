import type { Config, ConfigValue } from '../api/types';
import { sameValue } from '../state/value-equality';
import { OPTIONS } from '../options';

export const PRESET_META = /^(inherits|name|from|version|type|setting_id|base_id|filament_id|is_custom_defined|instantiation|\w+_settings_id)$/;

export const changedKeys = (values: Config, below: Config): Config => Object.fromEntries(
  Object.entries(values).filter(([key, value]) => !PRESET_META.test(key) && !sameValue(key, value, below[key] ?? OPTIONS[key]?.default as ConfigValue | undefined)),
);

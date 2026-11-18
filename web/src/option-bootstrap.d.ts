declare module 'virtual:option-bootstrap' {
  export const quick: Record<string, { type: string; mode: string; enum_values: unknown[] }>;
  export const vectors: Record<string, string>;
  export const removed: string[];
}

declare module 'virtual:option-hints' {
  export const hints: Record<string, { type: string; gui_type?: string; multiline?: boolean; full_width?: boolean }>;
}

declare module 'virtual:demo-resolver' {
  export const wasm: string;
  export const exec: string;
}

declare const __BUILD__: string;

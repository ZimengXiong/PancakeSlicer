import type { CSSProperties } from 'react';

export function Skeleton({ width, height, block = false, className = '', style }: { width?: CSSProperties['width']; height?: CSSProperties['height']; block?: boolean; className?: string; style?: CSSProperties }) {
  return <span className={`${block ? 'skel-block' : 'skel-line'}${className ? ` ${className}` : ''}`} aria-hidden="true" style={{ width, height, ...style }} />;
}

export function HeldText({ text, className }: { text: string; className: string }) {
  return <span className={`skel-line ${className}`} aria-hidden="true" style={{ color: 'transparent', width: 'fit-content', maxWidth: '100%' }}>{text}</span>;
}

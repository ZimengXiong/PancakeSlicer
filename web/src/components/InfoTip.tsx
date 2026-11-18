import type { ReactNode } from 'react';
import { Popover } from 'radix-ui';
import { InLayer } from './PageLayer';
export function InfoTip({ label, children, className = '' }: { label: string; children: ReactNode; className?: string }) {
  return <Popover.Root><Popover.Trigger asChild><button type="button" className={`info-tip ${className}`} aria-label={`About ${label}`}>?</button></Popover.Trigger>
    <InLayer portal={Popover.Portal}><Popover.Content className="info-pop" side="bottom" align="start" sideOffset={4} collisionPadding={8} onKeyDown={(e) => e.stopPropagation()}>{children}</Popover.Content></InLayer></Popover.Root>;
}

export function LabelTip({ label, full, children }: { label: ReactNode; full?: string; children: ReactNode }) {
  return <Popover.Root><Popover.Trigger asChild><button type="button" className="opt-label-text label-tip" data-full-text={full}>{label}</button></Popover.Trigger>
    <InLayer portal={Popover.Portal}><Popover.Content className="info-pop" side="bottom" align="start" sideOffset={4} collisionPadding={8} onKeyDown={(e) => e.stopPropagation()}>{children}</Popover.Content></InLayer></Popover.Root>;
}

export function SourceGlyph({ text }: { text?: string }) {
  return <span className="source-glyph" role="img" aria-label={text ?? 'Computed value'} data-truncated="true" data-full-text={text ?? 'Calculated automatically by this profile\'s formula'}>✦</span>;
}
